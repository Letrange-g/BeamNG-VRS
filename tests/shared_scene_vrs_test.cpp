#include "shared_scene_vrs.hpp"
#include <cassert>
#include <iostream>
using namespace beamng_shared;
struct Fixture {
 Tracker t; const Id set=3;
 Fixture(){
  t.extent(100,80);t.image(10,100,80,true);t.image(11,100,80,true);
  t.image(20,100,80,false);t.image(21,100,80,false);t.image(30,50,40,true);
  t.allocateSet(3,4);t.write(3,0,0,10);
  t.allocateCommand(1,2,true);t.allocateCommand(2,2,true);
 }
 void producer(Id cmd=1,Id image=10,Id pass=50,unsigned draws=128){
  t.beginPass(cmd,image,-1,pass,true);for(unsigned i=0;i<draws;++i)t.draw(cmd);t.endPass(cmd,100);
 }
 void consumer(Id cmd=2,int eye=0){
  t.beginPass(cmd,eye?21:20,eye,60);t.bind(cmd,5,0,1,&set);t.draw(cmd);t.endPass(cmd,100);
 }
 void record(int eye=0){t.resetCommand(1,true);producer();t.resetCommand(2,true);consumer(2,eye);}
 void confirm(){for(unsigned i=0;i<3;++i){record(i%2);t.submitted(100,{1,2},100+i);}}
};
int main(){
 {
  Fixture f;f.record();assert(!f.t.ready(10,50,100));
  f.t.submitted(100,{1,2},100);assert(!f.t.ready(10,50,100));
  f.record(1);f.t.submitted(100,{1,2},101);assert(!f.t.ready(10,50,101));
  f.record();f.t.submitted(100,{1,2},102);assert(f.t.ready(10,50,102));
  assert(!f.t.ready(10,51,102) && !f.t.ready(10,50,2103) && !f.t.ready(10,50,101));
  // Both eyes corroborate the same shared target without assigning an eye.
  assert(f.t.evidence.at(10).observations==3);
 }
 {
  Fixture f;f.record();f.consumer();f.consumer(2,1);f.t.submitted(100,{1,2},100);
  assert(f.t.evidence.at(10).observations==1 && !f.t.ready(10,50,100));
  // CPU recording order is irrelevant; read-before-producer cannot learn.
  Fixture wrong;wrong.record();wrong.t.submitted(100,{2,1},100);assert(wrong.t.evidence.empty());
  // Abandoned/re-recorded producer and reset pool cannot authorize a read.
  Fixture abandoned;abandoned.record();abandoned.t.resetCommand(1,true);
  abandoned.t.submitted(100,{1,2},100);assert(abandoned.t.evidence.empty());
  abandoned.t.resetCommands(2);abandoned.t.submitted(100,{1,2},101);assert(abandoned.t.evidence.empty());
 }
 {
  Fixture f;
  for(unsigned i=0;i<3;++i){f.record(i%2);f.t.resetCommand(1,true);f.producer();f.producer(1,10,50,1);
   // An unrelated ineligible render pass must not erase submitted history.
   f.t.beginPass(1,30,-1,70);f.t.draw(1);f.t.endPass(1,100);f.t.submitted(100,{1,2},100+i);}
  assert(f.t.ready(10,50,102) && f.t.stats.retainedSmallWrites==3);
  f.record();f.producer(1,10,51,1);f.t.submitted(100,{1,2},103);assert(f.t.ready(10,50,103));
  f.record();f.producer(1,10,51,128);f.t.submitted(100,{1,2},104);assert(!f.t.ready(10,50,104) && !f.t.ready(10,51,104));
  Fixture small;
  for(unsigned i=0;i<3;++i){small.record();small.t.resetCommand(1,true);small.producer(1,10,50,127);small.t.submitted(100,{1,2},100+i);}
  assert(small.t.evidence.empty() && small.t.stats.noProducer==3);
 }
 {
  Fixture f;f.confirm();f.t.eraseImage(10);f.t.image(10,100,80,true);
  f.t.submitted(100,{1,2},104);assert(!f.t.ready(10,50,104) && f.t.stats.staleEvents);
  f.t.write(3,0,0,10);f.confirm();assert(f.t.ready(10,50,102));
  f.t.erasePass(50);f.t.submitted(100,{1,2},103);assert(!f.t.ready(10,50,103));
  f.confirm();f.t.extent(200,160);assert(f.t.evidence.empty());
  f.t.extent(100,80);assert(!f.t.ready(10,50,104));
 }
 {
  Fixture f;f.t.unknown(3);f.confirm();assert(f.t.evidence.empty() && f.t.stats.ambiguous==3);
  Fixture missing;missing.t.eraseSet(3);missing.confirm();assert(missing.t.evidence.empty() && missing.t.stats.setMisses);
  Fixture reused;reused.record();reused.t.resetCommand(2,true);
  reused.t.beginPass(2,20,0,60);reused.t.bind(2,5,0,1,&reused.set);
  reused.t.eraseSet(3);reused.t.allocateSet(3,4);reused.t.write(3,0,0,10);
  reused.t.draw(2);reused.t.endPass(2,100);reused.t.submitted(100,{1,2},100);assert(reused.t.evidence.empty());
  Fixture ambiguous;ambiguous.t.write(3,1,0,11);ambiguous.confirm();assert(ambiguous.t.evidence.empty());
 }
 {
  Fixture f;f.confirm();f.record();f.t.invalidate(1);f.t.submitted(100,{1,2},103);
  assert(!f.t.ready(10,50,103) && f.t.stats.barriers);
  f.confirm();f.t.submitted(101,{1,2},103);assert(!f.t.ready(10,50,103) && f.t.stats.multipleQueues==1);
  f.confirm();assert(!f.t.ready(10,50,102)); // Multi-queue lockout is persistent.
  Fixture secondary;secondary.t.allocateCommand(1,2,false);secondary.confirm();assert(secondary.t.evidence.empty());
 }
 {
  Fixture f;f.t.allocateCommand(9,9,true);f.t.resetCommand(9,true);
  f.t.submitted(999,{9},90); // Empty startup upload work does not choose a queue.
  for(unsigned i=0;i<3;++i){f.record(i%2);f.t.resetCommand(1,true);f.t.invalidate(1);f.producer();
   f.t.submitted(100,{1,2},100+i);}
  assert(f.t.ready(10,50,102) && !f.t.stats.multipleQueues);
  f.t.submitted(999,{9},103);assert(f.t.ready(10,50,103));
  f.record();f.t.submitted(999,{1,2},104);assert(!f.t.ready(10,50,104) && f.t.stats.multipleQueues==1);
  Fixture oneEye;for(unsigned i=0;i<3;++i){oneEye.record();oneEye.t.submitted(100,{1,2},100+i);}
  assert(!oneEye.t.ready(10,50,102));
  Fixture expired;expired.confirm();expired.t.resetCommand(1,true);
  expired.t.resetCommand(2,true);expired.consumer();expired.consumer(2,1);
  expired.t.submitted(100,{2},2000);assert(expired.t.ready(10,50,2102));
  assert(!expired.t.ready(10,50,2103)); // Consumer traffic cannot extend heavy age.
  Fixture staleEye;staleEye.confirm();staleEye.record(0);staleEye.t.submitted(100,{1,2},2102);
  assert(!staleEye.t.ready(10,50,2102)); // One eye cannot keep the other eye fresh.
 }
 {
  Fixture f;f.t.resetCommand(1,true);f.producer();f.t.resetCommand(2,true);
  f.t.beginPass(2,20,0,60);f.t.bind(2,5,0,1,&f.set);f.t.draw(2);
  const auto scans=f.t.stats.descriptorScans;
  for(unsigned i=0;i<100;++i)f.t.draw(2);
  assert(f.t.stats.descriptorScans==scans && f.t.stats.scanSkips>=100);
  f.t.write(3,1,0,11);f.t.draw(2);f.t.endPass(2,100);f.t.submitted(100,{1,2},100);
  assert(f.t.evidence.empty() && f.t.stats.descriptorScans==scans+1);
 }
 {
  Fixture changed;changed.record();changed.t.write(3,0,0,11);changed.t.submitted(100,{1,2},100);
  assert(changed.t.evidence.empty() && changed.t.stats.staleDescriptors==1);
  Fixture unrelated;unrelated.record();unrelated.t.allocateSet(6,4);unrelated.t.write(6,0,0,11);
  unrelated.t.submitted(100,{1,2},100);assert(unrelated.t.evidence.at(10).observations==1 && !unrelated.t.stats.staleDescriptors);
  // Copy, unknown/template updates, destruction and reuse invalidate a watched set.
  Fixture copied;copied.record();copied.t.allocateSet(6,4);copied.t.write(6,0,0,11);
  copied.t.copy(6,0,0,3,0,0);copied.t.submitted(100,{1,2},100);
  assert(copied.t.evidence.empty() && copied.t.stats.staleDescriptors==1);
  Fixture unknown;unknown.record();unknown.t.unknown(3);unknown.t.submitted(100,{1,2},100);
  assert(unknown.t.evidence.empty() && unknown.t.stats.staleDescriptors==1);
  Fixture recycled;recycled.record();recycled.t.eraseSet(3);recycled.t.allocateSet(3,4);recycled.t.write(3,0,0,10);
  recycled.t.submitted(100,{1,2},100);assert(recycled.t.evidence.empty() && recycled.t.stats.staleDescriptors==1);
  // An initially empty bound set must be watched too, including across rebinds.
  Fixture empty;empty.t.allocateSet(6,4);empty.t.resetCommand(1,true);empty.producer();empty.t.resetCommand(2,true);
  empty.t.beginPass(2,20,0,60);const Id bound[]={3,6};empty.t.bind(2,5,0,2,bound);empty.t.draw(2);
  empty.t.bind(2,6,0,1,&empty.set);empty.t.draw(2);empty.t.endPass(2,100);
  empty.t.write(6,0,0,11);empty.t.submitted(100,{1,2},100);
  assert(empty.t.evidence.empty() && empty.t.stats.staleDescriptors==1);
  // A rejected event neither erases nor refreshes prior valid observations.
  Fixture previous;previous.confirm();previous.record();previous.t.write(3,0,0,11);previous.t.submitted(100,{1,2},103);
  assert(previous.t.evidence.at(10).tick==102 && previous.t.stats.staleDescriptors==1);
 }
 {
  Fixture f;
  for(unsigned i=0;i<300;++i)f.t.write(3,i,0,0);
  f.t.write(3,0,0,10);f.confirm();assert(f.t.ready(10,50,102) && !f.t.sets.at(3).unknown);
  f.t.allocateSet(6,4);f.t.copy(3,0,0,6,2,0);assert(f.t.sets.at(6).images.at(uint64_t(2)<<32)==10);
  f.t.write(3,0,0,0);f.t.copy(3,0,0,6,2,0);assert(f.t.sets.at(6).images.empty());
  for(unsigned i=0;i<129;++i)f.t.write(3,i,0,10);
  assert(f.t.sets.at(3).unknown && f.t.stats.slotDrops==1);
  f.t.resetDescriptors(4);assert(f.t.sets.empty());
  for(Id i=1;i<=131073;++i)f.t.allocateSet(i,8);
  assert(f.t.sets.size()==131072 && f.t.stats.setDrops==1);
  f.t.resetDescriptors(8);assert(f.t.sets.empty());
  f.t.resetCommands(2,true);assert(f.t.commands.empty());
 }
 {
  Fixture f;f.record();for(unsigned i=0;i<257;++i)f.producer(1,10,50,1);
  f.t.submitted(100,{1,2},100);assert(f.t.evidence.empty() && f.t.stats.eventDrops==1);
 }
 std::cout<<"Shared scene submission evidence, ambiguity, bounds and lifetime passed\n";
}
