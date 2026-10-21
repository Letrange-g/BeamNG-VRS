// M8.41-M8.47: Direct3D 12 probe, foveation and eye-image effects; see d3d12_probe.hpp.
// The C COM interface gives typed vtable slots, so hooks are patched by field
// name rather than by hand-counted indices. Every hook forwards first; VRS
// state is only added on top of the game's own binding.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include "d3d12_probe.hpp"
#include "m825_gaze_hold.hpp"
#include "lens_mask.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beamng_d3d12 {
namespace {
struct Target { uint32_t width=0,height=0,format=0,samples=0; uint64_t resource=0; ID3D12Device* device=nullptr; };
struct Key {
 uint32_t width,height,format,samples; bool eye;
 bool operator==(const Key&) const = default;
};
struct KeyHash { size_t operator()(const Key& k) const {
 return (size_t(k.width)<<40)^(size_t(k.height)<<20)^(size_t(k.format)<<8)^(size_t(k.samples)<<1)^size_t(k.eye);
}};
// One shading-rate image per eye-shaped render-target size.
struct RateImage {
 ID3D12Resource* texture=nullptr; ID3D12Resource* upload=nullptr; uint8_t* mapped=nullptr;
 uint32_t tilesX=0,tilesY=0,rowPitch=0;
 std::vector<uint8_t> texels; // Last uploaded content.
 bool ready=false;            // Uploaded and in SHADING_RATE_SOURCE state.
};
using Size=std::pair<uint32_t,uint32_t>;

LogFn logFn=nullptr;
std::once_flag hookOnce;
std::atomic<bool> hooked{false},diagnostics{false},foveating{false};
UINT rtvIncrement=0;

std::shared_mutex targetsMutex; // descriptor address -> bound texture, eye images, rate images
std::unordered_map<SIZE_T,Target> targets;
std::unordered_set<uint64_t> eyeImages;
std::map<Size,RateImage> rateImages;
std::unordered_set<uint64_t> wantedSizes; // Eye-shaped sizes seen without an image yet.
std::atomic<uint32_t> eyeWidth{0},eyeHeight{0};

std::mutex statsMutex;
std::unordered_map<Key,uint64_t,KeyHash> binds;
uint64_t unknownBinds=0,omBinds=0,passBinds=0,vrsBinds=0,vrsLaterTarget=0,vrsDepthOnly=0;
unsigned frames=0,reports=0;

// Foveation device state; touched only from attach() and frame() (OpenXR frame thread).
ID3D12Device* vrsDevice=nullptr;ID3D12CommandQueue* vrsQueue=nullptr;
ID3D12CommandAllocator* uploadAllocator=nullptr;ID3D12GraphicsCommandList* uploadList=nullptr;
ID3D12Fence* uploadFence=nullptr;uint64_t uploadValue=0;
Foveation config{};
uint8_t coarsestRate=beamng_ffr::rate(4,4); // Lens corners; 2x2 without additional rates.
std::mutex gazeMutex;
float gazeU[2]{.5f,.5f},gazeV[2]{.5f,.5f};uint32_t gazeMask=0;uint64_t gazeTick=0;
float heldU[2]{.5f,.5f},heldV[2]{.5f,.5f}; // Gaze-held centers of the current patterns, for the outline.
beamng_gaze_hold::State hold[2]{};

// M8.43 effects. Set once in attach(); the device and queue are AddRef'd.
Effects effects{};
std::atomic<bool> cropping{false},finishing{false};
ID3D12Device* fxDevice=nullptr;ID3D12CommandQueue* fxQueue=nullptr;
// Game scissors per command list, so a later eye-target bind can clip them.
// assumed: no game scissor was seen on this list, so the crop itself was set as its scissor.
struct ListCrop { D3D12_RECT game[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}; UINT count=0; D3D12_RECT keep{}; bool active=false,assumed=false; };
std::mutex cropMutex;
std::unordered_map<ID3D12GraphicsCommandList*,ListCrop> cropLists;
std::atomic<uint64_t> croppedScissors{0},scissorCalls{0},eyeBindsWithoutScissor{0},assumedScissors{0};
// Eye-image finishing: a small ring of lists on the OpenXR queue.
struct Post { ID3D12CommandAllocator* allocator=nullptr; ID3D12GraphicsCommandList* list=nullptr; uint64_t value=0; };
std::mutex postMutex;
Post posts[6]{};ID3D12Fence* postFence=nullptr;uint64_t postValue=0;
ID3D12DescriptorHeap* postRtvs=nullptr;D3D12_CPU_DESCRIPTOR_HANDLE postRtvStart{};std::unordered_map<uint64_t,UINT> postSlots;UINT nextPostSlot=0;
std::atomic<uint64_t> finished{0},finishBusy{0};
constexpr UINT postRtvCount=16;
constexpr size_t clearBatch=4;
constexpr uint32_t lensBand=64; // Rows per black lens-corner band; taller bands mean fewer rectangles.

decltype(ID3D12DeviceVtbl::CreateRenderTargetView) nextCreateRtv=nullptr;
decltype(ID3D12DeviceVtbl::CreateDepthStencilView) nextCreateDsv=nullptr;
decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets) nextOMSetRenderTargets=nullptr;
decltype(ID3D12GraphicsCommandList4Vtbl::BeginRenderPass) nextBeginRenderPass=nullptr;
decltype(ID3D12GraphicsCommandListVtbl::RSSetScissorRects) nextRSSetScissorRects=nullptr;
decltype(ID3D12GraphicsCommandListVtbl::Reset) nextReset=nullptr;
decltype(ID3D12CommandQueueVtbl::ExecuteCommandLists) nextExecute=nullptr;

// M8.47: GPU busy time of the game's queue. Each submission is bracketed by
// two timestamps in the same ExecuteCommandLists call, so time the queue spends
// waiting for the CPU between submissions is not counted.
struct Stamp { ID3D12CommandAllocator* allocator=nullptr; ID3D12GraphicsCommandList* begin=nullptr; ID3D12GraphicsCommandList* end=nullptr; uint64_t value=0; bool pending=false,ours=false; };
constexpr UINT stampSlots=128;
Stamp stamps[stampSlots]{};UINT stampNext=0;
ID3D12CommandQueue* gameQueue=nullptr;ID3D12QueryHeap* stampHeap=nullptr;ID3D12Resource* stampReadback=nullptr;
const uint64_t* stampMapped=nullptr;ID3D12Fence* stampFence=nullptr;uint64_t stampValue=0;double ticksPerMs=0;
std::mutex stampMutex;
uint64_t busyTicks=0,ourTicks=0,submissions=0,stampSkipped=0;
unsigned gpuFrames=0,gpuReports=0;
thread_local bool ownSubmit=false; // Our uploads and eye-image effects, reported separately.
// Ctrl+Shift+V pauses VRS for an A/B in the same scene; the GPU report says which.
std::atomic<bool> vrsPaused{false};

void log(const char* msg){if(logFn)logFn(msg);}
uint64_t sizeKey(uint32_t w,uint32_t h){return (uint64_t(w)<<32)|h;}

// Same aspect ratio as the OpenXR eye image, at 12.5-100% of its size (M8.47:
// was 25%, which missed BeamNG's quarter-size 1297x1042 pass by rounding).
bool eyeShaped(uint32_t w,uint32_t h){
 const uint32_t ew=eyeWidth,eh=eyeHeight;
 if(!ew||!eh||w>ew||h>eh||uint64_t(w)*8<ew)return false;
 const double expected=double(eh)*double(w)/double(ew);
 return std::fabs(double(h)-expected)<=2.0;
}

// Shared intermediate targets serve both eyes: keep what either eye needs.
D3D12_RECT cropKeep(uint32_t w,uint32_t h){
 const auto& m=effects.margins;
 const auto k=beamng_scissor::make(w,h,{std::min(m[0].top,m[1].top),std::min(m[0].bottom,m[1].bottom),
   std::min(m[0].left,m[1].left),std::min(m[0].right,m[1].right)}).keep;
 return {LONG(k.x),LONG(k.y),LONG(k.x+k.width),LONG(k.y+k.height)};
}
// Caller holds cropMutex. Re-sends the game's scissors, clipped while active.
void applyScissors(ID3D12GraphicsCommandList* list,const ListCrop& c){
 if(!c.count)return;
 D3D12_RECT rects[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
 for(UINT i=0;i<c.count;++i){
  auto r=c.game[i];
  if(c.active){
   r.left=std::max(r.left,c.keep.left);r.top=std::max(r.top,c.keep.top);
   r.right=std::max(r.left,std::min(r.right,c.keep.right));r.bottom=std::max(r.top,std::min(r.bottom,c.keep.bottom));
  }
  rects[i]=r;
 }
 nextRSSetScissorRects(list,c.count,rects);
 if(c.active && croppedScissors.fetch_add(c.count,std::memory_order_relaxed)==0){
  static std::atomic<bool> once{false};
  if(!once.exchange(true)){char msg[200]{};
   sprintf_s(msg,"M8.43 first scissor clipped: (%ld,%ld)-(%ld,%ld) to keep (%ld,%ld)-(%ld,%ld)",
     c.game[0].left,c.game[0].top,c.game[0].right,c.game[0].bottom,c.keep.left,c.keep.top,c.keep.right,c.keep.bottom);log(msg);}
 }
}

// The first render target decides the shading-rate image for the whole bind.
// M8.46: VRS follows the first eye-shaped render target of the bind, or the
// eye-shaped depth buffer of a depth-only pass (depth prepass, alpha-tested foliage).
void bind(ID3D12GraphicsCommandList* list,SIZE_T first,UINT count,bool renderPass,const SIZE_T* all,SIZE_T depth){
 Target t{},v{};bool known=false,eye=false,eligible=false;ID3D12Resource* image=nullptr;int source=0;
 {
  std::shared_lock<std::shared_mutex> g(targetsMutex);
  if(count){
   auto it=targets.find(first);
   if(it!=targets.end()){t=it->second;known=true;eye=eyeImages.count(t.resource)!=0;}
  }
  const auto fits=[&](const Target& c){return c.samples==1 && c.device==vrsDevice && eyeShaped(c.width,c.height);};
  if(foveating){
   if(known && fits(t)){v=t;eligible=true;}
   for(UINT i=1;i<count && !eligible;++i){auto it=targets.find(all[i]);
    if(it!=targets.end() && fits(it->second)){v=it->second;eligible=true;source=1;}}
   if(!eligible && !count && depth){auto it=targets.find(depth);
    if(it!=targets.end() && fits(it->second)){v=it->second;eligible=true;source=2;}}
  }
  if(eligible && !vrsPaused.load(std::memory_order_relaxed)){
   auto r=rateImages.find({v.width,v.height});
   if(r!=rateImages.end()&&r->second.ready)image=r->second.texture;
  }
 }
 // Bundles inherit VRS state from their direct list and must not set it.
 if(foveating && ID3D12GraphicsCommandList_GetType(list)==D3D12_COMMAND_LIST_TYPE_DIRECT){
  auto* list5=reinterpret_cast<ID3D12GraphicsCommandList5*>(list);
  if(image){
   const D3D12_SHADING_RATE_COMBINER combiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_OVERRIDE};
   ID3D12GraphicsCommandList5_RSSetShadingRate(list5,D3D12_SHADING_RATE_1X1,combiners);
   ID3D12GraphicsCommandList5_RSSetShadingRateImage(list5,image);
  }else{
   ID3D12GraphicsCommandList5_RSSetShadingRateImage(list5,nullptr);
   ID3D12GraphicsCommandList5_RSSetShadingRate(list5,D3D12_SHADING_RATE_1X1,nullptr);
   if(eligible && !vrsPaused.load(std::memory_order_relaxed)){
    std::unique_lock<std::shared_mutex> g(targetsMutex);wantedSizes.insert(sizeKey(v.width,v.height));
   }
  }
 }
 // Bundles cannot set scissors; only direct lists are cropped.
 if(cropping && ID3D12GraphicsCommandList_GetType(list)==D3D12_COMMAND_LIST_TYPE_DIRECT){
  const bool target=known && t.device==fxDevice && eyeShaped(t.width,t.height);
  std::lock_guard<std::mutex> g(cropMutex);
  auto& c=cropLists[list];
  const bool was=c.active;
  c.active=target;
  if(target)c.keep=cropKeep(t.width,t.height);
  if(target && !c.count)eyeBindsWithoutScissor.fetch_add(1,std::memory_order_relaxed);
  if(c.count){c.assumed=false;if(target||was)applyScissors(list,c);}
  // BeamNG 0.40 sets its scissors through a path these hooks do not see, so
  // the crop is set right after the eye-target bind (engines set the scissor
  // after the target, which then replaces ours if they set one). Leaving the
  // eye target opens the scissor fully again.
  else if(target){nextRSSetScissorRects(list,1,&c.keep);c.assumed=true;assumedScissors.fetch_add(1,std::memory_order_relaxed);}
  else if(c.assumed){const D3D12_RECT all{0,0,D3D12_VIEWPORT_BOUNDS_MAX,D3D12_VIEWPORT_BOUNDS_MAX};nextRSSetScissorRects(list,1,&all);c.assumed=false;}
 }
 if(!diagnostics && !foveating)return;
 std::lock_guard<std::mutex> g(statsMutex);
 if(image){++vrsBinds;if(source==1)++vrsLaterTarget;else if(source==2)++vrsDepthOnly;}
 if(!diagnostics)return;
 for(UINT i=0;i<count;++i){
  Target ti{};bool k=false,e=false;
  if(i==0){ti=t;k=known;e=eye;}
  else{std::shared_lock<std::shared_mutex> g2(targetsMutex);auto it=targets.find(all[i]);
   if(it!=targets.end()){ti=it->second;k=true;e=eyeImages.count(ti.resource)!=0;}}
  ++(renderPass?passBinds:omBinds);
  if(!k){++unknownBinds;continue;}
  ++binds[Key{ti.width,ti.height,ti.format,ti.samples,e}];
 }
}

void STDMETHODCALLTYPE hookCreateRtv(ID3D12Device* device,ID3D12Resource* resource,
  const D3D12_RENDER_TARGET_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE destination){
 nextCreateRtv(device,resource,desc,destination);
 if(!resource)return;
 D3D12_RESOURCE_DESC rd{};ID3D12Resource_GetDesc(resource,&rd);
 if(rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D){
  std::unique_lock<std::shared_mutex> g(targetsMutex);targets.erase(destination.ptr);return;
 }
 UINT mip=0;
 if(desc && desc->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D)mip=desc->Texture2D.MipSlice;
 else if(desc && desc->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2DARRAY)mip=desc->Texture2DArray.MipSlice;
 Target t{};
 t.width=std::max<uint32_t>(1,uint32_t(rd.Width>>mip));t.height=std::max<uint32_t>(1,rd.Height>>mip);
 t.format=uint32_t(desc && desc->Format!=DXGI_FORMAT_UNKNOWN?desc->Format:rd.Format);t.samples=rd.SampleDesc.Count;
 t.resource=uint64_t(reinterpret_cast<uintptr_t>(resource));t.device=device;
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 targets[destination.ptr]=t;
}

void STDMETHODCALLTYPE hookOMSetRenderTargets(ID3D12GraphicsCommandList* list,UINT count,
  const D3D12_CPU_DESCRIPTOR_HANDLE* rts,BOOL singleRange,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){
 nextOMSetRenderTargets(list,count,rts,singleRange,depth);
 SIZE_T handles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
 const UINT n=rts?std::min<UINT>(count,D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT):0;
 for(UINT i=0;i<n;++i)handles[i]=singleRange?rts[0].ptr+SIZE_T(i)*rtvIncrement:rts[i].ptr;
 bind(list,handles[0],n,false,handles,depth?depth->ptr:0);
}

void STDMETHODCALLTYPE hookBeginRenderPass(ID3D12GraphicsCommandList4* list,UINT count,
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,D3D12_RENDER_PASS_FLAGS flags){
 // The shading-rate image must be set before the pass begins.
 SIZE_T handles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
 const UINT n=rts?std::min<UINT>(count,D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT):0;
 for(UINT i=0;i<n;++i)handles[i]=rts[i].cpuDescriptor.ptr;
 bind(reinterpret_cast<ID3D12GraphicsCommandList*>(list),handles[0],n,true,handles,depth?depth->cpuDescriptor.ptr:0);
 nextBeginRenderPass(list,count,rts,depth,flags);
}

void STDMETHODCALLTYPE hookRSSetScissorRects(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* rects){
 if(cropping)scissorCalls.fetch_add(1,std::memory_order_relaxed);
 if(!cropping || !rects || !count || count>D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE){
  nextRSSetScissorRects(list,count,rects);return;
 }
 std::lock_guard<std::mutex> g(cropMutex);
 auto& c=cropLists[list];
 c.count=count;c.assumed=false;std::copy(rects,rects+count,c.game);
 if(c.active)applyScissors(list,c);
 else nextRSSetScissorRects(list,count,rects);
}

HRESULT STDMETHODCALLTYPE hookReset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* state){
 const HRESULT hr=nextReset(list,allocator,state);
 if(cropping){std::lock_guard<std::mutex> g(cropMutex);cropLists.erase(list);} // A new recording starts without state.
 return hr;
}

// Vtables live in the D3D12 runtime's read-only data and are shared by every
// object of that class. Chain to whatever was installed before us.
template<class T> bool patch(const T* constSlot,T hook,T& next){
 T* slot=const_cast<T*>(constSlot);
 if(*slot==hook)return true;
 DWORD old=0;
 if(!VirtualProtect(slot,sizeof(T),PAGE_READWRITE,&old))return false;
 next=*slot;*slot=hook;
 VirtualProtect(slot,sizeof(T),old,&old);
 return true;
}

void STDMETHODCALLTYPE hookCreateDsv(ID3D12Device* device,ID3D12Resource* resource,
  const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE destination){
 nextCreateDsv(device,resource,desc,destination);
 // DSV and RTV heaps never share addresses, so both live in one map.
 D3D12_RESOURCE_DESC rd{};
 if(resource)ID3D12Resource_GetDesc(resource,&rd);
 if(!resource || rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D){
  std::unique_lock<std::shared_mutex> g(targetsMutex);targets.erase(destination.ptr);return;
 }
 UINT mip=0;
 if(desc && desc->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D)mip=desc->Texture2D.MipSlice;
 else if(desc && desc->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2DARRAY)mip=desc->Texture2DArray.MipSlice;
 Target t{};
 t.width=std::max<uint32_t>(1,uint32_t(rd.Width>>mip));t.height=std::max<uint32_t>(1,rd.Height>>mip);
 t.format=uint32_t(desc && desc->Format!=DXGI_FORMAT_UNKNOWN?desc->Format:rd.Format);t.samples=rd.SampleDesc.Count;
 t.resource=uint64_t(reinterpret_cast<uintptr_t>(resource));t.device=device;
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 targets[destination.ptr]=t;
}

// Caller holds stampMutex.
void collectStamps(){
 const uint64_t done=ID3D12Fence_GetCompletedValue(stampFence);
 for(UINT i=0;i<stampSlots;++i){auto& st=stamps[i];
  if(!st.pending || st.value>done)continue;
  st.pending=false;
  const uint64_t a=stampMapped[2*i],b=stampMapped[2*i+1];
  if(b>a && b-a<uint64_t(ticksPerMs*1000.0))(st.ours?ourTicks:busyTicks)+=b-a;
 }
}

void STDMETHODCALLTYPE hookExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
 if(queue!=gameQueue || !count || count>62){nextExecute(queue,count,lists);return;}
 std::lock_guard<std::mutex> g(stampMutex);
 if(!stampFence){nextExecute(queue,count,lists);return;}
 collectStamps();
 auto& st=stamps[stampNext];
 const UINT i=stampNext;
 if(st.pending || FAILED(ID3D12CommandAllocator_Reset(st.allocator)) ||
    FAILED(ID3D12GraphicsCommandList_Reset(st.begin,st.allocator,nullptr))){
  ++stampSkipped;nextExecute(queue,count,lists);return;
 }
 ID3D12GraphicsCommandList_EndQuery(st.begin,stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,2*i);
 ID3D12GraphicsCommandList_Close(st.begin);
 if(FAILED(ID3D12GraphicsCommandList_Reset(st.end,st.allocator,nullptr))){++stampSkipped;nextExecute(queue,count,lists);return;}
 ID3D12GraphicsCommandList_EndQuery(st.end,stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,2*i+1);
 ID3D12GraphicsCommandList_ResolveQueryData(st.end,stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,2*i,2,stampReadback,UINT64(16)*i);
 ID3D12GraphicsCommandList_Close(st.end);
 ID3D12CommandList* all[64];
 all[0]=reinterpret_cast<ID3D12CommandList*>(st.begin);
 std::copy(lists,lists+count,all+1);
 all[count+1]=reinterpret_cast<ID3D12CommandList*>(st.end);
 nextExecute(queue,count+2,all);
 if(SUCCEEDED(ID3D12CommandQueue_Signal(queue,stampFence,++stampValue))){st.value=stampValue;st.pending=true;st.ours=ownSubmit;}
 if(!ownSubmit)++submissions;
 stampNext=(stampNext+1)%stampSlots;
}

// The OpenXR binding's queue must be a direct queue, which timestamps support.
void startGpuTiming(ID3D12Device* device,ID3D12CommandQueue* queue){
 char msg[200]{};
 auto fail=[&](const char* step,HRESULT hr){
  sprintf_s(msg,"M8.47 D3D12 GPU timing WARNING: %s failed hr=0x%08lX; timing off",step,hr);log(msg);};
 UINT64 frequency=0;
 HRESULT hr=ID3D12CommandQueue_GetTimestampFrequency(queue,&frequency);
 if(FAILED(hr) || !frequency){fail("GetTimestampFrequency",hr);return;}
 D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=2*stampSlots;
 if(FAILED(hr=ID3D12Device_CreateQueryHeap(device,&qd,IID_ID3D12QueryHeap,reinterpret_cast<void**>(&stampHeap)))){fail("CreateQueryHeap",hr);return;}
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
 D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=UINT64(16)*stampSlots;bd.Height=1;
 bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(FAILED(hr=ID3D12Device_CreateCommittedResource(device,&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_ID3D12Resource,reinterpret_cast<void**>(&stampReadback)))){fail("readback buffer",hr);return;}
 void* mapped=nullptr;
 if(FAILED(hr=ID3D12Resource_Map(stampReadback,0,nullptr,&mapped))){fail("Map",hr);return;}
 // Leaked on failure: a few small objects, once per process.
 for(UINT i=0;i<stampSlots;++i){auto& st=stamps[i];
  if(FAILED(hr=ID3D12Device_CreateCommandAllocator(device,D3D12_COMMAND_LIST_TYPE_DIRECT,IID_ID3D12CommandAllocator,reinterpret_cast<void**>(&st.allocator))) ||
     FAILED(hr=ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,st.allocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&st.begin)))){fail("timestamp lists",hr);return;}
  ID3D12GraphicsCommandList_Close(st.begin); // The allocator records one list at a time.
  if(FAILED(hr=ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,st.allocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&st.end)))){fail("timestamp lists",hr);return;}
  ID3D12GraphicsCommandList_Close(st.end);
 }
 ID3D12Fence* fence=nullptr;
 if(FAILED(hr=ID3D12Device_CreateFence(device,0,D3D12_FENCE_FLAG_NONE,IID_ID3D12Fence,reinterpret_cast<void**>(&fence)))){fail("CreateFence",hr);return;}
 stampMapped=static_cast<const uint64_t*>(mapped);ticksPerMs=double(frequency)/1000.0;
 ID3D12CommandQueue_AddRef(queue);gameQueue=queue;
 if(!patch(&queue->lpVtbl->ExecuteCommandLists,decltype(ID3D12CommandQueueVtbl::ExecuteCommandLists)(&hookExecute),nextExecute)){
  fail("ExecuteCommandLists hook",E_FAIL);return;
 }
 {std::lock_guard<std::mutex> g(stampMutex);stampFence=fence;}
 log("M8.47 D3D12 GPU timing active (Ctrl+Shift+V pauses VRS for an A/B)");
}

bool install(ID3D12Device* device){
 char msg[256]{};
 rtvIncrement=ID3D12Device_GetDescriptorHandleIncrementSize(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 // The hooks outlive xrDestroyInstance, so this DLL must never unload.
 HMODULE self=nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
     reinterpret_cast<LPCWSTR>(&install),&self)){log("M8.41 D3D12 probe WARNING: cannot pin module; hooks not installed");return false;}
 ID3D12CommandAllocator* allocator=nullptr;ID3D12GraphicsCommandList* list=nullptr;
 if(FAILED(ID3D12Device_CreateCommandAllocator(device,D3D12_COMMAND_LIST_TYPE_DIRECT,IID_ID3D12CommandAllocator,reinterpret_cast<void**>(&allocator)))){
  log("M8.41 D3D12 probe WARNING: CreateCommandAllocator failed; hooks not installed");return false;
 }
 if(FAILED(ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&list)))){
  ID3D12CommandAllocator_Release(allocator);
  log("M8.41 D3D12 probe WARNING: CreateCommandList failed; hooks not installed");return false;
 }
 ID3D12GraphicsCommandList_Close(list);
 bool ok=patch(&device->lpVtbl->CreateRenderTargetView,
   decltype(ID3D12DeviceVtbl::CreateRenderTargetView)(&hookCreateRtv),nextCreateRtv);
 ok=ok&&patch(&device->lpVtbl->CreateDepthStencilView,
   decltype(ID3D12DeviceVtbl::CreateDepthStencilView)(&hookCreateDsv),nextCreateDsv);
 ok=ok&&patch(&list->lpVtbl->OMSetRenderTargets,
   decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets)(&hookOMSetRenderTargets),nextOMSetRenderTargets);
 ok=ok&&patch(&list->lpVtbl->RSSetScissorRects,
   decltype(ID3D12GraphicsCommandListVtbl::RSSetScissorRects)(&hookRSSetScissorRects),nextRSSetScissorRects);
 ok=ok&&patch(&list->lpVtbl->Reset,
   decltype(ID3D12GraphicsCommandListVtbl::Reset)(&hookReset),nextReset);
 bool renderPasses=false;
 ID3D12GraphicsCommandList4* list4=nullptr;
 if(ok && SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(list,IID_ID3D12GraphicsCommandList4,reinterpret_cast<void**>(&list4)))){
  renderPasses=patch(&list4->lpVtbl->BeginRenderPass,
    decltype(ID3D12GraphicsCommandList4Vtbl::BeginRenderPass)(&hookBeginRenderPass),nextBeginRenderPass);
  ID3D12GraphicsCommandList4_Release(list4);
 }
 ID3D12GraphicsCommandList_Release(list);ID3D12CommandAllocator_Release(allocator);
 sprintf_s(msg,"M8.41 D3D12 probe hooks=%s renderPassHook=%d rtvIncrement=%u",ok?"installed":"FAILED",int(renderPasses),rtvIncrement);
 log(msg);
 return ok;
}

// M8.45: the fraction of the full eye the image shows, so foveation sizes stay
// relative to the full eye (the lens) with the M8.44 FOV crop.
std::pair<float,float> kept(int eye){
 if(!effects.fovCrop)return {1.f,1.f};
 const auto& m=effects.margins[eye];
 return {float(std::max(.02,1.0-m.left-m.right)),float(std::max(.02,1.0-m.top-m.bottom))};
}

// VRS Tier 2 with the image tile size the patterns are built for.
bool startFoveation(ID3D12Device* device,ID3D12CommandQueue* queue,const Foveation& f){
 D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
 char msg[256]{};
 if(FAILED(ID3D12Device_CheckFeatureSupport(device,D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options)))){
  log("M8.42 D3D12 foveation unavailable: VRS feature query failed");return false;
 }
 if(options.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_2 || !options.ShadingRateImageTileSize){
  sprintf_s(msg,"M8.42 D3D12 foveation unavailable: VRS tier=%d (Tier 2 required)",int(options.VariableShadingRateTier));log(msg);return false;
 }
 if(!queue){log("M8.42 D3D12 foveation unavailable: no command queue in the OpenXR binding");return false;}
 if(FAILED(ID3D12Device_CreateCommandAllocator(device,D3D12_COMMAND_LIST_TYPE_DIRECT,IID_ID3D12CommandAllocator,reinterpret_cast<void**>(&uploadAllocator))) ||
    FAILED(ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,uploadAllocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&uploadList))) ||
    FAILED(ID3D12Device_CreateFence(device,0,D3D12_FENCE_FLAG_NONE,IID_ID3D12Fence,reinterpret_cast<void**>(&uploadFence)))){
  log("M8.42 D3D12 foveation WARNING: upload objects could not be created; rendering unchanged");return false;
 }
 ID3D12GraphicsCommandList_Close(uploadList);
 config=f;
 // 2x4/4x2/4x4 are "additional" rates; fall back to 2x2 without them.
 if(!options.AdditionalShadingRatesSupported){
  if(config.middleRate>beamng_ffr::rate(2,2))config.middleRate=beamng_ffr::rate(2,2);
  if(config.outerRate>beamng_ffr::rate(2,2))config.outerRate=beamng_ffr::rate(2,2);
  coarsestRate=beamng_ffr::rate(2,2);
 }
 ID3D12Device_AddRef(device);ID3D12CommandQueue_AddRef(queue); // Our resources must not outlive them.
 vrsDevice=device;vrsQueue=queue;
 sprintf_s(msg,"M8.42 D3D12 foveation active: tier=2 tile=%u gaze=%d center=%.2f middle=%.2f rates=%u/%u shape=%s keptEye=%.2fx%.2f",
   options.ShadingRateImageTileSize,int(config.gaze),config.center,config.middle,
   unsigned(config.middleRate),unsigned(config.outerRate),beamng_ffr::shapeName(config.shape),kept(0).first,kept(0).second);
 log(msg);
 return true;
}
UINT tileSize(){
 D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
 static UINT cached=0;
 if(!cached && vrsDevice && SUCCEEDED(ID3D12Device_CheckFeatureSupport(vrsDevice,D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options))))cached=options.ShadingRateImageTileSize;
 return cached?cached:16;
}

bool createImage(uint32_t w,uint32_t h,RateImage& out){
 const UINT tile=tileSize();
 RateImage img{};img.tilesX=(w+tile-1)/tile;img.tilesY=(h+tile-1)/tile;
 img.rowPitch=(img.tilesX+D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1)&~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1);
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=img.tilesX;rd.Height=img.tilesY;
 rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8_UINT;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
 if(FAILED(ID3D12Device_CreateCommittedResource(vrsDevice,&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_ID3D12Resource,reinterpret_cast<void**>(&img.texture))))return false;
 D3D12_HEAP_PROPERTIES upHeap{};upHeap.Type=D3D12_HEAP_TYPE_UPLOAD;
 D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=UINT64(img.rowPitch)*img.tilesY;bd.Height=1;
 bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(FAILED(ID3D12Device_CreateCommittedResource(vrsDevice,&upHeap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_ID3D12Resource,reinterpret_cast<void**>(&img.upload))) ||
    FAILED(ID3D12Resource_Map(img.upload,0,nullptr,reinterpret_cast<void**>(&img.mapped)))){
  ID3D12Resource_Release(img.texture);if(img.upload)ID3D12Resource_Release(img.upload);return false;
 }
 out=std::move(img);
 return true;
}

uint8_t d3d12Rate(uint8_t vulkan){ // Vulkan: width bits 0-1, height bits 2-3; D3D12: width bits 2-3, height bits 0-1.
 return uint8_t(((vulkan&3)<<2)|((vulkan>>2)&3));
}

// The lens ellipse in normalized eye-image coordinates. With M8.44 the image is
// the kept part of the full eye, so the ellipse moves and grows accordingly.
struct Ellipse { double cx,cy,a,b; };
Ellipse lensEllipse(int eye){
 const auto& m=effects.margins[eye];
 const double kw=effects.fovCrop?std::max(.02,1.0-m.left-m.right):1.0,kh=effects.fovCrop?std::max(.02,1.0-m.top-m.bottom):1.0;
 const double ox=effects.fovCrop?m.left:0.0,oy=effects.fovCrop?m.top:0.0;
 return {(.5-ox)/kw,(.5-oy)/kh,effects.lensSize*.5/kw,effects.lensSize*.5/kh};
}

std::vector<uint8_t> pattern(uint32_t w,uint32_t h,const float u[2],const float v[2]){
 const UINT tile=tileSize();
 beamng_ffr::Pattern eyes[2];
 for(int i=0;i<2;++i){const auto [ku,kv]=kept(i);
  eyes[i]=beamng_ffr::makeGaze(w,h,tile,tile,u[i],v[i],config.center,config.middle,
   config.middleRate,config.outerRate,config.shape,ku,kv);}
 std::vector<uint8_t> combined;beamng_ffr::combineFiner(eyes[0].texels,eyes[1].texels,combined);
 // M8.43: tiles entirely outside the lens ellipse are blacked out at the end; shade them coarsest.
 // Shared targets serve both eyes, so a tile must lie outside both eyes' ellipses.
 if(effects.lensMask){
  const uint32_t tx=(w+tile-1)/tile,ty=(h+tile-1)/tile;
  for(uint32_t y=0;y<ty;++y)for(uint32_t x=0;x<tx;++x){
   const double x0=double(x*tile)/w,x1=double(std::min(w,(x+1)*tile))/w;
   const double y0=double(y*tile)/h,y1=double(std::min(h,(y+1)*tile))/h;
   bool outside=true;
   for(int e=0;e<2&&outside;++e){
    const auto el=lensEllipse(e);
    const double dx=(std::clamp(el.cx,x0,x1)-el.cx)/el.a,dy=(std::clamp(el.cy,y0,y1)-el.cy)/el.b;
    outside=dx*dx+dy*dy>=1.0;
   }
   if(outside && size_t(y)*tx+x<combined.size())combined[size_t(y)*tx+x]=coarsestRate;
  }
 }
 for(auto& t:combined)t=d3d12Rate(t);
 return combined;
}

// Rebuilds changed images and submits one copy list on the game's OpenXR
// queue; queue order keeps every upload between frames' reads of the image.
void updateFoveation(){
 if(!vrsDevice||!uploadFence)return;
 if(ID3D12Fence_GetCompletedValue(uploadFence)<uploadValue)return; // Previous upload still in flight.
 float u[2],v[2];
 {
  std::lock_guard<std::mutex> g(gazeMutex);
  const uint64_t now=GetTickCount64();
  for(int i=0;i<2;++i){
   beamng_gaze_hold::Sample raw{};
   raw.valid=config.gaze && (gazeMask&(1u<<i)) && now-gazeTick<=250;
   raw.u=gazeU[i];raw.v=gazeV[i];
   const auto d=beamng_gaze_hold::choose(hold[i],raw,now,config.graceMs);
   u[i]=std::clamp(d.sample.u,0.f,1.f);v[i]=std::clamp(d.sample.v,0.f,1.f);
   heldU[i]=u[i];heldV[i]=v[i];
  }
 }
 std::vector<Size> create;
 {
  std::unique_lock<std::shared_mutex> g(targetsMutex);
  for(auto key:wantedSizes){Size s{uint32_t(key>>32),uint32_t(key)};if(!rateImages.count(s)&&rateImages.size()<8)create.push_back(s);}
  wantedSizes.clear();
 }
 for(const auto& s:create){
  RateImage img{};char msg[200]{};
  if(createImage(s.first,s.second,img)){
   sprintf_s(msg,"M8.42 shading-rate image for %ux%u: %ux%u tiles",s.first,s.second,img.tilesX,img.tilesY);log(msg);
   std::unique_lock<std::shared_mutex> g(targetsMutex);rateImages.emplace(s,std::move(img));
  }else{sprintf_s(msg,"M8.42 WARNING: shading-rate image for %ux%u could not be created",s.first,s.second);log(msg);}
 }
 // Only this thread changes images, so they can be read without the lock here.
 struct Job{RateImage* img;bool first;};
 std::vector<Job> jobs;
 for(auto& [size,img]:rateImages){
  std::vector<uint8_t> texels;
  try{texels=pattern(size.first,size.second,u,v);}catch(const std::exception&){continue;}
  if(img.ready && texels==img.texels)continue;
  for(uint32_t y=0;y<img.tilesY;++y)memcpy(img.mapped+size_t(y)*img.rowPitch,texels.data()+size_t(y)*img.tilesX,img.tilesX);
  img.texels=std::move(texels);
  jobs.push_back({&img,!img.ready});
 }
 if(jobs.empty())return;
 if(FAILED(ID3D12CommandAllocator_Reset(uploadAllocator))||FAILED(ID3D12GraphicsCommandList_Reset(uploadList,uploadAllocator,nullptr)))return;
 for(const auto& j:jobs){
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=j.img->texture;
  b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if(!j.first){b.Transition.StateBefore=D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
   ID3D12GraphicsCommandList_ResourceBarrier(uploadList,1,&b);}
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=j.img->texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=0;
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=j.img->upload;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset=0;src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8_UINT;
  src.PlacedFootprint.Footprint.Width=j.img->tilesX;src.PlacedFootprint.Footprint.Height=j.img->tilesY;
  src.PlacedFootprint.Footprint.Depth=1;src.PlacedFootprint.Footprint.RowPitch=j.img->rowPitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(uploadList,&dst,0,0,0,&src,nullptr);
  b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
  ID3D12GraphicsCommandList_ResourceBarrier(uploadList,1,&b);
 }
 if(FAILED(ID3D12GraphicsCommandList_Close(uploadList)))return;
 ID3D12CommandList* lists[]{reinterpret_cast<ID3D12CommandList*>(uploadList)};
 ownSubmit=true;ID3D12CommandQueue_ExecuteCommandLists(vrsQueue,1,lists);ownSubmit=false;
 ID3D12CommandQueue_Signal(vrsQueue,uploadFence,++uploadValue);
 // Game lists recorded from now on are submitted after this upload on the same queue.
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 for(const auto& j:jobs)j.img->ready=true;
}

bool startEffects(ID3D12Device* device,ID3D12CommandQueue* queue,const Effects& e){
 if(!queue){log("M8.43 D3D12 crop/lens mask/outline unavailable: no command queue in the OpenXR binding");return false;}
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=postRtvCount;
 bool ok=SUCCEEDED(ID3D12Device_CreateDescriptorHeap(device,&hd,IID_ID3D12DescriptorHeap,reinterpret_cast<void**>(&postRtvs))) &&
   SUCCEEDED(ID3D12Device_CreateFence(device,0,D3D12_FENCE_FLAG_NONE,IID_ID3D12Fence,reinterpret_cast<void**>(&postFence)));
 for(auto& p:posts){
  ok=ok&&SUCCEEDED(ID3D12Device_CreateCommandAllocator(device,D3D12_COMMAND_LIST_TYPE_DIRECT,IID_ID3D12CommandAllocator,reinterpret_cast<void**>(&p.allocator))) &&
     SUCCEEDED(ID3D12Device_CreateCommandList(device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,p.allocator,nullptr,IID_ID3D12GraphicsCommandList,reinterpret_cast<void**>(&p.list)));
  if(ok)ID3D12GraphicsCommandList_Close(p.list);
 }
 if(!ok){log("M8.43 D3D12 effects WARNING: objects could not be created; crop, lens mask and outline stay off");return false;}
 // COM methods return structs through a hidden pointer after `this`; some C
 // headers declare this one by value, so call it with the explicit form.
 using HeapStart=D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_CPU_DESCRIPTOR_HANDLE*);
 reinterpret_cast<HeapStart>(postRtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart)(postRtvs,&postRtvStart);
 effects=e;
 ID3D12Device_AddRef(device);ID3D12CommandQueue_AddRef(queue);
 fxDevice=device;fxQueue=queue;
 cropping=e.crop;finishing=e.crop||e.lensMask||e.outline;
 char msg[300]{};
 sprintf_s(msg,"M8.43 D3D12 effects active: fovCrop=%d crop=%d top=%.2f/%.2f bottom=%.2f/%.2f left=%.2f/%.2f right=%.2f/%.2f lensMask=%d size=%.2f outline=%d",
   int(e.fovCrop),int(e.crop),e.margins[0].top,e.margins[1].top,e.margins[0].bottom,e.margins[1].bottom,
   e.margins[0].left,e.margins[1].left,e.margins[0].right,e.margins[1].right,int(e.lensMask),e.lensSize,int(e.outline));
 log(msg);
 return true;
}

using Rects=std::vector<D3D12_RECT>;
void addRect(Rects& out,LONG x0,LONG y0,LONG x1,LONG y1,LONG w,LONG h){
 x0=std::max<LONG>(x0,0);y0=std::max<LONG>(y0,0);x1=std::min(x1,w);y1=std::min(y1,h);
 if(x1>x0&&y1>y0)out.push_back({x0,y0,x1,y1});
}
// Static per eye and size: cropped edges and lens corners.
const Rects& blackRects(uint32_t w,uint32_t h,int eye){
 static std::mutex m;static std::map<std::tuple<uint32_t,uint32_t,int>,Rects> cache;
 std::lock_guard<std::mutex> g(m);
 auto [it,inserted]=cache.try_emplace({w,h,eye});
 if(inserted){
  if(effects.crop)for(const auto& r:beamng_scissor::make(w,h,effects.margins[eye]).outside)
   addRect(it->second,LONG(r.x),LONG(r.y),LONG(r.x+r.width),LONG(r.y+r.height),LONG(w),LONG(h));
  // With M8.44 the image is the kept part of the full eye: build the mask for
  // the full eye and shift it into this image.
  const auto& mg=effects.margins[eye];
  const double kw=effects.fovCrop?std::max(.02,1.0-mg.left-mg.right):1.0,kh=effects.fovCrop?std::max(.02,1.0-mg.top-mg.bottom):1.0;
  const uint32_t fw=uint32_t(std::lround(w/kw)),fh=uint32_t(std::lround(h/kh));
  const LONG ox=effects.fovCrop?LONG(std::lround(mg.left*fw)):0,oy=effects.fovCrop?LONG(std::lround(mg.top*fh)):0;
  if(effects.lensMask)for(const auto& r:beamng_lens::outside(fw,fh,effects.lensSize,lensBand))
   addRect(it->second,LONG(r.x)-ox,LONG(r.y)-oy,LONG(r.x+r.width)-ox,LONG(r.y+r.height)-oy,LONG(w),LONG(h));
 }
 return it->second;
}
// Same tile classification as the shading-rate image the scene is rendered with
// (the smallest eye-shaped one, normally the FSR input), scaled to the eye image.
void outlineRects(uint32_t w,uint32_t h,int eye,float cu,float cv,float size,Rects& out){
 if(size<=0.f)return;
 const auto [ku,kv]=kept(eye);
 uint32_t sw=w,sh=h;
 {std::shared_lock<std::shared_mutex> g(targetsMutex);
  for(const auto& [s,img]:rateImages)if(img.ready && s.first<sw){sw=s.first;sh=s.second;}}
 const uint32_t tile=tileSize(),cols=(sw+tile-1)/tile,rows=(sh+tile-1)/tile;
 const double sx=double(w)/sw,sy=double(h)/sh;
 const LONG thick=std::max<LONG>(2,LONG(std::lround(1.5*sx)));
 const auto inside=[&](int x,int y){
  if(x<0||y<0||x>=int(cols)||y>=int(rows))return false;
  return beamng_ffr::inside(beamng_ffr::tileUV(uint32_t(x),tile,sw),beamng_ffr::tileUV(uint32_t(y),tile,sh),cu,cv,sw,sh,size,config.shape,ku,kv);
 };
 const float halfV=.5f*size/kv,halfU=config.shape==beamng_ffr::Shape::Ellipse?.5f*size/ku:halfV*float(sh)/float(sw);
 const int x0=std::max(0,int((cu-halfU)*cols)-2),x1=std::min(int(cols),int((cu+halfU)*cols)+3);
 const int y0=std::max(0,int((cv-halfV)*rows)-2),y1=std::min(int(rows),int((cv+halfV)*rows)+3);
 for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
  if(!inside(x,y))continue;
  const LONG l=LONG(std::lround(x*tile*sx)),t=LONG(std::lround(y*tile*sy));
  const LONG r=std::min(LONG(w),LONG(std::lround((x+1)*tile*sx))),b=std::min(LONG(h),LONG(std::lround((y+1)*tile*sy)));
  if(!inside(x-1,y))addRect(out,l,t,l+thick,b,LONG(w),LONG(h));
  if(!inside(x+1,y))addRect(out,r-thick,t,r,b,LONG(w),LONG(h));
  if(!inside(x,y-1))addRect(out,l,t,r,t+thick,LONG(w),LONG(h));
  if(!inside(x,y+1))addRect(out,l,b-thick,r,b,LONG(w),LONG(h));
 }
}
}

bool attach(void* device,void* queue,LogFn fn,bool diag,const Foveation& f,const Effects& e){
 if(!device || (!diag && !f.enabled && !e.any()))return false;
 auto* d=static_cast<ID3D12Device*>(device);
 std::call_once(hookOnce,[&]{logFn=fn;hooked=install(d);});
 if(!hooked)return false;
 if(diag && !diagnostics){
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};char msg[200]{};
  if(SUCCEEDED(ID3D12Device_CheckFeatureSupport(d,D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options))))
   sprintf_s(msg,"M8.41 D3D12 VRS tier=%d tileSize=%u perPrimitiveViewports=%d additionalRates=%d",
     int(options.VariableShadingRateTier),options.ShadingRateImageTileSize,
     int(options.PerPrimitiveShadingRateSupportedWithViewportIndexing),int(options.AdditionalShadingRatesSupported));
  else sprintf_s(msg,"M8.41 D3D12 VRS tier=unknown (OPTIONS6 query failed)");
  log(msg);diagnostics=true;
 }
 static std::once_flag timingOnce;
 if(queue)std::call_once(timingOnce,[&]{startGpuTiming(d,static_cast<ID3D12CommandQueue*>(queue));});
 // Effects first: the lens corners are part of the first shading-rate images.
 if(e.any() && !fxDevice)startEffects(d,static_cast<ID3D12CommandQueue*>(queue),e);
 // One VRS device per process; a later session on another device keeps rendering unchanged.
 if(f.enabled && !vrsDevice)foveating=startFoveation(d,static_cast<ID3D12CommandQueue*>(queue),f);
 else if(f.enabled && vrsDevice!=d)log("M8.42 D3D12 foveation WARNING: new D3D12 device; VRS stays on the first device until restart");
 return true;
}

void setEyeImages(const uint64_t* images,size_t count){
 if(!hooked||!images)return;
 {std::shared_lock<std::shared_mutex> g(targetsMutex);bool known=true;
  for(size_t i=0;i<count;++i)if(images[i] && !eyeImages.count(images[i]))known=false;
  if(known)return;}
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 for(size_t i=0;i<count;++i)if(images[i])eyeImages.insert(images[i]);
}

void setEyeSize(uint32_t width,uint32_t height){
 eyeWidth=width;eyeHeight=height;
}

void setGaze(const float u[2],const float v[2],uint32_t mask){
 if(!foveating)return;
 std::lock_guard<std::mutex> g(gazeMutex);
 for(int i=0;i<2;++i){gazeU[i]=u[i];gazeV[i]=v[i];}
 gazeMask=mask;gazeTick=GetTickCount64();
}

namespace {
void finishEyeImageImpl(uint64_t texture,int eye,uint32_t format){
 auto* resource=reinterpret_cast<ID3D12Resource*>(uintptr_t(texture));
 D3D12_RESOURCE_DESC rd{};ID3D12Resource_GetDesc(resource,&rd);
 static std::atomic<bool> described{false};
 if(!described.exchange(true)){char msg[220]{};
  sprintf_s(msg,"M8.43 first eye image release: eye=%d %llux%u format=%u/%u flags=0x%X samples=%u",
    eye,(unsigned long long)rd.Width,rd.Height,unsigned(rd.Format),format,unsigned(rd.Flags),rd.SampleDesc.Count);log(msg);}
 if(rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count!=1 || rd.DepthOrArraySize!=1 ||
    !(rd.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))return;
 const uint32_t w=uint32_t(rd.Width),h=rd.Height;
 const Rects& black=blackRects(w,h,eye);
 Rects red,orange;
 if(effects.outline && foveating && !vrsPaused.load(std::memory_order_relaxed)){
  float cu,cv;{std::lock_guard<std::mutex> g(gazeMutex);cu=heldU[eye];cv=heldV[eye];}
  outlineRects(w,h,eye,cu,cv,config.center,red);
  // Orange marks the outer edge of the 2x2 band, as in the Vulkan outline.
  if(config.middle>config.center && config.middleRate==beamng_ffr::rate(2,2) && config.outerRate==beamng_ffr::rate(4,4))
   outlineRects(w,h,eye,cu,cv,config.middle,orange);
 }
 if(black.empty() && red.empty() && orange.empty())return;
 std::lock_guard<std::mutex> g(postMutex);
 const uint64_t done=ID3D12Fence_GetCompletedValue(postFence);
 Post* p=nullptr;
 for(auto& candidate:posts)if(candidate.value<=done){p=&candidate;break;}
 if(!p){finishBusy.fetch_add(1,std::memory_order_relaxed);return;} // GPU far behind: skip this image.
 auto slot=postSlots.find(texture);
 D3D12_CPU_DESCRIPTOR_HANDLE rtv=postRtvStart;
 if(slot==postSlots.end()){
  // Descriptors are read when recorded, so reusing a slot later is safe.
  if(postSlots.size()>=postRtvCount)postSlots.clear();
  slot=postSlots.emplace(texture,nextPostSlot++%postRtvCount).first;
  D3D12_CPU_DESCRIPTOR_HANDLE target=rtv;target.ptr+=SIZE_T(slot->second)*rtvIncrement;
  D3D12_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT(format?format:rd.Format);vd.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
  nextCreateRtv(fxDevice,resource,&vd,target);
 }
 rtv.ptr+=SIZE_T(slot->second)*rtvIncrement;
 const bool first=finished.load(std::memory_order_relaxed)==0;
 char msg[220]{};
 if(first){sprintf_s(msg,"M8.43 first eye image: black=%zu outline=%zu rtvSlot=%u",black.size(),red.size()+orange.size(),slot->second);log(msg);}
 HRESULT hr=ID3D12CommandAllocator_Reset(p->allocator);
 if(SUCCEEDED(hr))hr=ID3D12GraphicsCommandList_Reset(p->list,p->allocator,nullptr);
 if(FAILED(hr)){sprintf_s(msg,"M8.43 WARNING: eye image list reset failed hr=0x%08lX",hr);log(msg);return;}
 const float blackColor[4]{0,0,0,1},redColor[4]{1,.016f,.016f,1},orangeColor[4]{1,.55f,.05f,1};
 // ClearRenderTargetView ignores scissors and needs no render-target binding.
 // Hundreds of rectangles in one call took BeamNG down on a headset GPU while
 // three did not, so rectangles go in small batches.
 const auto clear=[&](const float color[4],const Rects& rects){
  for(size_t i=0;i<rects.size();i+=clearBatch)
   ID3D12GraphicsCommandList_ClearRenderTargetView(p->list,rtv,color,UINT(std::min(clearBatch,rects.size()-i)),rects.data()+i);
 };
 clear(blackColor,black);clear(orangeColor,orange);clear(redColor,red);
 hr=ID3D12GraphicsCommandList_Close(p->list);
 if(FAILED(hr)){sprintf_s(msg,"M8.43 WARNING: eye image list close failed hr=0x%08lX",hr);log(msg);return;}
 if(first)log("M8.43 first eye image: recorded, submitting");
 ID3D12CommandList* lists[]{reinterpret_cast<ID3D12CommandList*>(p->list)};
 ownSubmit=true;ID3D12CommandQueue_ExecuteCommandLists(fxQueue,1,lists);ownSubmit=false;
 ID3D12CommandQueue_Signal(fxQueue,postFence,++postValue);
 p->value=postValue;
 if(finished.fetch_add(1,std::memory_order_relaxed)<2){
  sprintf_s(msg,"M8.43 eye image %d finished: %ux%u black=%zu outline=%zu",eye,w,h,black.size(),red.size()+orange.size());log(msg);
 }
}
}

void finishEyeImage(uint64_t texture,int eye,uint32_t format){
 if(!finishing || !texture || (eye!=0 && eye!=1))return;
#ifdef _MSC_VER
 // A fault here must not take the game down: log it and stop finishing images.
 __try{finishEyeImageImpl(texture,eye,format);}
 __except(EXCEPTION_EXECUTE_HANDLER){finishing=false;log("M8.43 ERROR: eye image finishing faulted; black edges and outline turned off");}
#else
 finishEyeImageImpl(texture,eye,format);
#endif
}

void frame(){
 if(!hooked)return;
 // M8.43: report a lost device once, with its reason, before the game shuts down.
 static unsigned checks=0;static bool removedLogged=false;
 if(ID3D12Device* d=fxDevice?fxDevice:vrsDevice; d && !removedLogged && (++checks%8)==1){
  const HRESULT reason=ID3D12Device_GetDeviceRemovedReason(d);
  if(FAILED(reason)){removedLogged=true;char msg[160]{};
   sprintf_s(msg,"M8.43 ERROR: D3D12 device removed reason=0x%08lX (eyeImagesFinished=%llu)",reason,(unsigned long long)finished.load());log(msg);}
 }
 // M8.47: Ctrl+Shift+V toggles VRS; the GPU window restarts so each report covers one state.
 static bool keyDown=false;
 const bool down=foveating && (GetAsyncKeyState(VK_CONTROL)&0x8000) && (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState('V')&0x8000);
 if(down && !keyDown){
  const bool paused=!vrsPaused.load();vrsPaused=paused;
  {std::lock_guard<std::mutex> g(stampMutex);busyTicks=ourTicks=submissions=0;gpuFrames=0;gpuReports=0;}
  log(paused?"M8.47 VRS paused (Ctrl+Shift+V)":"M8.47 VRS resumed (Ctrl+Shift+V)");
 }
 keyDown=down;
 if(foveating)updateFoveation();
 if(gameQueue){
  uint64_t busy=0,ours=0,subs=0,skipped=0;unsigned gn=0;
  {std::lock_guard<std::mutex> g(stampMutex);
   if(stampFence)collectStamps();
   if(stampFence && ++gpuFrames>=(gpuReports?900u:300u)){
    ++gpuReports;gn=gpuFrames;busy=busyTicks;ours=ourTicks;subs=submissions;skipped=stampSkipped;
    gpuFrames=0;busyTicks=ourTicks=submissions=0;
   }}
  if(gn){char msg[220]{};
   sprintf_s(msg,"M8.47 D3D12 GPU frames=%u busy=%.2f ms/frame (mod=%.2f) submissions=%.1f/frame vrs=%s skipped=%llu",
     gn,double(busy)/ticksPerMs/gn,double(ours)/ticksPerMs/gn,double(subs)/gn,!foveating?"off":vrsPaused.load()?"paused":"on",(unsigned long long)skipped);log(msg);}
 }
 std::vector<std::pair<Key,uint64_t>> top;uint64_t unknown=0,om=0,pass=0,vrs=0,later=0,depthOnly=0,cropped=0,calls=0,bare=0,assumed=0;unsigned n=0;
 {
  std::lock_guard<std::mutex> g(statsMutex);
  ++frames;
  // First report after warm-up, then a few more; reports cover the frames since the last one.
  if(reports>=4 || frames<(reports?900u:300u))return;
  ++reports;n=frames;frames=0;
  top.assign(binds.begin(),binds.end());unknown=unknownBinds;om=omBinds;pass=passBinds;vrs=vrsBinds;later=vrsLaterTarget;depthOnly=vrsDepthOnly;
  binds.clear();unknownBinds=omBinds=passBinds=vrsBinds=vrsLaterTarget=vrsDepthOnly=0;
  cropped=croppedScissors.exchange(0,std::memory_order_relaxed);
  calls=scissorCalls.exchange(0,std::memory_order_relaxed);
  bare=eyeBindsWithoutScissor.exchange(0,std::memory_order_relaxed);
  assumed=assumedScissors.exchange(0,std::memory_order_relaxed);
 }
 char msg[384]{};
 if(fxDevice){sprintf_s(msg,"M8.43 D3D12 effects frames=%u croppedScissors=%llu (%.1f per frame) scissorCallsPerFrame=%.1f eyeBindsWithoutScissorPerFrame=%.1f cropScissorsSetPerFrame=%.1f eyeImagesFinished=%llu skippedBusy=%llu",
   n,(unsigned long long)cropped,double(cropped)/n,double(calls)/n,double(bare)/n,double(assumed)/n,(unsigned long long)finished.load(),(unsigned long long)finishBusy.load());log(msg);}
 if(foveating){sprintf_s(msg,"M8.42 D3D12 foveation frames=%u vrsBindsPerFrame=%.1f (laterTarget=%.2f depthOnly=%.2f)",n,double(vrs)/n,double(later)/n,double(depthOnly)/n);log(msg);}
 if(!diagnostics)return;
 std::sort(top.begin(),top.end(),[](const auto& a,const auto& b){return a.second>b.second;});
 sprintf_s(msg,"M8.41 D3D12 render targets frames=%u bindsPerFrame=%.1f renderPassPerFrame=%.1f unknownPerFrame=%.1f",
   n,double(om)/n,double(pass)/n,double(unknown)/n);
 log(msg);
 for(size_t i=0;i<top.size()&&i<16;++i){
  const auto& k=top[i].first;
  sprintf_s(msg,"M8.41 rt %ux%u format=%u samples=%u eyeSwapchain=%d bindsPerFrame=%.1f",
    k.width,k.height,k.format,k.samples,int(k.eye),double(top[i].second)/n);
  log(msg);
 }
}
}
