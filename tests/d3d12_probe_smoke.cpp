// M8.41-M8.43: real D3D12 hooks on a WARP device (no GPU or headset): install,
// chain to the runtime, classify render targets, clip scissors on eye-shaped
// targets, black out cropped edges on a released eye image and, where WARP
// offers VRS Tier 2, create, upload and bind shading-rate images and draw the
// gaze outline, without device removal.
#include "d3d12_probe.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
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
 D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};ID3D12CommandQueue* queue=nullptr;
 assert(SUCCEEDED(device->CreateCommandQueue(&qd,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&queue))));
 D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
 const bool tier2=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options))) &&
   options.VariableShadingRateTier>=D3D12_VARIABLE_SHADING_RATE_TIER_2 && options.ShadingRateImageTileSize==16;
 beamng_d3d12::Foveation fov{};
 assert(!beamng_d3d12::attach(device,queue,capture,false,fov)); // Nothing requested: no hooks.
 assert(lines.empty());
 fov.enabled=true;fov.gaze=true;
 beamng_d3d12::Effects fx{};
 fx.crop=true;fx.margins[0]={.25,.25,.25,0};fx.margins[1]={.25,.25,0,.25};
 fx.lensMask=true;fx.lensSize=0.6;fx.outline=true;
 assert(beamng_d3d12::attach(device,queue,capture,true,fov,fx));
 assert(beamng_d3d12::attach(device,queue,capture,true,fov,fx)); // Idempotent: hooks install once.
 assert(logged("M8.43 D3D12 effects active: fovCrop=0 crop=1"));
 assert(logged("M8.41 D3D12 VRS tier=") && logged("hooks=installed"));
 assert(logged(tier2?"M8.42 D3D12 foveation active":"M8.42 D3D12 foveation unavailable"));
 beamng_d3d12::setEyeSize(96,80);

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
 // M8.46: an eye-shaped depth buffer drives VRS for depth-only passes.
 ID3D12Resource* depthBuffer=nullptr;
 {D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=96;rd.Height=80;
  rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_D32_FLOAT;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  assert(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&depthBuffer))));}
 D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1};
 ID3D12DescriptorHeap* dsvs=nullptr;
 assert(SUCCEEDED(device->CreateDescriptorHeap(&dd,__uuidof(ID3D12DescriptorHeap),reinterpret_cast<void**>(&dsvs))));
 const auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();
 device->CreateDepthStencilView(depthBuffer,nullptr,dsv);
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
 ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);
 ID3D12Fence* fence=nullptr;assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&fence))));
 uint64_t fenceValue=0;
 auto drain=[&]{assert(SUCCEEDED(queue->Signal(fence,++fenceValue)));while(fence->GetCompletedValue()<fenceValue)Sleep(1);
  assert(SUCCEEDED(device->GetDeviceRemovedReason()));};
 drain();

 // Both eye-shaped sizes were requested; the next frame builds and uploads their images.
 beamng_d3d12::frame();drain();
 if(tier2){
  assert(logged("M8.42 shading-rate image for 96x80: 6x5 tiles"));
  assert(logged("M8.42 shading-rate image for 48x40: 3x3 tiles"));
 }
 // Bind with VRS, then a non-eye target, then follow a moving gaze (re-upload) and bind again.
 for(int round=0;round<2;++round){
  assert(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator,nullptr)));
  list->OMSetRenderTargets(1,&first,FALSE,nullptr);list->ClearRenderTargetView(first,black,0,nullptr);
  const D3D12_RECT full{0,0,96,80};list->RSSetScissorRects(1,&full); // Clipped now and again for the next eye-shaped bind.
  list->OMSetRenderTargets(1,&second,FALSE,nullptr);list->ClearRenderTargetView(second,black,0,nullptr);
  list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.f,0,0,nullptr);
  list->OMSetRenderTargets(0,nullptr,FALSE,nullptr);
  assert(SUCCEEDED(list->Close()));queue->ExecuteCommandLists(1,lists);drain();
  const float u[2]{.2f,.1f},v[2]{.8f,.9f};beamng_d3d12::setGaze(u,v,3);
  beamng_d3d12::frame();drain();
 }

 for(int i=0;i<297;++i)beamng_d3d12::frame();
 drain();
 assert(logged("M8.41 D3D12 render targets frames=300"));
 assert(logged("M8.41 rt 96x80 format=28 samples=1 eyeSwapchain=1"));
 assert(logged("M8.41 rt 48x40 format=28 samples=1 eyeSwapchain=0"));
 if(tier2)assert(logged("M8.42 D3D12 foveation frames=300 vrsBindsPerFrame=0.0 (laterTarget=0.00 depthOnly=0.01)")); // 6 VRS binds, 2 depth-only, over 300 frames.
 assert(logged("M8.43 D3D12 effects frames=300 croppedScissors=4 "));
 assert(logged("M8.47 D3D12 GPU timing active"));
 assert(logged("M8.47 D3D12 GPU frames=300 busy="));
 assert(logged(tier2?" vrs=on skipped=0":" vrs=off skipped=0")); // 2 rounds x (scissor set + half-size bind).

 // Finish the left eye image: white, then cropped edges/corners black and the outline at the gaze.
 {const float u[2]{.2f,.1f},v[2]{.8f,.9f};beamng_d3d12::setGaze(u,v,3);beamng_d3d12::frame();drain();}
 const float white[4]{1,1,1,1};
 assert(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator,nullptr)));
 list->ClearRenderTargetView(first,white,0,nullptr);
 assert(SUCCEEDED(list->Close()));queue->ExecuteCommandLists(1,lists);drain();
 beamng_d3d12::finishEyeImage(uint64_t(reinterpret_cast<uintptr_t>(eye)),0,DXGI_FORMAT_R8G8B8A8_UNORM);drain();
 assert(logged("M8.43 eye image 0 finished: 96x80"));
 D3D12_HEAP_PROPERTIES readHeap{D3D12_HEAP_TYPE_READBACK};
 D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=512*80;bd.Height=1;bd.DepthOrArraySize=1;
 bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ID3D12Resource* readback=nullptr;
 assert(SUCCEEDED(device->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&readback))));
 assert(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator,nullptr)));
 D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=eye;
 barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
 barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
 list->ResourceBarrier(1,&barrier);
 D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=eye;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
 dst.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,96,80,1,512};
 list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
 std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
 assert(SUCCEEDED(list->Close()));queue->ExecuteCommandLists(1,lists);drain();
 uint8_t* pixels=nullptr;assert(SUCCEEDED(readback->Map(0,nullptr,reinterpret_cast<void**>(&pixels))));
 auto pixel=[&](int x,int y){const uint8_t* p=pixels+y*512+x*4;return (uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];};
 assert(pixel(48,40)==0xFFFFFF);   // Kept center.
 assert(pixel(48,2)==0);           // Top crop (25% of 80 rows).
 assert(pixel(48,77)==0);          // Bottom crop.
 assert(pixel(12,40)==0);          // Left eye's outer edge (25% of 96 columns).
 assert(pixel(70,40)==0xFFFFFF);   // The left eye keeps its nose side...
 assert(pixel(90,24)==0);          // ...up to the lens ellipse (size 0.6).
 if(tier2)assert(pixel(1,70)>>16==0xFF && (pixel(1,70)&0xFFFF)<0x1000); // Red outline at the gaze (u=0.2, v=0.8).
 readback->Unmap(0,nullptr);release(readback);
 for(const auto& l:lines)std::cout<<l<<'\n';
 release(fence);release(queue);release(list);release(allocator);release(rtvs);release(dsvs);release(depthBuffer);release(half);release(eye);
 release(device);release(warp);release(factory);
 std::cout<<"D3D12 probe: WARP device hooks, binds, eye classification, report and "<<(tier2?"VRS image upload/bind/gaze update":"VRS-unavailable fallback")<<" passed\n";
}
