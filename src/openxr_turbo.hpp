#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace beamng_turbo {
// One session, one persistent wait worker. App frame operations are serialized;
// the worker only calls the downstream xrWaitFrame and never accesses layer maps.
class Scheduler {
 // "App" means BeamNG; "downstream" means the next OpenXR layer/runtime.
 // Normal: idle -> waited -> begun -> idle.
 // Turbo:  prefetched -> turboWaited -> turboBegun -> (wait/begin/end) -> prefetched.
 enum class Phase {
  idle,        // No app frame and no outstanding downstream wait.
  waited,      // Downstream wait returned; app still needs to begin this frame.
  begun,       // App and downstream have both begun the same frame.
  prefetched,  // Worker is waiting for the NEXT frame; app has not requested it.
  turboWaited, // App received next-frame timing; downstream wait may still block.
  turboBegun   // App began rendering; downstream begin is deferred until submit.
 };
 XrSession session_;
 PFN_xrWaitFrame wait_;
 PFN_xrBeginFrame begin_;
 PFN_xrEndFrame end_;
 std::mutex operation_,work_;
 std::condition_variable changed_;
 std::thread worker_;
 bool stop_=false,job_=false,complete_=false;
 bool configured_,active_=false,running_=false;
 Phase phase_=Phase::idle;
 XrFrameState pending_{XR_TYPE_FRAME_STATE},last_{XR_TYPE_FRAME_STATE};
 XrResult pendingResult_=XR_SUCCESS,lastResult_=XR_SUCCESS;
 XrTime lastDelivered_=0;
 int64_t lastReturnedNs_=0;
 XrEnvironmentBlendMode blend_=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
 std::atomic<uint64_t> normal_{0},predicted_{0},ready_{0},blocked_{0},failures_{0};
 std::atomic<uint64_t> runtimeWaitNs_{0},appWaitNs_{0},endWaitNs_{0};
 static int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(
   std::chrono::steady_clock::now().time_since_epoch()).count();}
 static void copyState(XrFrameState* out,const XrFrameState& state){
  out->predictedDisplayTime=state.predictedDisplayTime;
  out->predictedDisplayPeriod=state.predictedDisplayPeriod;
  out->shouldRender=state.shouldRender;
 }
 XrResult runtimeWait(const XrFrameWaitInfo* info,XrFrameState* state){
  const auto start=now();const auto result=wait_(session_,info,state);
  runtimeWaitNs_.fetch_add(uint64_t(now()-start),std::memory_order_relaxed);
  return result;
 }
 // The callback runs without either layer-global stateMutex or the app lock.
 // Publish all result fields under work_ before waking a consumer.
 void run(){
  std::unique_lock lock(work_);
  for(;;){
   changed_.wait(lock,[&]{return stop_||job_;});
   if(stop_)return;
   job_=false;lock.unlock();
   XrFrameState state{XR_TYPE_FRAME_STATE};
   const auto result=runtimeWait(nullptr,&state);const auto returned=now();
   lock.lock();pending_=state;pendingResult_=result;lastReturnedNs_=returned;
   complete_=true;changed_.notify_all();
  }
 }
 // A completed end is the only place that queues a job. Never queue two waits
 // before consuming the first; OpenXR requires a begin between such waits.
 bool prefetch(){
  try {
   if(!worker_.joinable())worker_=std::thread([this]{run();});
  } catch(...) {active_=false;failures_.fetch_add(1);return false;}
  std::lock_guard lock(work_);complete_=false;job_=true;
  phase_=Phase::prefetched;changed_.notify_all();return true;
 }
 // This can block at submission/teardown. A downstream wait is not cancellable;
 // never detach the thread and let it outlive its session or DLL dispatch.
 XrResult consume(){
  std::unique_lock lock(work_);changed_.wait(lock,[&]{return complete_;});
  last_=pending_;lastResult_=pendingResult_;complete_=false;
  if(lastResult_!=XR_SUCCESS){active_=false;failures_.fetch_add(1);}
  return lastResult_;
 }
 // Discard an unconsumed prefetch before forwarding an extensible app wait or
 // closing a session. Every successful downstream wait gets a begin before end.
 XrResult discard(){
  const auto result=consume();phase_=Phase::idle;
  if(XR_FAILED(result))return result;
  const auto begun=begin_(session_,nullptr);
  if(XR_FAILED(begun)){active_=false;return begun;}
  XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
  empty.displayTime=last_.predictedDisplayTime;empty.environmentBlendMode=blend_;
  const auto ended=end_(session_,&empty);
  if(ended!=XR_SUCCESS)active_=false;
  return ended;
 }
 void shutdownLocked(){
  running_=false;active_=false;
  if(phase_==Phase::prefetched || phase_==Phase::turboWaited || phase_==Phase::turboBegun)
   discard();
  {std::lock_guard lock(work_);stop_=true;changed_.notify_all();}
  if(worker_.joinable())worker_.join();
  phase_=Phase::idle;
 }
public:
 struct Stats {uint64_t normal,predicted,ready,blocked,failures,runtimeWaitNs,appWaitNs,endWaitNs;};
 Scheduler(XrSession session,PFN_xrWaitFrame wait,PFN_xrBeginFrame begin,PFN_xrEndFrame end,bool configured)
  :session_(session),wait_(wait),begin_(begin),end_(end),configured_(configured&&wait&&begin&&end){}
 // Explicit lifecycle hooks call stop() while runtime handles are alive.
 // Destruction only joins: do not issue fresh OpenXR calls during DLL teardown.
 ~Scheduler(){
  std::lock_guard lock(operation_);running_=false;active_=false;
  {std::lock_guard workLock(work_);stop_=true;changed_.notify_all();}
  if(worker_.joinable())worker_.join();
 }
 Scheduler(const Scheduler&)=delete;
 Scheduler& operator=(const Scheduler&)=delete;
 void start(){std::lock_guard lock(operation_);stop_=false;running_=true;active_=configured_;lastDelivered_=0;}
 void stop(){std::lock_guard lock(operation_);shutdownLocked();}
 Stats stats() const{return {normal_.load(),predicted_.load(),ready_.load(),blocked_.load(),failures_.load(),
   runtimeWaitNs_.load(),appWaitNs_.load(),endWaitNs_.load()};}
 // Return the real frame state when ready. While the worker blocks, extrapolate
 // only a short, renderable frame; a hitch or shouldRender=false uses real data.
 XrResult wait(const XrFrameWaitInfo* info,XrFrameState* out){
  std::lock_guard lock(operation_);const auto start=now();
  if(!out || out->type!=XR_TYPE_FRAME_STATE || (info&&info->type!=XR_TYPE_FRAME_WAIT_INFO))
   return XR_ERROR_VALIDATION_FAILURE;
  // An app which already pipelines needs its original scheduling. Finish our
  // deferred begin before passing its next wait downstream; no second prefetch.
  if(phase_==Phase::turboBegun){
   const auto result=consume();phase_=Phase::idle;active_=false;
   if(XR_FAILED(result))return result;
   const auto begun=begin_(session_,nullptr);if(XR_FAILED(begun))return begun;
   phase_=Phase::begun;
  }
  if(phase_==Phase::waited || phase_==Phase::turboWaited)return XR_ERROR_CALL_ORDER_INVALID;
  if(phase_==Phase::begun)active_=false;
  if(phase_==Phase::prefetched && ((info&&info->next)||out->next)){
   active_=false;const auto result=discard();if(XR_FAILED(result))return result;
  }
  if(phase_==Phase::prefetched){
   bool completed=false;
   int64_t elapsed=0;
   {std::lock_guard lock(work_);completed=complete_;elapsed=std::max<int64_t>(0,now()-lastReturnedNs_);}
   const bool canPredict=lastResult_==XR_SUCCESS && last_.shouldRender && last_.predictedDisplayPeriod>0 &&
    last_.predictedDisplayPeriod<=1000000000 && elapsed<=2*last_.predictedDisplayPeriod &&
    last_.predictedDisplayTime<=std::numeric_limits<XrTime>::max()-2*last_.predictedDisplayPeriod &&
    lastDelivered_<std::numeric_limits<XrTime>::max();
   if(completed || !canPredict){
    // Keep the result until begin/end can consume it. Ready/error results are
    // returned verbatim instead of claiming a successful speculative frame.
    std::unique_lock lock(work_);changed_.wait(lock,[&]{return complete_;});
    copyState(out,pending_);lastResult_=pendingResult_;
    if(XR_FAILED(pendingResult_)){active_=false;phase_=Phase::idle;complete_=false;failures_.fetch_add(1);return pendingResult_;}
    (completed?ready_:blocked_).fetch_add(1);
   }else{
    copyState(out,last_);
    out->predictedDisplayTime=last_.predictedDisplayTime+std::max<int64_t>(last_.predictedDisplayPeriod,elapsed);
    predicted_.fetch_add(1);
   }
   if(lastDelivered_==std::numeric_limits<XrTime>::max()){
    active_=false;return XR_ERROR_TIME_INVALID;
   }
   // Extrapolation may run slightly ahead of the next real timestamp. OpenXR
   // requires app-facing predicted display times to increase monotonically.
   out->predictedDisplayTime=std::max(out->predictedDisplayTime,lastDelivered_+1);
   lastDelivered_=out->predictedDisplayTime;phase_=Phase::turboWaited;
   appWaitNs_.fetch_add(uint64_t(now()-start));return lastResult_;
  }
  const auto result=runtimeWait(info,out);normal_.fetch_add(1);
  if(XR_SUCCEEDED(result)){
   last_=*out;last_.next=nullptr;lastResult_=result;lastReturnedNs_=now();
   lastDelivered_=out->predictedDisplayTime;phase_=Phase::waited;
  }else{active_=false;failures_.fetch_add(1);}
  appWaitNs_.fetch_add(uint64_t(now()-start));return result;
 }
 // An extension chain cannot be saved by pointer until end: its memory belongs
 // to the caller. In that case resolve the wait and forward begin immediately.
 XrResult begin(const XrFrameBeginInfo* info){
  std::lock_guard lock(operation_);
  if(info&&info->type!=XR_TYPE_FRAME_BEGIN_INFO)return XR_ERROR_VALIDATION_FAILURE;
  if(phase_==Phase::turboWaited){
   if(!info||!info->next){phase_=Phase::turboBegun;return XR_SUCCESS;}
   active_=false;const auto result=consume();phase_=Phase::idle;if(XR_FAILED(result))return result;
  }else if(phase_==Phase::prefetched)return XR_ERROR_CALL_ORDER_INVALID;
  const auto result=begin_(session_,info);
  if(XR_SUCCEEDED(result))phase_=Phase::begun;else active_=false;
  return result;
 }
 // GPU work and submitted layer pointers remain BeamNG-owned. Before forwarding
 // them, finish the real wait and real begin; then queue exactly one next wait.
 XrResult end(const XrFrameEndInfo* info){
  std::lock_guard lock(operation_);
  if(!info||info->type!=XR_TYPE_FRAME_END_INFO)return XR_ERROR_VALIDATION_FAILURE;
  if(phase_==Phase::turboBegun){
   const auto start=now();const auto waited=consume();endWaitNs_.fetch_add(uint64_t(now()-start));
   phase_=Phase::idle;if(XR_FAILED(waited))return waited;
   const auto begun=begin_(session_,nullptr);if(XR_FAILED(begun)){active_=false;return begun;}
   phase_=Phase::begun;
  }
  if(phase_==Phase::prefetched || phase_==Phase::turboWaited)return XR_ERROR_CALL_ORDER_INVALID;
  const bool completedFrame=phase_==Phase::begun;
  const auto result=end_(session_,info);
  if(XR_SUCCEEDED(result)){
   phase_=Phase::idle;blend_=info->environmentBlendMode;
   if(result==XR_SUCCESS && completedFrame && running_ && active_ && lastResult_==XR_SUCCESS && !info->next)prefetch();
   else if(result!=XR_SUCCESS || info->next)active_=false;
  }else{active_=false;failures_.fetch_add(1);}
  return result;
 }
 // Swapchain destruction must not race the background wait. Do not invent a
 // second frame wait, nor end an app-owned frame during a resolution change.
 void drain(){
  std::lock_guard lock(operation_);
  if(phase_==Phase::prefetched){discard();return;}
  if(phase_==Phase::turboWaited || phase_==Phase::turboBegun){
   const bool begun=phase_==Phase::turboBegun;const auto result=consume();
   phase_=XR_FAILED(result)?Phase::idle:Phase::waited;
   if(begun && XR_SUCCEEDED(result)){
    const auto r=begin_(session_,nullptr);phase_=XR_SUCCEEDED(r)?Phase::begun:Phase::idle;
    if(XR_FAILED(r))active_=false;
   }
  }
 }
};
} // namespace beamng_turbo
