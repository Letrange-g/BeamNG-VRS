#include "early_vrs.hpp"
#include <cassert>
#include <iostream>
using namespace beamng_early;
int main(){
 Tracker t;t.extent(100,80);t.image(10,100,80);t.image(11,100,80);t.image(20,100,80);t.image(21,100,80);t.image(30,50,40);
 t.allocateCommand(1,2,true);t.allocateSet(3,4);t.write(3,0,0,10);
 const Id set=3;
 auto producer=[&](Id image){t.beginPass(1,image,-1);t.draw(1);t.endPass(1,100);};
 auto consume=[&](int eye){t.beginPass(1,eye?21:20,eye);t.bind(1,5,0,1,&set);t.draw(1);t.endPass(1,100);};
 auto record=[&]{t.resetCommand(1,true);producer(10);consume(0);};
 record();assert(t.eye(10,100)==-1);record();assert(t.eye(10,100)==-1);record();assert(t.eye(10,100)==0);
 assert(t.eye(10,100,1)==0 && t.eye(10,100,2)==-1);
 t.erasePass(1);assert(t.eye(10,100)==-1);
 for(int i=0;i<3;++i)record();
 assert(t.eye(10,2101)==-1 && t.eye(10,99)==-1);assert(t.eye(20,100)==-1);
 // Several observations in a single recording are not several confirmations.
 t.evidence.clear();t.resetCommand(1,true);producer(10);consume(0);consume(0);consume(0);assert(t.eye(10,100)==-1);
 // Same-frame producer requirement excludes history/unwritten sampled images.
 t.evidence.clear();t.resetCommand(1,true);consume(0);assert(t.evidence.empty());
 for(int i=0;i<3;++i)record();
 consume(1);assert(t.eye(10,100)==-1); // Shared between eyes: never guess.
 t.evidence.clear();t.write(3,1,0,11);
 for(int i=0;i<3;++i){t.resetCommand(1,true);producer(10);producer(11);consume(0);}
 assert(t.evidence.empty() && t.stats.ambiguous>0);
 t.write(3,1,0,0);
 // Copies retain image identity; unknown writes/templates invalidate the set.
 t.allocateSet(6,4);t.copy(3,0,0,6,2,0);assert(t.sets.at(6).images.at(uint64_t(2)<<32)==10);
 t.unknown(3);record();assert(t.evidence.empty());t.allocateSet(3,4);t.write(3,0,0,10);
 for(int i=0;i<3;++i)record();
 assert(t.eye(10,100)==0);
 t.eraseImage(10);t.image(10,100,80);assert(t.eye(10,100)==-1);record();assert(t.evidence.empty());
 t.write(3,0,0,10);for(int i=0;i<3;++i)record();
 t.extent(200,160);assert(t.evidence.empty() && !t.eligible(10));
 t.extent(100,80);
 // Secondary work, unknown render paths and reset commands break the chain.
 t.resetCommand(1,true);producer(10);t.invalidate(1);consume(0);assert(t.evidence.empty());
 t.resetCommand(1,true);producer(10);t.resetCommands(2);consume(0);assert(t.evidence.empty());
 t.allocateCommand(1,2,false);record();assert(t.evidence.empty());t.allocateCommand(1,2,true);
 // Unrelated sizes, self-reads, and descriptors from a destroyed/reused pool.
 t.write(3,0,0,30);for(int i=0;i<3;++i){t.resetCommand(1,true);producer(30);consume(0);}assert(t.evidence.empty());
 t.write(3,0,0,20);for(int i=0;i<3;++i){t.resetCommand(1,true);producer(20);consume(0);}assert(t.evidence.empty());
 t.resetDescriptors(4);assert(t.sets.empty());
 t.resetCommands(2,true);assert(t.commands.empty());
 // Repeated draws skip descriptor scans, but edits without a rebind must
 // still invalidate that cache and detect a second possible producer.
 t.allocateCommand(1,2,true);t.allocateSet(3,4);t.write(3,0,0,10);
 t.evidence.clear();t.resetCommand(1,true);producer(10);producer(11);
 t.beginPass(1,20,0);t.bind(1,5,0,1,&set);t.draw(1);
 const auto scans=t.stats.descriptorScans;
 for(int i=0;i<100;++i)t.draw(1);
 assert(t.stats.descriptorScans==scans && t.stats.scanSkips>=100);
 t.write(3,0,0,11);t.draw(1);t.endPass(1,100);
 assert(t.evidence.empty() && t.stats.descriptorScans==scans+1);
 // Unknown/template writes after an already-scanned draw also fail closed.
 t.allocateSet(3,4);t.write(3,0,0,10);t.resetCommand(1,true);producer(10);
 t.beginPass(1,20,0);t.bind(1,5,0,1,&set);t.draw(1);t.unknown(3);t.draw(1);t.endPass(1,100);
 assert(t.evidence.empty());
 // Host memory is bounded on malformed/unusually large workloads.
 t.allocateSet(3,4);for(unsigned i=0;i<129;++i)t.write(3,i,0,10);assert(t.sets.at(3).unknown);
 std::cout<<"Early VRS producer inference, ambiguity, scope and lifetime passed\n";
}
