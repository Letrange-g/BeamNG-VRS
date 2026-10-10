#pragma once
#include "openxr_turbo.hpp"
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
namespace beamng_turbo_fixture {
struct Runtime {
 std::mutex mutex;std::condition_variable cv;
 unsigned waits=0,begins=0,ends=0,permitted=1,inFlight=0,maxInFlight=0;
 bool waited=false,begun=false,render=true;
 unsigned failWait=0,failBegin=0,failEnd=0;
 XrResult special=XR_SUCCESS;
 XrTime time=1000000000;
 const void* waitNext=nullptr;void* stateNext=nullptr;const void* beginNext=nullptr;
 std::vector<uint32_t> layers;
 std::thread::id worker;
 void allow(unsigned n){std::lock_guard lock(mutex);permitted=n;cv.notify_all();}
 void entered(unsigned n){std::unique_lock lock(mutex);assert(cv.wait_for(lock,2s,[&]{return waits>=n;}));}
};
inline Runtime* runtime=nullptr;
XRAPI_ATTR XrResult XRAPI_CALL waitFrame(XrSession,const XrFrameWaitInfo* info,XrFrameState* state){
 auto& r=*runtime;std::unique_lock lock(r.mutex);
 const auto n=++r.waits;++r.inFlight;r.maxInFlight=std::max(r.maxInFlight,r.inFlight);assert(r.maxInFlight==1);
 r.waitNext=info?info->next:nullptr;r.stateNext=state->next;r.cv.notify_all();
 r.cv.wait(lock,[&]{return r.permitted>=n;});--r.inFlight;
 if(r.failWait==n)return XR_ERROR_RUNTIME_FAILURE;
 assert(!r.waited);r.waited=true;r.time+=100000000;
 state->predictedDisplayTime=r.time;state->predictedDisplayPeriod=100000000;state->shouldRender=r.render;
 if(n>1){if(r.worker==std::thread::id{})r.worker=std::this_thread::get_id();}
 return r.special;
}
XRAPI_ATTR XrResult XRAPI_CALL beginFrame(XrSession,const XrFrameBeginInfo* info){
 auto& r=*runtime;std::lock_guard lock(r.mutex);++r.begins;r.beginNext=info?info->next:nullptr;
 assert(r.inFlight==0 && r.waited);r.waited=false;
 if(r.failBegin==r.begins)return XR_ERROR_RUNTIME_FAILURE;
 const bool discarded=r.begun;r.begun=true;return discarded?XR_FRAME_DISCARDED:XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL endFrame(XrSession,const XrFrameEndInfo* info){
 auto& r=*runtime;std::lock_guard lock(r.mutex);++r.ends;
 assert(r.inFlight==0 && r.begun);r.begun=false;r.layers.push_back(info->layerCount);
 return r.failEnd==r.ends?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;
}
XrSession session=reinterpret_cast<XrSession>(uintptr_t(1));
XrFrameState state(){return {XR_TYPE_FRAME_STATE};}
XrFrameEndInfo endInfo(){XrFrameEndInfo e{XR_TYPE_FRAME_END_INFO};e.displayTime=1000000000;e.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;return e;}
void prime(beamng_turbo::Scheduler& s){auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);}
}
