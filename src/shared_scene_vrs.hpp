#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

// Experimental shared scene-color recognition. The adapter supplies only
// supported SFLOAT color targets and directly classified final-eye consumers.
// Bound descriptors are candidate reads, not shader reflection or proof of UV
// correspondence. Events establish host submission order, not GPU dependencies.
// Call under the layer mutex; this module never calls Vulkan or waits on a GPU.
namespace beamng_shared {
using Id = uint64_t;
struct Image { uint32_t width=0,height=0; bool sceneCandidate=false; uint64_t generation=0; };
struct Evidence {
 Id pass=0; uint64_t generation=0,submission=0,tick=0;
 unsigned observations=0,eyes=0; std::array<uint64_t,2> eyeTicks{};
};
struct Set { Id pool=0; uint64_t generation=0,revision=0; bool unknown=false; std::map<uint64_t,Id> images; };
struct BoundSet { Id id=0; uint64_t generation=0,revision=0; };
struct Event {
 enum Kind { Write, Read, Barrier } kind=Barrier;
 Id image=0,pass=0; uint64_t generation=0;
 bool scenePass=false,heavy=false; int eye=-1;
 std::vector<BoundSet> readSets;
};
struct Command {
 Id pool=0,layout=0,output=0,input=0,pass=0;
 bool primary=false,recording=false,ambiguous=false,scenePass=false,overflow=false,relevant=false;
 int eye=-1;
 uint64_t draws=0,checkedRevision=0;
 std::array<BoundSet,8> sets{};
 std::vector<BoundSet> readSets;
 std::vector<Event> events;
};
struct Stats {
 uint64_t selected=0,binds=0,submissions=0,observed=0,producerPasses=0,
  lowDrawPasses=0,retainedSmallWrites=0,otherWrites=0,missingInputs=0,
  ambiguous=0,noProducer=0,unsupported=0,barriers=0,descriptorScans=0,
  scanSkips=0,setMisses=0,setDrops=0,slotDrops=0,imageDrops=0,
  commandDrops=0,eventDrops=0,staleEvents=0,staleDescriptors=0,multipleQueues=0;
};
class Tracker {
 static constexpr size_t MaxImages=8192,MaxCommands=8192,MaxSets=131072,MaxSlots=128,MaxEvents=256,MaxReadSets=64;
 static constexpr uint64_t MinDraws=128,ExpiryMs=2000;
 uint64_t generation_=0,descriptorRevision_=1,submission_=0;
 uint32_t width_=0,height_=0;
 Id queue_=0; bool multipleQueues_=false;
 struct Writer { Id pass=0; uint64_t generation=0,tick=0; };
 std::unordered_map<Id,Writer> writers_;
 void append(Command& c,Event event){
  if(event.heavy || event.kind==Event::Read)c.relevant=true;
  if(c.overflow)return;
  if(c.events.size()>=MaxEvents){c.events.clear();c.overflow=true;++stats.eventDrops;return;}
  c.events.push_back(std::move(event));
 }
 void clearHistory(){writers_.clear();evidence.clear();}
 void clearPass(Command& c){
  c.layout=c.output=c.input=c.pass=0;c.eye=-1;c.draws=0;c.checkedRevision=0;
  c.ambiguous=c.scenePass=false;c.sets={};c.readSets.clear();
 }
 bool recent(uint64_t tick,uint64_t now)const{return now>=tick && now-tick<=ExpiryMs;}
public:
 std::unordered_map<Id,Image> images;
 std::unordered_map<Id,Set> sets;
 std::unordered_map<Id,Command> commands;
 std::unordered_map<Id,Evidence> evidence;
 Stats stats;
 bool eligible(Id id)const{
  const auto i=images.find(id);
  return i!=images.end() && width_ && i->second.width==width_ && i->second.height==height_;
 }
 bool candidate(Id id)const{const auto i=images.find(id);return eligible(id) && i->second.sceneCandidate;}
 void extent(uint32_t w,uint32_t h){
  if(w==width_ && h==height_)return;
  width_=w;height_=h;++descriptorRevision_;clearHistory();
  // Events recorded against an old resolution cannot authorize a new one.
  for(auto& [id,c]:commands){c.events.clear();c.overflow=c.relevant=false;clearPass(c);}
 }
 void image(Id id,uint32_t w,uint32_t h,bool sceneCandidate){
  // Fresh handles cannot already occur in descriptors: write() only retains
  // known images. Avoid scanning every allocated material set on allocation.
  if(images.count(id))eraseImage(id);
  if(images.size()>=MaxImages){++stats.imageDrops;return;}
  images[id]={w,h,sceneCandidate,++generation_};
 }
 void eraseImage(Id id){
  if(!images.erase(id))return;
  ++descriptorRevision_;evidence.erase(id);writers_.erase(id);
  // Remove old descriptor references before the driver can reuse this handle.
  for(auto& [key,s]:sets)for(auto i=s.images.begin();i!=s.images.end();)
   if(i->second==id){i=s.images.erase(i);++s.revision;}else ++i;
  for(auto& [key,c]:commands)if(c.output==id || c.input==id){c.ambiguous=true;c.output=0;}
 }
 void erasePass(Id pass){
  for(auto i=evidence.begin();i!=evidence.end();)if(i->second.pass==pass)i=evidence.erase(i);else ++i;
  for(auto i=writers_.begin();i!=writers_.end();)if(i->second.pass==pass)i=writers_.erase(i);else ++i;
  // Replace its old events with a barrier so reused pass handles never restore
  // evidence. Unrelated events remain bounded and conservatively ordered.
  for(auto& [key,c]:commands){
   for(auto& e:c.events)if(e.pass==pass)e={};
   if(c.pass==pass){c.ambiguous=true;c.output=0;}
  }
 }
 void allocateSet(Id id,Id pool){
  ++descriptorRevision_;sets.erase(id);
  if(sets.size()>=MaxSets){++stats.setDrops;return;}
  sets[id]={pool,++generation_,0,false,{}};
 }
 void eraseSet(Id id){++descriptorRevision_;sets.erase(id);}
 void resetDescriptors(Id pool){
  ++descriptorRevision_;
  for(auto i=sets.begin();i!=sets.end();)if(i->second.pool==pool)i=sets.erase(i);else ++i;
 }
 void unknown(Id id){
  ++descriptorRevision_;const auto s=sets.find(id);
  if(s!=sets.end()){++s->second.revision;s->second.unknown=true;s->second.images.clear();}
  ++stats.unsupported;
 }
 void write(Id id,uint32_t binding,uint32_t element,Id image){
  auto s=sets.find(id);if(s==sets.end())return;
  ++s->second.revision;
  if(s->second.unknown)return;
  ++descriptorRevision_;const auto slot=(uint64_t(binding)<<32)|element;
  const auto i=images.find(image);
  // Ordinary textures consume no candidate slots. Keep other-resolution scene
  // images until the eye extent is discovered, and filter size when sampling.
  if(!image || i==images.end() || !i->second.sceneCandidate){s->second.images.erase(slot);return;}
  if(s->second.images.size()>=MaxSlots && !s->second.images.count(slot)){
   ++stats.slotDrops;unknown(id);return;
  }
  s->second.images[slot]=image;
 }
 void copy(Id source,uint32_t sb,uint32_t se,Id dest,uint32_t db,uint32_t de){
  const auto s=sets.find(source);
  if(s==sets.end() || s->second.unknown){unknown(dest);return;}
  const auto slot=s->second.images.find((uint64_t(sb)<<32)|se);
  write(dest,db,de,slot==s->second.images.end()?0:slot->second);
 }
 void allocateCommand(Id id,Id pool,bool primary){
  commands.erase(id);
  if(commands.size()>=MaxCommands){++stats.commandDrops;return;}
  auto& c=commands[id];c.pool=pool;c.primary=primary;
 }
 void resetCommand(Id id,bool begin=false){
  const auto i=commands.find(id);if(i==commands.end())return;
  auto& c=i->second;clearPass(c);c.events.clear();c.overflow=c.relevant=false;c.recording=begin && c.primary;
 }
 void resetCommands(Id pool,bool destroy=false){
  for(auto i=commands.begin();i!=commands.end();){
   if(i->second.pool!=pool){++i;continue;}
   const auto id=i->first;++i;if(destroy)commands.erase(id);else resetCommand(id);
  }
 }
 void invalidate(Id cmd){
  const auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  append(i->second,{});clearPass(i->second);++stats.unsupported;
 }
 void beginPass(Id cmd,Id output,int eye,Id pass,bool scenePass=false){
  const auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  auto& c=i->second;clearPass(c);c.output=eligible(output)?output:0;c.eye=eye;c.pass=pass;
  c.scenePass=scenePass && candidate(output) && pass;
 }
 void bind(Id cmd,Id layout,unsigned first,unsigned count,const Id* bound){
  const auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording || !i->second.output)return;
  auto& c=i->second;c.checkedRevision=0;
  if(first>8 || count>8-first || (count && !bound)){c.ambiguous=true;++stats.unsupported;return;}
  if(c.layout!=layout)c.sets={};
  c.layout=layout;
  for(unsigned j=0;j<count;++j){const auto s=sets.find(bound[j]);
   c.sets[first+j]={bound[j],s==sets.end()?0:s->second.generation};}
 }
 void draw(Id cmd){
  const auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  auto& c=i->second;if(!c.output)return;++c.draws;
  if(c.eye<0 || c.eye>1)return;
  if(c.ambiguous || c.checkedRevision==descriptorRevision_){++stats.scanSkips;return;}
  c.checkedRevision=descriptorRevision_;++stats.descriptorScans;
  for(const auto bound:c.sets){
   if(!bound.id)continue;
   const auto s=sets.find(bound.id);
   if(s==sets.end() || s->second.generation!=bound.generation || s->second.unknown){
    c.ambiguous=true;++stats.setMisses;continue;
   }
   // Capture all bound sets, including empty material sets: an update-after-
   // bind write could add or replace a candidate before queue submission.
   bool captured=false;
   for(const auto& watched:c.readSets)if(watched.id==bound.id){
    captured=true;
    if(watched.generation!=s->second.generation || watched.revision!=s->second.revision)c.ambiguous=true;
    break;
   }
   if(!captured){
    if(c.readSets.size()>=MaxReadSets){c.ambiguous=true;++stats.unsupported;}
    else c.readSets.push_back({bound.id,s->second.generation,s->second.revision});
   }
   for(const auto [slot,image]:s->second.images){
    if(image==c.output || !candidate(image))continue;
    if(c.input && c.input!=image)c.ambiguous=true;
    c.input=image;
   }
  }
 }
 void endPass(Id cmd,uint64_t /*now*/){
  const auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  auto& c=i->second;
  if(c.output){
   if(c.eye>=0 && c.eye<2 && c.draws){
    if(c.ambiguous)++stats.ambiguous;
    else if(!c.input)++stats.missingInputs;
    else append(c,{Event::Read,c.input,0,images.at(c.input).generation,false,false,c.eye,c.readSets});
   }
   const bool heavy=c.scenePass && c.draws>=MinDraws;
   if(heavy)++stats.producerPasses;else if(c.scenePass)++stats.lowDrawPasses;
   // Even a clear-only pass may overwrite the image, so zero draws is a write.
   append(c,{Event::Write,c.output,c.pass,images.at(c.output).generation,c.scenePass,heavy,-1,{}});
  }
  clearPass(c);
 }
 void submitted(Id queue,const std::vector<Id>& submittedCommands,uint64_t now){
  if(!queue || submittedCommands.empty())return;
  bool relevant=false;
  for(const auto cmd:submittedCommands){const auto c=commands.find(cmd);
   if(c!=commands.end() && c->second.relevant){relevant=true;break;}}
  // Startup upload/transfer queues with no candidate scene or eye observations
  // must not lock discovery to the wrong queue.
  if(!relevant && queue!=queue_)return;
  if(!queue_)queue_=queue;
  if(queue!=queue_){if(!multipleQueues_)++stats.multipleQueues;multipleQueues_=true;clearHistory();}
  if(multipleQueues_)return;
  ++submission_;++stats.submissions;
  for(const auto cmd:submittedCommands){
   const auto c=commands.find(cmd);
   if(c==commands.end() || !c->second.primary || !c->second.recording || c->second.overflow){
    writers_.clear();++stats.barriers;continue;
   }
   for(const auto& e:c->second.events){
    // Unknown work breaks the pending producer-to-consumer association. Prior
    // independently confirmed participation remains until its normal expiry;
    // ready() also needs a newly established, recent heavy producer. Otherwise
    // one unrelated unsupported command per frame would erase all training.
    if(e.kind==Event::Barrier){writers_.clear();++stats.barriers;continue;}
    const auto image=images.find(e.image);
    if(image==images.end() || image->second.generation!=e.generation || !eligible(e.image)){
     writers_.clear();++stats.staleEvents;continue;
    }
    if(e.kind==Event::Write){
     if(e.heavy){
      const auto old=writers_.find(e.image);
      if(old!=writers_.end() && (old->second.pass!=e.pass || old->second.generation!=e.generation))evidence.erase(e.image);
      writers_[e.image]={e.pass,e.generation,now};continue;
     }
     const auto old=writers_.find(e.image);
     // This is candidate participation, not a last-writer graph: the traced
     // image also receives lightweight writes/copies before the eye samples it.
     // They retain a recent heavy association without refreshing its age.
     // A different qualifying heavy producer above resets the association.
     if(old!=writers_.end() && old->second.generation==e.generation && recent(old->second.tick,now)){
      ++stats.retainedSmallWrites;continue;
     }
     writers_.erase(e.image);evidence.erase(e.image);++stats.otherWrites;continue;
    }
    bool descriptorsCurrent=true;
    for(const auto& watched:e.readSets){
     const auto set=sets.find(watched.id);
     if(set==sets.end() || set->second.unknown || set->second.generation!=watched.generation ||
        set->second.revision!=watched.revision){descriptorsCurrent=false;break;}
    }
    // Reject only this observation. Other sets' edits do not invalidate it,
    // and an old read never refreshes independently confirmed good evidence.
    if(!descriptorsCurrent){++stats.staleDescriptors;continue;}
    const auto producer=writers_.find(e.image);
    if(producer==writers_.end() || producer->second.generation!=e.generation || !recent(producer->second.tick,now)){
     ++stats.noProducer;continue;
    }
    auto& proof=evidence[e.image];
    if(proof.pass!=producer->second.pass || proof.generation!=e.generation || !recent(proof.tick,now))
     proof={producer->second.pass,e.generation,0,now,0,0,{}};
    if(proof.submission!=submission_){if(proof.observations<3)++proof.observations;proof.submission=submission_;}
    proof.eyes|=1u<<e.eye;proof.eyeTicks[e.eye]=now;proof.tick=now;++stats.observed;
   }
  }
 }
 bool ready(Id image,Id pass,uint64_t now)const{
  const auto e=evidence.find(image);const auto i=images.find(image);
  const auto producer=writers_.find(image);
  return !multipleQueues_ && candidate(image) && pass && e!=evidence.end() &&
   e->second.pass==pass && e->second.generation==i->second.generation &&
   e->second.observations>=3 && e->second.eyes==3 && recent(e->second.tick,now) &&
   recent(e->second.eyeTicks[0],now) && recent(e->second.eyeTicks[1],now) &&
   producer!=writers_.end() && producer->second.pass==pass && recent(producer->second.tick,now);
 }
};
} // namespace beamng_shared
