#include "openxr_turbo.hpp"
#include <cassert>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "turbo_runtime_fixture.hpp"
using namespace beamng_turbo_fixture;
int main(){
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,false);s.start();r.allow(100);
  for(int i=0;i<30;++i)prime(s);
  s.stop();assert(r.waits==30 && r.begins==30 && r.ends==30 && s.stats().predicted==0);
 }
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto future=std::async(std::launch::async,[&]{auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);return out;});
  assert(future.wait_for(100ms)==std::future_status::ready);const auto out=future.get();
  assert(out.predictedDisplayTime>r.time && out.shouldRender && s.stats().predicted==1);
  assert(s.begin(nullptr)==XR_SUCCESS && r.begins==1);
  auto end=std::async(std::launch::async,[&]{auto e=endInfo();return s.end(&e);});
  assert(end.wait_for(20ms)==std::future_status::timeout);assert(r.ends==1);
  r.allow(2);assert(end.get()==XR_SUCCESS);r.entered(3);
  auto stop=std::async(std::launch::async,[&]{s.stop();});
  assert(stop.wait_for(20ms)==std::future_status::timeout);r.allow(3);stop.get();
  assert(r.waits==3 && r.begins==3 && r.ends==3 && r.layers.back()==0 && r.inFlight==0);
  // Session restart creates a new worker only after its first completed frame.
  s.start();r.allow(100);prime(s);s.stop();assert(r.waits==r.begins && r.begins==r.ends);
 }
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  for(unsigned n=2;n<=51;++n){
   auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS && out.predictedDisplayTime>0);
   assert(s.begin(nullptr)==XR_SUCCESS);r.allow(n);auto e=endInfo();e.displayTime=out.predictedDisplayTime;assert(s.end(&e)==XR_SUCCESS);r.entered(n+1);
  }
  r.allow(52);s.stop();assert(r.waits==52 && r.begins==52 && r.ends==52 && r.maxInFlight==1);
 }
 {
  Runtime r;runtime=&r;r.render=false;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto future=std::async(std::launch::async,[&]{auto out=state();const auto result=s.wait(nullptr,&out);assert(!out.shouldRender);return result;});
  assert(future.wait_for(20ms)==std::future_status::timeout);r.allow(2);assert(future.get()==XR_SUCCESS);
  assert(s.stats().predicted==0);assert(s.begin(nullptr)==XR_SUCCESS);r.allow(3);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);s.stop();
 }
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  int extension=0;XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};info.next=&extension;
  auto out=state();out.next=&extension;r.allow(3);
  assert(s.wait(&info,&out)==XR_SUCCESS);assert(r.waitNext==&extension && r.stateNext==&extension);
  assert(r.waits==3 && r.begins==2 && r.ends==2);
  assert(s.begin(nullptr)==XR_SUCCESS);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);s.stop();assert(r.waits==3);
 }
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);r.allow(2);
  int extension=0;XrFrameBeginInfo b{XR_TYPE_FRAME_BEGIN_INFO};b.next=&extension;
  assert(s.begin(&b)==XR_SUCCESS && r.beginNext==&extension);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);s.stop();assert(r.waits==2);
 }
 {
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);r.allow(3);
  // Existing app pipelining switches to synchronous forwarding, without a
  // second runtime wait before the deferred begin has completed.
  assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_FRAME_DISCARDED);
  auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);s.stop();assert(r.waits==3 && r.ends==2);
 }
 for(bool knownError:{false,true}){
  Runtime r;runtime=&r;r.failWait=2;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto out=state();
  if(knownError){
   r.allow(2);const auto result=s.wait(nullptr,&out);
   if(result==XR_SUCCESS){assert(s.begin(nullptr)==XR_SUCCESS);auto e=endInfo();assert(s.end(&e)==XR_ERROR_RUNTIME_FAILURE);}
   else assert(result==XR_ERROR_RUNTIME_FAILURE);
  }
  else {assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);r.allow(2);auto e=endInfo();assert(s.end(&e)==XR_ERROR_RUNTIME_FAILURE);}
  r.allow(3);assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);
  s.stop();assert(r.waits==3 && r.begins==2 && r.ends==2 && s.stats().failures>0);
 }
 for(bool beginError:{false,true}){
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);
  if(beginError)r.failBegin=2;else r.failEnd=2;r.allow(2);auto e=endInfo();assert(s.end(&e)==XR_ERROR_RUNTIME_FAILURE);
  s.stop();assert(r.waits==2 && r.begins==2 && r.ends==(beginError?1u:2u));
 }
 {
  Runtime r;runtime=&r;r.special=XR_SESSION_LOSS_PENDING;r.allow(100);
  beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();auto out=state();
  assert(s.wait(nullptr,&out)==XR_SESSION_LOSS_PENDING);assert(s.begin(nullptr)==XR_SUCCESS);auto e=endInfo();assert(s.end(&e)==XR_SUCCESS);s.stop();assert(r.waits==1);
 }
 for(bool appOwnsFrame:{false,true}){
  Runtime r;runtime=&r;beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);
  if(appOwnsFrame){auto out=state();assert(s.wait(nullptr,&out)==XR_SUCCESS);assert(s.begin(nullptr)==XR_SUCCESS);}
  r.allow(2);s.drain();assert(r.inFlight==0 && r.begins==2);
  if(appOwnsFrame){auto e=endInfo();r.allow(3);assert(s.end(&e)==XR_SUCCESS);}
  r.allow(100);s.stop();assert(r.waits==r.begins && r.begins==r.ends);
 }
 {
  Runtime r;runtime=&r;
  {beamng_turbo::Scheduler s(session,waitFrame,beginFrame,endFrame,true);s.start();prime(s);r.entered(2);r.allow(2);}
  assert(r.inFlight==0 && r.begins==1 && r.ends==1); // Destructor does not call begin/end.
 }
 std::cout<<"Turbo: normal forwarding, one-frame overlap, ordered submission, shouldRender, extension chains, existing pipelining, failures, drain, stop and restart passed\n";
}
