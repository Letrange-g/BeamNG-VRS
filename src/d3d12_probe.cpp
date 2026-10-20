// M8.41/M8.42: Direct3D 12 probe and foveation; see d3d12_probe.hpp.
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
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
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
uint64_t unknownBinds=0,omBinds=0,passBinds=0,vrsBinds=0;
unsigned frames=0,reports=0;

// Foveation device state; touched only from attach() and frame() (OpenXR frame thread).
ID3D12Device* vrsDevice=nullptr;ID3D12CommandQueue* vrsQueue=nullptr;
ID3D12CommandAllocator* uploadAllocator=nullptr;ID3D12GraphicsCommandList* uploadList=nullptr;
ID3D12Fence* uploadFence=nullptr;uint64_t uploadValue=0;
Foveation config{};
std::mutex gazeMutex;
float gazeU[2]{.5f,.5f},gazeV[2]{.5f,.5f};uint32_t gazeMask=0;uint64_t gazeTick=0;
beamng_gaze_hold::State hold[2]{};

decltype(ID3D12DeviceVtbl::CreateRenderTargetView) nextCreateRtv=nullptr;
decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets) nextOMSetRenderTargets=nullptr;
decltype(ID3D12GraphicsCommandList4Vtbl::BeginRenderPass) nextBeginRenderPass=nullptr;

void log(const char* msg){if(logFn)logFn(msg);}
uint64_t sizeKey(uint32_t w,uint32_t h){return (uint64_t(w)<<32)|h;}

// Same aspect ratio as the OpenXR eye image, at 25-100% of its size.
bool eyeShaped(uint32_t w,uint32_t h){
 const uint32_t ew=eyeWidth,eh=eyeHeight;
 if(!ew||!eh||w>ew||h>eh||uint64_t(w)*4<ew)return false;
 const double expected=double(eh)*double(w)/double(ew);
 return std::fabs(double(h)-expected)<=2.0;
}

// The first render target decides the shading-rate image for the whole bind.
void bind(ID3D12GraphicsCommandList* list,SIZE_T first,UINT count,bool renderPass,const SIZE_T* all){
 Target t{};bool known=false,eye=false,eligible=false;ID3D12Resource* image=nullptr;
 {
  std::shared_lock<std::shared_mutex> g(targetsMutex);
  if(count){
   auto it=targets.find(first);
   if(it!=targets.end()){t=it->second;known=true;eye=eyeImages.count(t.resource)!=0;}
  }
  eligible=foveating && known && t.samples==1 && t.device==vrsDevice && eyeShaped(t.width,t.height);
  if(eligible){
   auto r=rateImages.find({t.width,t.height});
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
   if(eligible){
    std::unique_lock<std::shared_mutex> g(targetsMutex);wantedSizes.insert(sizeKey(t.width,t.height));
   }
  }
 }
 if(!diagnostics && !foveating)return;
 std::lock_guard<std::mutex> g(statsMutex);
 if(image)++vrsBinds;
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
 bind(list,handles[0],n,false,handles);
}

void STDMETHODCALLTYPE hookBeginRenderPass(ID3D12GraphicsCommandList4* list,UINT count,
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,D3D12_RENDER_PASS_FLAGS flags){
 // The shading-rate image must be set before the pass begins.
 SIZE_T handles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
 const UINT n=rts?std::min<UINT>(count,D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT):0;
 for(UINT i=0;i<n;++i)handles[i]=rts[i].cpuDescriptor.ptr;
 bind(reinterpret_cast<ID3D12GraphicsCommandList*>(list),handles[0],n,true,handles);
 nextBeginRenderPass(list,count,rts,depth,flags);
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
 ok=ok&&patch(&list->lpVtbl->OMSetRenderTargets,
   decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets)(&hookOMSetRenderTargets),nextOMSetRenderTargets);
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
 }
 ID3D12Device_AddRef(device);ID3D12CommandQueue_AddRef(queue); // Our resources must not outlive them.
 vrsDevice=device;vrsQueue=queue;
 sprintf_s(msg,"M8.42 D3D12 foveation active: tier=2 tile=%u gaze=%d center=%.2f middle=%.2f rates=%u/%u shape=%s",
   options.ShadingRateImageTileSize,int(config.gaze),config.center,config.middle,
   unsigned(config.middleRate),unsigned(config.outerRate),beamng_ffr::shapeName(config.shape));
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

std::vector<uint8_t> pattern(uint32_t w,uint32_t h,const float u[2],const float v[2]){
 const UINT tile=tileSize();
 beamng_ffr::Pattern eyes[2];
 for(int i=0;i<2;++i)eyes[i]=beamng_ffr::makeGaze(w,h,tile,tile,u[i],v[i],config.center,config.middle,
   config.middleRate,config.outerRate,config.shape);
 std::vector<uint8_t> combined;beamng_ffr::combineFiner(eyes[0].texels,eyes[1].texels,combined);
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
 ID3D12CommandQueue_ExecuteCommandLists(vrsQueue,1,lists);
 ID3D12CommandQueue_Signal(vrsQueue,uploadFence,++uploadValue);
 // Game lists recorded from now on are submitted after this upload on the same queue.
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 for(const auto& j:jobs)j.img->ready=true;
}
}

bool attach(void* device,void* queue,LogFn fn,bool diag,const Foveation& f){
 if(!device || (!diag && !f.enabled))return false;
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

void frame(){
 if(!hooked)return;
 if(foveating)updateFoveation();
 std::vector<std::pair<Key,uint64_t>> top;uint64_t unknown=0,om=0,pass=0,vrs=0;unsigned n=0;
 {
  std::lock_guard<std::mutex> g(statsMutex);
  ++frames;
  // First report after warm-up, then a few more; reports cover the frames since the last one.
  if(reports>=4 || frames<(reports?900u:300u))return;
  ++reports;n=frames;frames=0;
  top.assign(binds.begin(),binds.end());unknown=unknownBinds;om=omBinds;pass=passBinds;vrs=vrsBinds;
  binds.clear();unknownBinds=omBinds=passBinds=vrsBinds=0;
 }
 char msg[256]{};
 if(foveating){sprintf_s(msg,"M8.42 D3D12 foveation frames=%u vrsBindsPerFrame=%.1f",n,double(vrs)/n);log(msg);}
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
