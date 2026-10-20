// M8.41: diagnostics-only Direct3D 12 probe; see d3d12_probe.hpp.
// The C COM interface gives typed vtable slots, so hooks are patched by field
// name rather than by hand-counted indices. Every hook forwards first and
// only records; nothing here changes what the game renders.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include "d3d12_probe.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beamng_d3d12 {
namespace {
struct Target { uint32_t width=0,height=0,format=0,samples=0; uint64_t resource=0; };
struct Key {
 uint32_t width,height,format,samples; bool eye;
 bool operator==(const Key&) const = default;
};
struct KeyHash { size_t operator()(const Key& k) const {
 return (size_t(k.width)<<40)^(size_t(k.height)<<20)^(size_t(k.format)<<8)^(size_t(k.samples)<<1)^size_t(k.eye);
}};

LogFn logFn=nullptr;
std::atomic<bool> attached{false};
std::once_flag attachOnce;
UINT rtvIncrement=0;

std::shared_mutex targetsMutex; // descriptor address -> bound texture
std::unordered_map<SIZE_T,Target> targets;
std::unordered_set<uint64_t> eyeImages;
std::mutex statsMutex;
std::unordered_map<Key,uint64_t,KeyHash> binds;
uint64_t unknownBinds=0,omBinds=0,passBinds=0;
unsigned frames=0,reports=0;

decltype(ID3D12DeviceVtbl::CreateRenderTargetView) nextCreateRtv=nullptr;
decltype(ID3D12GraphicsCommandListVtbl::OMSetRenderTargets) nextOMSetRenderTargets=nullptr;
decltype(ID3D12GraphicsCommandList4Vtbl::BeginRenderPass) nextBeginRenderPass=nullptr;

void log(const char* msg){if(logFn)logFn(msg);}

void record(SIZE_T descriptor,bool renderPass){
 Target t{};bool known=false;
 bool eye=false;
 {std::shared_lock<std::shared_mutex> g(targetsMutex);auto it=targets.find(descriptor);
  if(it!=targets.end()){t=it->second;known=true;eye=eyeImages.count(t.resource)!=0;}}
 std::lock_guard<std::mutex> g(statsMutex);
 ++(renderPass?passBinds:omBinds);
 if(!known){++unknownBinds;return;}
 ++binds[Key{t.width,t.height,t.format,t.samples,eye}];
}

void STDMETHODCALLTYPE hookCreateRtv(ID3D12Device* device,ID3D12Resource* resource,
  const D3D12_RENDER_TARGET_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE destination){
 nextCreateRtv(device,resource,desc,destination);
 if(!resource)return;
 D3D12_RESOURCE_DESC rd{};ID3D12Resource_GetDesc(resource,&rd);
 if(rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)return;
 UINT mip=0;
 if(desc && desc->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D)mip=desc->Texture2D.MipSlice;
 else if(desc && desc->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2DARRAY)mip=desc->Texture2DArray.MipSlice;
 Target t{};
 t.width=std::max<uint32_t>(1,uint32_t(rd.Width>>mip));t.height=std::max<uint32_t>(1,rd.Height>>mip);
 t.format=uint32_t(desc && desc->Format!=DXGI_FORMAT_UNKNOWN?desc->Format:rd.Format);t.samples=rd.SampleDesc.Count;
 t.resource=uint64_t(reinterpret_cast<uintptr_t>(resource));
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 targets[destination.ptr]=t;
}

void STDMETHODCALLTYPE hookOMSetRenderTargets(ID3D12GraphicsCommandList* list,UINT count,
  const D3D12_CPU_DESCRIPTOR_HANDLE* rts,BOOL singleRange,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){
 nextOMSetRenderTargets(list,count,rts,singleRange,depth);
 if(!rts)return;
 for(UINT i=0;i<count;++i)record(singleRange?rts[0].ptr+SIZE_T(i)*rtvIncrement:rts[i].ptr,false);
}

void STDMETHODCALLTYPE hookBeginRenderPass(ID3D12GraphicsCommandList4* list,UINT count,
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,D3D12_RENDER_PASS_FLAGS flags){
 nextBeginRenderPass(list,count,rts,depth,flags);
 if(!rts)return;
 for(UINT i=0;i<count;++i)record(rts[i].cpuDescriptor.ptr,true);
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
 D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
 if(SUCCEEDED(ID3D12Device_CheckFeatureSupport(device,D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options)))){
  sprintf_s(msg,"M8.41 D3D12 VRS tier=%d tileSize=%u perPrimitiveViewports=%d additionalRates=%d",
    int(options.VariableShadingRateTier),options.ShadingRateImageTileSize,
    int(options.PerPrimitiveShadingRateSupportedWithViewportIndexing),int(options.AdditionalShadingRatesSupported));
 }else sprintf_s(msg,"M8.41 D3D12 VRS tier=unknown (OPTIONS6 query failed)");
 log(msg);
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
 sprintf_s(msg,"M8.41 D3D12 probe hooks=%s renderPassHook=%d rtvIncrement=%u (diagnostics only, rendering unchanged)",
   ok?"installed":"FAILED",int(renderPasses),rtvIncrement);
 log(msg);
 return ok;
}
}

bool attach(void* device,LogFn fn){
 if(!device)return false;
 std::call_once(attachOnce,[&]{logFn=fn;attached=install(static_cast<ID3D12Device*>(device));});
 return attached;
}

void setEyeImages(const uint64_t* images,size_t count){
 if(!attached||!images)return;
 {std::shared_lock<std::shared_mutex> g(targetsMutex);bool known=true;
  for(size_t i=0;i<count;++i)if(images[i] && !eyeImages.count(images[i]))known=false;
  if(known)return;}
 std::unique_lock<std::shared_mutex> g(targetsMutex);
 for(size_t i=0;i<count;++i)if(images[i])eyeImages.insert(images[i]);
}

void frame(){
 if(!attached)return;
 std::vector<std::pair<Key,uint64_t>> top;uint64_t unknown=0,om=0,pass=0;unsigned n=0;
 {
  std::lock_guard<std::mutex> g(statsMutex);
  ++frames;
  // First report after warm-up, then a few more; reports cover the frames since the last one.
  if(reports>=4 || frames<(reports?900u:300u))return;
  ++reports;n=frames;frames=0;
  top.assign(binds.begin(),binds.end());unknown=unknownBinds;om=omBinds;pass=passBinds;
  binds.clear();unknownBinds=omBinds=passBinds=0;
 }
 std::sort(top.begin(),top.end(),[](const auto& a,const auto& b){return a.second>b.second;});
 char msg[256]{};
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
