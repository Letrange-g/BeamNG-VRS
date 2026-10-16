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
// The UNORM pilot learns an ordered producer/scene/final-eye association.
// A shared depth handle alone, or a pipeline merely bound without a draw,
// must never authorize coarse shading.
struct UnormFixture:Fixture {
 UnormFixture(){t.image(40,100,80,false,true);t.image(41,100,80,false,true);t.depthImage(70);t.depthImage(71);}
 void unorm(Id image=40,Id depth=70,Id first=101){
  t.beginPass(1,image,-1,80,false,depth,true);
  t.pipeline(1,999); // Bound but never drawn: must not enter the cohort.
  t.pipeline(1,first);for(unsigned n=0;n<64;++n)t.draw(1);
  t.pipeline(1,102);for(unsigned n=0;n<64;++n)t.draw(1);
  t.endPass(1,100);
 }
 void scene(Id depth=70){t.beginPass(1,10,-1,50,true,depth);for(unsigned n=0;n<128;++n)t.draw(1);t.endPass(1,100);}
 void chain(int eye=0){t.resetCommand(1,true);unorm();scene();consumer(1,eye);}
 void confirmUnorm(){for(unsigned i=0;i<3;++i){chain(i%2);t.submitted(100,{1},100+i);}}
};
void depthWriteTests(){
 {
  UnormFixture copied;
  for(unsigned i=0;i<3;++i){
   copied.t.resetCommand(1,true);copied.unorm();
   copied.t.depthWrite(1,71); // Copy source depth 70 into scratch 71.
   copied.t.depthWrite(1,71); // Clearing scratch also leaves source intact.
   copied.scene();copied.consumer(1,i%2);copied.t.submitted(100,{1},100+i);
  }
  assert(copied.t.readyUnorm(40,80,70,102) && copied.t.ready(10,50,102));
  assert(copied.t.stats.scopedDepthWrites==6 && !copied.t.stats.barriers);
 }
 {
  for(unsigned position=0;position<3;++position){
   UnormFixture overwritten;
   for(unsigned i=0;i<3;++i){
    overwritten.t.resetCommand(1,true);overwritten.unorm();
    if(position==0)overwritten.t.depthWrite(1,70);
    overwritten.scene();if(position==1)overwritten.t.depthWrite(1,70);
    overwritten.consumer(1,i%2);if(position==2)overwritten.t.depthWrite(1,70);
    overwritten.t.submitted(100,{1},100+i);
   }
   assert(!overwritten.t.readyUnorm(40,80,70,102) && overwritten.t.unormEvidence.empty());
   overwritten.confirmUnorm();assert(overwritten.t.readyUnorm(40,80,70,102));
  }
 }
 {
  UnormFixture f;f.confirmUnorm();f.t.resetCommand(2,true);f.t.depthWrite(2,70);
  assert(f.t.readyUnorm(40,80,70,103)); // Recording has no submitted effect.
  f.t.resetCommand(2,true);f.t.submitted(100,{2},103);
  assert(f.t.readyUnorm(40,80,70,103)); // Abandoned recording cannot revoke it.
  f.t.beginPass(1,40,-1,80,false,70,true);assert(f.t.prepareUnorm(1,103));f.t.pipeline(1,101);
  f.t.depthWrite(2,70);f.t.submitted(100,{2},103);
  assert(!f.t.allowUnormPipeline(1,101,103) && !f.t.readyUnorm(40,80,70,103));
  // Same queue: a new complete chain AFTER the write may train normally.
  for(unsigned i=0;i<3;++i){f.chain(i%2);f.t.submitted(100,{2,1},104+i);}
  assert(!f.t.readyUnorm(40,80,70,106)); // Each repeated overwrite discards prior proof.
  f.confirmUnorm();assert(f.t.readyUnorm(40,80,70,102));
 }
 {
  UnormFixture f;f.t.resetCommand(2,true);f.t.depthWrite(2,70);
  f.t.eraseImage(70);f.t.depthImage(70);f.confirmUnorm();
  f.t.submitted(100,{2},103);assert(f.t.readyUnorm(40,80,70,103) && f.t.stats.staleDepthWrites==1);
  f.t.submitted(999,{2},103);assert(f.t.readyUnorm(40,80,70,103) && !f.t.stats.multipleQueues);
 }
 {
  UnormFixture f;f.t.resetCommand(2,true);f.t.depthWrite(2,70);f.t.depthWrite(2,71);
  f.t.submitted(999,{2},90); // Startup transfers do not select the learner queue.
  f.confirmUnorm();assert(f.t.readyUnorm(40,80,70,102) && !f.t.stats.multipleQueues);
  f.t.resetCommand(2,true);f.t.depthWrite(2,71);f.t.submitted(999,{2},103);
  assert(f.t.readyUnorm(40,80,70,103) && !f.t.stats.multipleQueues);
  f.t.beginPass(1,40,-1,80,false,70,true);assert(f.t.prepareUnorm(1,103));f.t.pipeline(1,101);
  f.t.resetCommand(2,true);f.t.depthWrite(2,70);f.t.submitted(999,{2},103);
  assert(f.t.stats.multipleQueues==1 && !f.t.readyUnorm(40,80,70,103) && !f.t.allowUnormPipeline(1,101,103));
 }
 {
  // Even without full learned proof, another queue touching a pending producer
  // matters. An unknown supported-color depth write follows the same rule.
  for(bool color:{false,true}){
   UnormFixture f;f.t.resetCommand(1,true);f.unorm();f.t.submitted(100,{1},100);
   f.t.resetCommand(2,true);
   if(color){f.t.beginPass(2,20,-1,90,false,70);f.t.draw(2);f.t.endPass(2,101);}else f.t.depthWrite(2,70);
   f.t.submitted(999,{2},101);assert(f.t.stats.multipleQueues==1);
  }
 }
 {
  for(Id queue:{100u,999u}){
   UnormFixture f;f.confirmUnorm();f.t.beginPass(1,40,-1,80,false,70,true);assert(f.t.prepareUnorm(1,103));f.t.pipeline(1,101);
   f.t.resetCommand(2,true);for(unsigned n=0;n<257;++n)f.t.depthWrite(2,n==0?70:71);
   f.t.submitted(queue,{2},103);
   assert(f.t.stats.eventDrops==1 && !f.t.readyUnorm(40,80,70,103) && !f.t.allowUnormPipeline(1,101,103));
  }
 }
}
void unormTests(){
 {
  UnormFixture f;f.t.resetCommand(1,true);f.t.beginPass(1,40,-1,80,false,70,true);
  assert(!f.t.prepareUnorm(1,100));f.t.pipeline(1,101);assert(!f.t.allowUnormPipeline(1,101,100));
  f.confirmUnorm();f.t.resetCommand(1,true);f.t.beginPass(1,40,-1,80,false,70,true);
  f.t.pipeline(1,101);assert(!f.t.allowUnormPipeline(1,101,103)); // Preparation is required.
  assert(f.t.prepareUnorm(1,103));assert(f.t.allowUnormPipeline(1,101,103));
  // Evidence changes from another submitted recording leave this scope's
  // snapshot intact. The following scope must use the newly changed evidence.
  f.t.resetCommand(2,true);f.t.beginPass(2,40,-1,80,false,70,true);f.t.pipeline(2,111);
  for(unsigned n=0;n<128;++n)f.t.draw(2);
  f.t.endPass(2,103);f.t.submitted(100,{2},103);
  assert(f.t.allowUnormPipeline(1,101,103));
  f.t.endPass(1,103);f.t.beginPass(1,40,-1,80,false,70,true);
  assert(!f.t.prepareUnorm(1,103));
 }
 {
  // Every object kind involved in authorization invalidates active snapshots,
  // including reused handles; destruction of an unrelated pipeline does not.
  for(unsigned object=0;object<7;++object){
   UnormFixture f;f.confirmUnorm();f.t.resetCommand(1,true);f.t.beginPass(1,40,-1,80,false,70,true);
   assert(f.t.prepareUnorm(1,103));f.t.pipeline(1,101);assert(f.t.allowUnormPipeline(1,101,103));
   f.t.erasePipeline(123456);assert(f.t.allowUnormPipeline(1,101,103));
   if(object==0){f.t.eraseImage(70);f.t.depthImage(70);}
   if(object==1){f.t.eraseImage(40);f.t.image(40,100,80,false,true);}
   if(object==2)f.t.erasePass(80);
   if(object==3){f.t.erasePipeline(101);f.t.pipeline(1,101);}
   if(object==4)f.t.extent(200,160);
   if(object==5)f.t.resetCommand(1,true);
   if(object==6){f.t.resetCommand(2,true);f.t.beginPass(2,10,-1,50,true);
    for(unsigned n=0;n<128;++n)f.t.draw(2);
    f.t.endPass(2,103);f.t.submitted(101,{2},103);}
   assert(!f.t.allowUnormPipeline(1,101,103));
  }
 }
 {
  UnormFixture f;f.confirmUnorm();assert(f.t.readyUnorm(40,80,70,102));
  assert(f.t.unormEvidence.size()==1 && f.t.stats.unormObserved==3 && f.t.stats.unormProducerPasses==3);
  assert(!f.t.readyUnorm(40,80,71,102) && !f.t.readyUnorm(40,81,70,102) && !f.t.readyUnorm(40,80,70,2103));
  f.t.resetCommand(1,true);f.t.beginPass(1,40,-1,80,false,70,true);assert(f.t.prepareUnorm(1,103));
  f.t.pipeline(1,999);assert(!f.t.allowUnormPipeline(1,999,103));
  f.t.pipeline(1,101);assert(f.t.allowUnormPipeline(1,101,103));f.t.draw(1);
  f.t.pipeline(1,102);assert(f.t.allowUnormPipeline(1,102,103));
  f.t.pipeline(1,103);assert(!f.t.allowUnormPipeline(1,103,103));
  // A different first drawn pipeline never gets enabled by a later bind.
  f.t.resetCommand(1,true);f.t.beginPass(1,40,-1,80,false,70,true);assert(f.t.prepareUnorm(1,103));
  f.t.pipeline(1,102);f.t.draw(1);f.t.pipeline(1,101);assert(!f.t.allowUnormPipeline(1,101,103));
 }
 {
  UnormFixture noEye,wrongDepth,wrongOrder,oneEye;
  for(unsigned i=0;i<3;++i){
   noEye.t.resetCommand(1,true);noEye.unorm();noEye.scene();noEye.t.submitted(100,{1},100+i);
   wrongDepth.t.resetCommand(1,true);wrongDepth.unorm();wrongDepth.scene(71);wrongDepth.consumer(1,i%2);wrongDepth.t.submitted(100,{1},100+i);
   wrongOrder.t.resetCommand(1,true);wrongOrder.scene();wrongOrder.unorm();wrongOrder.consumer(1,i%2);wrongOrder.t.submitted(100,{1},100+i);
   oneEye.chain(0);oneEye.t.submitted(100,{1},100+i);
  }
  assert(noEye.t.unormEvidence.empty() && wrongDepth.t.unormEvidence.empty() && wrongOrder.t.unormEvidence.empty());
  assert(!oneEye.t.readyUnorm(40,80,70,102));
  UnormFixture repeatedRead;repeatedRead.chain();repeatedRead.t.submitted(100,{1},100);
  for(unsigned i=0;i<3;++i){repeatedRead.t.resetCommand(1,true);repeatedRead.consumer(1,i%2);repeatedRead.t.submitted(100,{1},101+i);}
  assert(repeatedRead.t.unormEvidence.at(40).proof.observations==1 && !repeatedRead.t.readyUnorm(40,80,70,104));
 }
 {
  UnormFixture conflicting,unknownDepth,unknownColor,staleDescriptor;
  for(unsigned i=0;i<3;++i){
   conflicting.t.resetCommand(1,true);conflicting.unorm();conflicting.unorm(41);conflicting.scene();conflicting.consumer(1,i%2);conflicting.t.submitted(100,{1},100+i);
   unknownDepth.t.resetCommand(1,true);unknownDepth.unorm();unknownDepth.t.beginPass(1,0,-1,90,false,70);unknownDepth.t.endPass(1,100);
   unknownDepth.scene();unknownDepth.consumer(1,i%2);unknownDepth.t.submitted(100,{1},100+i);
   unknownColor.t.resetCommand(1,true);unknownColor.unorm();unknownColor.t.beginPass(1,20,-1,90,false,70);unknownColor.t.draw(1);unknownColor.t.endPass(1,100);
   unknownColor.scene();unknownColor.consumer(1,i%2);unknownColor.t.submitted(100,{1},100+i);
   staleDescriptor.chain(i%2);staleDescriptor.t.write(3,0,0,10);staleDescriptor.t.submitted(100,{1},100+i);
  }
  assert(conflicting.t.unormEvidence.empty() && unknownDepth.t.unormEvidence.empty() && unknownColor.t.unormEvidence.empty() && staleDescriptor.t.unormEvidence.empty());
  assert(staleDescriptor.t.stats.staleDescriptors==3);
  // Unknown full-size color work also revokes a prepared scope on that depth,
  // while the same work using another depth leaves its authorization intact.
  UnormFixture prepared;prepared.confirmUnorm();prepared.t.resetCommand(1,true);
  prepared.t.beginPass(1,40,-1,80,false,70,true);assert(prepared.t.prepareUnorm(1,103));prepared.t.pipeline(1,101);
  prepared.t.resetCommand(2,true);prepared.t.beginPass(2,20,-1,90,false,71);prepared.t.draw(2);prepared.t.endPass(2,103);prepared.t.submitted(100,{2},103);
  assert(prepared.t.allowUnormPipeline(1,101,103));
  prepared.t.resetCommand(2,true);prepared.t.beginPass(2,20,-1,90,false,70);prepared.t.draw(2);prepared.t.endPass(2,103);prepared.t.submitted(100,{2},103);
  assert(!prepared.t.allowUnormPipeline(1,101,103) && !prepared.t.readyUnorm(40,80,70,103));
 }
 {
  UnormFixture depthReused,pipelineReused,colorReused;
  depthReused.chain();depthReused.t.eraseImage(70);depthReused.t.depthImage(70);depthReused.t.submitted(100,{1},100);
  pipelineReused.chain();pipelineReused.t.erasePipeline(101);pipelineReused.t.submitted(100,{1},100);
  colorReused.chain();colorReused.t.eraseImage(40);colorReused.t.image(40,100,80,false,true);colorReused.t.submitted(100,{1},100);
  assert(depthReused.t.unormEvidence.empty() && pipelineReused.t.unormEvidence.empty() && colorReused.t.unormEvidence.empty());
  depthReused.confirmUnorm();assert(depthReused.t.readyUnorm(40,80,70,102));depthReused.t.eraseImage(70);assert(!depthReused.t.readyUnorm(40,80,70,102));
  pipelineReused.confirmUnorm();assert(pipelineReused.t.readyUnorm(40,80,70,102));pipelineReused.t.erasePipeline(102);assert(!pipelineReused.t.readyUnorm(40,80,70,102));
 }
 {
  // Nearby primary command buffers on the same queue can form the chain;
  // unrelated long submission histories cannot.
  UnormFixture nearby,distant;
  for(unsigned i=0;i<3;++i){
   nearby.t.resetCommand(1,true);nearby.unorm();nearby.t.submitted(100,{1},100+i);
   nearby.t.resetCommand(1,true);nearby.scene();nearby.consumer(1,i%2);nearby.t.submitted(100,{1},100+i);
  }
  assert(nearby.t.readyUnorm(40,80,70,102));
  distant.t.resetCommand(1,true);distant.unorm();distant.t.submitted(100,{1},100);
  for(unsigned batch=0;batch<3;++batch){distant.t.resetCommand(1,true);
   for(unsigned n=0;n<100;++n){distant.t.beginPass(1,21,-1,90);distant.t.endPass(1,100);}
   distant.t.submitted(100,{1},100);
  }
  distant.t.resetCommand(1,true);distant.scene();distant.consumer(1);distant.t.submitted(100,{1},100);
  assert(distant.t.unormEvidence.empty());
 }
 {
  UnormFixture overflow;overflow.t.resetCommand(1,true);overflow.t.beginPass(1,40,-1,80,false,70,true);
  for(Id pipe=1;pipe<=257;++pipe){overflow.t.pipeline(1,pipe);overflow.t.draw(1);}
  overflow.t.endPass(1,100);overflow.scene();overflow.consumer(1);overflow.t.submitted(100,{1},100);
  assert(overflow.t.unormEvidence.empty() && overflow.t.stats.pipelineDrops);
  // Explicitly tracked bindings can survive a render-pass boundary. A bind
  // outside an eligible producer makes the inherited pipeline unknown.
  UnormFixture inherited;inherited.t.resetCommand(1,true);inherited.unorm();
  inherited.t.beginPass(1,40,-1,80,false,70,true);for(unsigned n=0;n<128;++n)inherited.t.draw(1);inherited.t.endPass(1,100);
  assert(inherited.t.stats.unormProducerPasses==2);
  inherited.t.beginPass(1,10,-1,50,true,70);inherited.t.pipeline(1,101);inherited.t.endPass(1,100);
  inherited.t.beginPass(1,40,-1,80,false,70,true);for(unsigned n=0;n<128;++n)inherited.t.draw(1);inherited.t.endPass(1,100);
  assert(inherited.t.stats.unormProducerPasses==2 && inherited.t.stats.pipelineDrops);
 }
 {
  UnormFixture changed;changed.confirmUnorm();changed.t.resetCommand(1,true);changed.unorm(40,70,111);changed.scene();changed.consumer(1);changed.t.submitted(100,{1},103);
  assert(!changed.t.readyUnorm(40,80,70,103) && changed.t.unormEvidence.at(40).proof.observations==1);
  changed.confirmUnorm();changed.t.submitted(101,{1},104);assert(!changed.t.readyUnorm(40,80,70,104));
 }
}
int main(){
 depthWriteTests();
 unormTests();
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
  // Producer material bindings do not define a scene-to-eye connection. The
  // same command buffer can later sample that scene in a final-eye pass,
  // where descriptor generation and revision checks must still apply.
  Fixture f;f.t.allocateSet(6,4);f.t.write(6,0,0,11);
  const Id materials[]={3,6};
  const auto record=[&](int eye){
   f.t.resetCommand(1,true);f.t.beginPass(1,10,-1,50,true);
   for(unsigned draw=0;draw<128;++draw){
    f.t.bind(1,5+draw%2,0,2,materials);f.t.draw(1);
   }
   f.t.endPass(1,100);f.consumer(1,eye);
  };
  for(unsigned i=0;i<3;++i){record(i%2);f.t.submitted(100,{1},100+i);}
  assert(f.t.ready(10,50,102) && f.t.stats.producerPasses==3);
  assert(f.t.stats.descriptorScans==3 && !f.t.stats.ambiguous);
  record(1);f.t.write(3,0,0,11);f.t.submitted(100,{1},103);
  assert(f.t.stats.staleDescriptors==1 && f.t.evidence.at(10).tick==102);

  // Reusing an eye-bound set handle before its draw also remains invalid.
  f.t.write(3,0,0,10);f.t.resetCommand(1,true);f.producer();
  f.t.beginPass(1,20,0,60);f.t.bind(1,5,0,1,&f.set);
  f.t.eraseSet(3);f.t.allocateSet(3,4);f.t.write(3,0,0,10);
  f.t.draw(1);f.t.endPass(1,104);f.t.submitted(100,{1},104);
  assert(f.t.stats.setMisses==1 && f.t.stats.ambiguous==1);
  assert(f.t.evidence.at(10).tick==102);
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
