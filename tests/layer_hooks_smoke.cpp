// Exercise the real Vulkan hooks and staging code without a GPU/runtime.
#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
#include <new>

namespace {std::atomic<uint64_t> allocations{0};}
void* operator new(size_t n){allocations.fetch_add(1);if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);}
void operator delete[](void* p,size_t)noexcept{std::free(p);}
namespace {
VkPipeline bound=VK_NULL_HANDLE;
std::array<VkRect2D,16> scissors{};
uint32_t scissorCount=0,clearCalls=0,clearRects=0;
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer,VkPipelineBindPoint,VkPipeline p){bound=p;}
VKAPI_ATTR void VKAPI_CALL setScissor(VkCommandBuffer,uint32_t,uint32_t count,const VkRect2D* rects){
 scissorCount=count;assert(count<=scissors.size());for(uint32_t i=0;i<count;++i)scissors[i]=rects[i];
}
VKAPI_ATTR void VKAPI_CALL clear(VkCommandBuffer,uint32_t attachmentCount,const VkClearAttachment*,uint32_t count,const VkClearRect* rects){
 assert(attachmentCount==1);++clearCalls;clearRects=count;
 for(uint32_t i=0;i<count;++i)assert(rects[i].layerCount==1 && rects[i].rect.extent.height==30);
}
}
int main(int argc,char** argv){
 const bool crop=argc==2 && std::strcmp(argv[1],"scissor")==0;
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE",crop?"1":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_TOP","0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_BOTTOM","0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_DEBUG_OUTLINE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M84_CPU_GAZE","1");
 SetEnvironmentVariableA("BEAMNG_VRS_GAZE_GRACE_MS","0");
 SetEnvironmentVariableA("BEAMNG_VRS_CENTER_RADIUS","0.25");
 SetEnvironmentVariableA("BEAMNG_VRS_MID_RADIUS","0.60");
 SetEnvironmentVariableA("BEAMNG_VRS_SHAPE","ellipse");
 wchar_t temporary[32768]{};assert(GetTempPathW(32768,temporary));
 const auto missing=std::filesystem::path(temporary)/(L"BeamNG-VRS-no-config-"+std::to_wstring(GetCurrentProcessId())+L".ini");
 assert(SetEnvironmentVariableW(L"BEAMNG_VRS_CONFIG",missing.c_str()));
 const auto device=reinterpret_cast<VkDevice>(uintptr_t(1));
 const auto cmd=reinterpret_cast<VkCommandBuffer>(uintptr_t(2));
 const auto original=reinterpret_cast<VkPipeline>(uintptr_t(3));
 const auto replacement=reinterpret_cast<VkPipeline>(uintptr_t(4));
 const auto pass=reinterpret_cast<VkRenderPass>(uintptr_t(5));
 DeviceDispatch d{};d.cmdBindPipeline=bind;d.cmdSetScissor=setScissor;d.cmdClearAttachments=clear;
 devices[device]=d;commandDevices[cmd]=device;
 m89ActiveShadows[cmd]=pass;m89Variants[original]={device,replacement,pass};
 m87ActivePasses[cmd]=pass;
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==replacement);
 VkRect2D rect{{0,0},{100,100}};
 cropStates[cmd]={true,100,100,rect,0};
 vkCmdSetScissor(cmd,0,1,&rect);assert(scissors[0].extent.height==(crop?40u:100u));
 const uint64_t before=allocations.load();
 for(unsigned i=0;i<1000;++i){
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);
  vkCmdSetScissor(cmd,0,1,&rect);
 }
 assert(allocations.load()==before);assert(m87PassBindCounts.empty());assert(bound==replacement);
 VkRect2D stereo[2]{rect,rect};
 const uint64_t multiBefore=allocations.load();vkCmdSetScissor(cmd,0,2,stereo);
 assert(allocations.load()==multiBefore && scissorCount==2);
 cropBlackBars(cmd,d);assert(clearCalls==(crop?1u:0u));if(crop)assert(clearRects==2);
 m89ActiveShadows.erase(cmd);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==original);
 // Stage through the real producer. A newer sample with identical payload
 // must not create a new upload, even while the latest source slot is pinned.
 m814ActiveWidth=64;m814ActiveHeight=64;
 std::array<std::array<std::array<uint8_t,64>,3>,2> memory{};
 auto& staging=m812Staging[device];
 for(int eye=0;eye<2;++eye)for(int slot=0;slot<3;++slot){
  staging.slots[eye][slot].mapped=memory[eye][slot].data();staging.slots[eye][slot].capacity=64;
 }
 HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(beamng_projected_gaze::Data),beamng_projected_gaze::mappingName);assert(mapping);
 auto* shared=static_cast<beamng_projected_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(beamng_projected_gaze::Data)));assert(shared);
 auto sample=[&](float u,float v,uint32_t mask=3){
  InterlockedIncrement(&shared->sequence);MemoryBarrier();shared->magic=beamng_projected_gaze::magicValue;
  shared->processId=GetCurrentProcessId();shared->eyeValidMask=mask;
  shared->poseTickMs=shared->projectionTickMs=GetTickCount64();
  for(int eye=0;eye<2;++eye){shared->eyeU[eye]=u;shared->eyeV[eye]=v;}
  MemoryBarrier();InterlockedIncrement(&shared->sequence);
 };
 auto prepare=[&](){Sleep(12);m84PrepareEyePatterns();};
 sample(.43f,.66f);prepare();assert(m812bPreparedCount[0]==1 && m812bPreparedCount[1]==1);
 auto& state=m815SlotStates[device][m814Key(64,64)];
 for(int eye=0;eye<2;++eye)state.busy[eye][state.latestSlot[eye]]=true;
 sample(.43f,.66f);prepare();assert(m812bPreparedCount[0]==1 && m821GazeStats.identicalPattern==2);
 sample(.9f,.1f);prepare();assert(m812bPreparedCount[0]==2 && m812bPreparedCount[1]==2);
 // Exhausted slots remain immutable and the new sample is retried later.
 for(auto& eye:state.busy)eye.fill(true);
 const auto saved=memory;sample(.1f,.9f);prepare();assert(memory==saved && m812bPreparedCount[0]==2);
 for(auto& eye:state.busy)eye.fill(false);
 prepare();assert(m812bPreparedCount[0]==3 && m812bPreparedCount[1]==3);
 // The same source sequence must be restaged after a resolution switch.
 m814ActiveWidth=80;prepare();assert(m812bPreparedCount[0]==4 && m812bPreparedCount[1]==4);
 sample(.1f,.9f,0);prepare();assert(m812bPreparedCount[0]==5 && m825StagedCenters[0].u==.5f);
 // Returning to a cached resolution during dropout still restores center.
 m814ActiveWidth=64;state.latestSlot={{-1,-1}};prepare();assert(m812bPreparedCount[0]==6 && m812bPreparedCount[1]==6);
 UnmapViewOfFile(shared);CloseHandle(mapping);
 std::cout<<"Real Vulkan hooks: allocation-free dispatch/scissors, pipeline substitution, batched bars, identical-payload skip, pinned staging and resize fallback passed\n";
}
