// M8.41: real D3D12 hooks on a WARP device (no GPU or headset): install,
// chain to the runtime, classify render targets and report without changing rendering.
#include "d3d12_probe.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::vector<std::string> lines;
void capture(const char* msg){lines.push_back(msg);}
bool logged(const char* text){for(const auto& l:lines)if(l.find(text)!=std::string::npos)return true;return false;}
template<class T> void release(T*& p){if(p){p->Release();p=nullptr;}}
}

int main(){
 IDXGIFactory4* factory=nullptr;IDXGIAdapter* warp=nullptr;ID3D12Device* device=nullptr;
 if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),reinterpret_cast<void**>(&factory))) ||
    FAILED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter),reinterpret_cast<void**>(&warp))) ||
    FAILED(D3D12CreateDevice(warp,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),reinterpret_cast<void**>(&device)))){
  // CI runners provide WARP; a silent skip would hide a broken probe.
  std::cerr<<"D3D12 WARP device unavailable\n";return 1;
 }
 assert(beamng_d3d12::attach(device,capture));
 assert(beamng_d3d12::attach(device,capture)); // Idempotent: hooks install once.
 assert(logged("M8.41 D3D12 VRS tier=") && logged("hooks=installed"));

 D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
 auto texture=[&](UINT64 w,UINT h){
  D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;
  rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;
  rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ID3D12Resource* r=nullptr;
  assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&r))));
  return r;
 };
 ID3D12Resource* eye=texture(96,80);ID3D12Resource* half=texture(48,40);
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2};
 ID3D12DescriptorHeap* rtvs=nullptr;
 assert(SUCCEEDED(device->CreateDescriptorHeap(&hd,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&rtvs))));
 auto first=rtvs->GetCPUDescriptorHandleForHeapStart();
 auto second=first;second.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 device->CreateRenderTargetView(eye,nullptr,first);
 device->CreateRenderTargetView(half,nullptr,second);
 const uint64_t images[6]{uint64_t(reinterpret_cast<uintptr_t>(eye))};
 beamng_d3d12::setEyeImages(images,6);

 ID3D12CommandAllocator* allocator=nullptr;ID3D12GraphicsCommandList* list=nullptr;
 assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&allocator))));
 assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,__uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(&list))));
 // One single-range bind of both targets, then the half-size target alone; clears still reach the runtime.
 list->OMSetRenderTargets(2,&first,TRUE,nullptr);
 list->OMSetRenderTargets(1,&second,FALSE,nullptr);
 const float black[4]{};list->ClearRenderTargetView(second,black,0,nullptr);
 ID3D12GraphicsCommandList4* list4=nullptr;
 if(SUCCEEDED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList4),reinterpret_cast<void**>(&list4)))){
  D3D12_RENDER_PASS_RENDER_TARGET_DESC rt{};rt.cpuDescriptor=first;
  rt.BeginningAccess.Type=D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;rt.EndingAccess.Type=D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
  list4->BeginRenderPass(1,&rt,nullptr,D3D12_RENDER_PASS_FLAG_NONE);list4->EndRenderPass();
  release(list4);
 }
 assert(SUCCEEDED(list->Close()));
 D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};ID3D12CommandQueue* queue=nullptr;
 assert(SUCCEEDED(device->CreateCommandQueue(&qd,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&queue))));
 ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);
 ID3D12Fence* fence=nullptr;assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&fence))));
 assert(SUCCEEDED(queue->Signal(fence,1)));
 while(fence->GetCompletedValue()<1)Sleep(1);
 assert(SUCCEEDED(device->GetDeviceRemovedReason()));

 for(int i=0;i<300;++i)beamng_d3d12::frame();
 assert(logged("M8.41 D3D12 render targets frames=300"));
 assert(logged("M8.41 rt 96x80 format=28 samples=1 eyeSwapchain=1"));
 assert(logged("M8.41 rt 48x40 format=28 samples=1 eyeSwapchain=0"));
 for(const auto& l:lines)std::cout<<l<<'\n';
 release(fence);release(queue);release(list);release(allocator);release(rtvs);release(half);release(eye);
 release(device);release(warp);release(factory);
 std::cout<<"D3D12 probe: WARP device hooks, single-range and render-pass binds, eye classification and report passed\n";
}
