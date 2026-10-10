// Actual layer interceptors, deterministic runtime, no headset/GPU.
#include "../src/openxr_probe.cpp"
#include "turbo_runtime_fixture.hpp"
#include <iostream>
using namespace beamng_turbo_fixture;
namespace {
unsigned sessionNumber=20,sessionEnds=0,sessionDestroys=0,swapchainDestroys=0,instanceDestroys=0;
XRAPI_ATTR XrResult XRAPI_CALL createSession(XrInstance,const XrSessionCreateInfo*,XrSession* out){*out=reinterpret_cast<XrSession>(uintptr_t(++sessionNumber));return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL beginSession(XrSession,const XrSessionBeginInfo*){return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL endSession(XrSession){assert(runtime->inFlight==0);++sessionEnds;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL destroySession(XrSession){assert(runtime->inFlight==0);++sessionDestroys;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL createSwapchain(XrSession,const XrSwapchainCreateInfo*,XrSwapchain* out){*out=reinterpret_cast<XrSwapchain>(uintptr_t(500));return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL destroySwapchain(XrSwapchain){assert(runtime->inFlight==0);++swapchainDestroys;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL destroyInstance(XrInstance){assert(runtime->inFlight==0);++instanceDestroys;return XR_SUCCESS;}
}
int main(){
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");SetEnvironmentVariableA("BEAMNG_EYE_SAMPLE","0");
 wchar_t temporary[32768]{};assert(GetTempPathW(32768,temporary));
 const auto directory=std::filesystem::path(temporary)/(L"BeamNG-turbo-hooks-"+std::to_wstring(GetCurrentProcessId()));
 std::filesystem::create_directories(directory);
 assert(SetEnvironmentVariableW(L"TEMP",directory.c_str()));
 const auto missing=directory/L"missing.ini";assert(SetEnvironmentVariableW(L"BEAMNG_VRS_CONFIG",missing.c_str()));
 const auto inst=reinterpret_cast<XrInstance>(uintptr_t(1));
 Dispatch d{};d.createSession=createSession;d.waitFrame=waitFrame;d.beginFrame=beginFrame;d.endFrame=endFrame;
 d.beginSession=beginSession;d.endSession=endSession;d.destroySession=destroySession;d.createSwapchain=createSwapchain;
 d.destroySwapchain=destroySwapchain;d.destroy=destroyInstance;instances[inst]=d;
 XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
 XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};ci.next=&binding;
 XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
 // Disabled VRS/eye tracking must still retain session/swapchain ownership.
 SetEnvironmentVariableA("BEAMNG_VRS_TURBO_MODE","0");
 {Runtime r;runtime=&r;r.allow(100);XrSession created{};
  assert(probeCreateSession(inst,&ci,&created)==XR_SUCCESS && sessions.count(created) && !sessions.at(created).turbo);
  assert(probeBeginSession(created,&begin)==XR_SUCCESS);
  auto out=state();assert(probeWaitFrame(created,nullptr,&out)==XR_SUCCESS);
  assert(probeBeginFrame(created,nullptr)==XR_SUCCESS);auto e=endInfo();assert(probeEndFrame(created,&e)==XR_SUCCESS);
  assert(r.waits==1 && r.begins==1 && r.ends==1);
  assert(probeEndSession(created)==XR_SUCCESS && probeDestroySession(created)==XR_SUCCESS && !sessions.count(created));
 }
 SetEnvironmentVariableA("BEAMNG_VRS_TURBO_MODE","1");
 for(bool instanceExit:{false,true}){
  Runtime r;runtime=&r;XrSession created{};instances[inst]=d;
  assert(probeCreateSession(inst,&ci,&created)==XR_SUCCESS && sessions.at(created).turbo);
  PFN_xrVoidFunction fn{};assert(probeGetInstanceProcAddr(inst,"xrBeginFrame",&fn)==XR_SUCCESS && fn==reinterpret_cast<PFN_xrVoidFunction>(probeBeginFrame));
  assert(probeGetInstanceProcAddr(inst,"xrDestroySwapchain",&fn)==XR_SUCCESS && fn==reinterpret_cast<PFN_xrVoidFunction>(probeDestroySwapchain));
  assert(probeBeginSession(created,&begin)==XR_SUCCESS);
  auto out=state();assert(probeWaitFrame(created,nullptr,&out)==XR_SUCCESS);
  assert(probeBeginFrame(created,nullptr)==XR_SUCCESS);auto e=endInfo();assert(probeEndFrame(created,&e)==XR_SUCCESS);r.entered(2);
  assert(probeWaitFrame(created,nullptr,&out)==XR_SUCCESS && sessions.at(created).displayTime==out.predictedDisplayTime);
  assert(probeBeginFrame(created,nullptr)==XR_SUCCESS && r.begins==1);
  r.allow(2);assert(probeEndFrame(created,&e)==XR_SUCCESS);r.entered(3);
  XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};sc.width=sc.height=100;sc.arraySize=sc.faceCount=sc.sampleCount=1;
  XrSwapchain swapchain{};assert(probeCreateSwapchain(created,&sc,&swapchain)==XR_SUCCESS && swapchainSessions.at(swapchain)==created);
  m75SwapchainImage[swapchain]={1,2,3};r.allow(3);
  assert(probeDestroySwapchain(swapchain)==XR_SUCCESS && !swapchainSessions.count(swapchain) && !m75SwapchainImage.count(swapchain));
  assert(r.waits==3 && r.begins==3 && r.ends==3 && r.layers.back()==0);
  // Prime once more, then exit with a worker wait outstanding.
  r.allow(4);assert(probeWaitFrame(created,nullptr,&out)==XR_SUCCESS);
  assert(probeBeginFrame(created,nullptr)==XR_SUCCESS);assert(probeEndFrame(created,&e)==XR_SUCCESS);r.entered(5);r.allow(5);
  if(instanceExit){assert(probeDestroyInstance(inst)==XR_SUCCESS && !instances.count(inst) && !sessions.count(created));}
  else {
   assert(probeEndSession(created)==XR_SUCCESS && r.waits==r.begins && r.begins==r.ends);
   // Session restart uses normal wait first and can safely re-enable turbo.
   assert(probeBeginSession(created,&begin)==XR_SUCCESS);r.allow(100);
   assert(probeWaitFrame(created,nullptr,&out)==XR_SUCCESS && probeBeginFrame(created,nullptr)==XR_SUCCESS && probeEndFrame(created,&e)==XR_SUCCESS);
   assert(probeDestroySession(created)==XR_SUCCESS && !sessions.count(created));
  }
  assert(r.inFlight==0 && r.waits==r.begins && r.begins==r.ends);
 }
 assert(sessionEnds==2 && sessionDestroys==2 && swapchainDestroys==2 && instanceDestroys==1);
 assert(std::filesystem::is_empty(directory)); // Level=off never creates a layer log.
 std::filesystem::remove_all(directory);
 std::cout<<"Real OpenXR hooks: disabled timing, no-gaze ownership, turbo option, proc routing, swapchain drain, restart, session/instance teardown and logging off passed\n";
}
