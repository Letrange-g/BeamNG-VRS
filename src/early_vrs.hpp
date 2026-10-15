#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>

// Experimental, one-hop producer discovery. IDs are Vulkan handles supplied by
// the adapter, but this module has no driver dependencies. Call under layer mu.
// A bound texture is only evidence of a possible read, not shader reflection.
// Never treat this as a general render graph or proof of matching UVs.
namespace beamng_early {
using Id=uint64_t;
struct Image { uint32_t width=0,height=0; };
struct Evidence { unsigned eyes=0,observations=0; uint64_t recording=0,tick=0; Id pass=0; };
struct Set { Id pool=0; bool unknown=false; std::map<uint64_t,Id> images; };
struct Command {
 Id pool=0,layout=0,output=0,input=0,pass=0;
 bool primary=false,recording=false,ambiguous=false,drew=false;
 int eye=-1;
 uint64_t sequence=0;
 std::array<Id,8> sets{};
 std::unordered_map<Id,Id> producers;
};
struct Stats { uint64_t draws=0,observed=0,ambiguous=0,noInput=0,unsupported=0,selected=0,binds=0; };
class Tracker {
 uint64_t sequence_=0;
 uint32_t width_=0,height_=0;
public:
 std::unordered_map<Id,Image> images;
 std::unordered_map<Id,Set> sets;
 std::unordered_map<Id,Command> commands;
 std::unordered_map<Id,Evidence> evidence;
 Stats stats;
 void extent(uint32_t w,uint32_t h){
  if(w==width_ && h==height_)return;
  width_=w;height_=h;evidence.clear();
  for(auto& [id,c]:commands){c.producers.clear();c.output=c.input=0;c.sets={};}
 }
 bool eligible(Id image)const{
  auto i=images.find(image);
  return i!=images.end() && width_ && i->second.width==width_ && i->second.height==height_;
 }
 void image(Id id,uint32_t w,uint32_t h){
  eraseImage(id);if(images.size()<8192)images[id]={w,h};
 }
 void eraseImage(Id id){
  images.erase(id);evidence.erase(id);
  for(auto& [key,c]:commands){c.producers.erase(id);if(c.output==id || c.input==id)c.ambiguous=true;}
  // Descriptor references to destroyed views/images are unusable until updated.
  for(auto& [key,s]:sets)for(auto& [binding,image]:s.images)if(image==id)image=0;
 }
 void erasePass(Id pass){
  for(auto i=evidence.begin();i!=evidence.end();)if(i->second.pass==pass)i=evidence.erase(i);else ++i;
  for(auto& [id,c]:commands){
   for(auto i=c.producers.begin();i!=c.producers.end();)if(i->second==pass){
    if(c.input==i->first)c.ambiguous=true;
    i=c.producers.erase(i);
   }else ++i;
   if(c.pass==pass){c.output=0;c.ambiguous=true;}
  }
 }
 void allocateSet(Id id,Id pool){sets.erase(id);if(sets.size()<65536)sets[id].pool=pool;}
 void eraseSet(Id id){
  sets.erase(id);for(auto& [key,c]:commands)for(auto& set:c.sets)if(set==id)set=0;
 }
 void resetDescriptors(Id pool){
  for(auto i=sets.begin();i!=sets.end();)if(i->second.pool==pool){auto id=i->first;++i;eraseSet(id);}else ++i;
 }
 void unknown(Id id){auto i=sets.find(id);if(i!=sets.end()){i->second.unknown=true;i->second.images.clear();}++stats.unsupported;}
 void write(Id id,uint32_t binding,uint32_t element,Id image){
  auto s=sets.find(id);if(s==sets.end() || s->second.unknown)return;
  const uint64_t slot=(uint64_t(binding)<<32)|element;
  if(s->second.images.size()>=128 && !s->second.images.count(slot)){unknown(id);return;}
  s->second.images[slot]=image;
 }
 void copy(Id source,uint32_t sb,uint32_t se,Id dest,uint32_t db,uint32_t de){
  auto s=sets.find(source);
  if(s==sets.end() || s->second.unknown){unknown(dest);return;}
  auto v=s->second.images.find((uint64_t(sb)<<32)|se);
  write(dest,db,de,v==s->second.images.end()?0:v->second);
 }
 void allocateCommand(Id id,Id pool,bool primary){
  commands.erase(id);if(commands.size()<8192){auto& c=commands[id];c.pool=pool;c.primary=primary;}
 }
 void resetCommand(Id id,bool begin=false){
  auto c=commands.find(id);if(c==commands.end())return;
  const auto pool=c->second.pool;const auto primary=c->second.primary;
  c->second={};c->second.pool=pool;c->second.primary=primary;
  c->second.recording=begin && primary;c->second.sequence=++sequence_;
 }
 void resetCommands(Id pool,bool destroy=false){
  for(auto i=commands.begin();i!=commands.end();){
   if(i->second.pool!=pool){++i;continue;}auto id=i->first;++i;
   if(destroy)commands.erase(id);else resetCommand(id);
  }
 }
 void invalidate(Id cmd){
  auto c=commands.find(cmd);if(c==commands.end())return;
  c->second.producers.clear();c->second.output=0;c->second.ambiguous=true;c->second.sets={};
  ++stats.unsupported;
 }
 void beginPass(Id cmd,Id output,int eye,Id pass=1){
  auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  auto& c=i->second;c.output=eligible(output)?output:0;c.eye=eye;c.input=0;c.ambiguous=false;c.drew=false;c.pass=pass;
  // Rebinding after begin is required. This intentionally misses prebound sets.
  c.sets={};c.layout=0;
  if(!c.output)c.producers.clear();else c.producers.erase(output);
 }
 void bind(Id cmd,Id layout,unsigned first,unsigned count,const Id* bound){
  auto c=commands.find(cmd);if(c==commands.end() || !c->second.recording)return;
  if(first>8 || count>8-first){invalidate(cmd);return;}
  if(c->second.layout!=layout)c->second.sets={};
  c->second.layout=layout;
  for(unsigned j=0;j<count;++j)c->second.sets[first+j]=bound[j];
 }
 void draw(Id cmd){
  auto i=commands.find(cmd);if(i==commands.end() || !i->second.recording)return;
  auto& c=i->second;if(!c.output)return;c.drew=true;
  if(c.eye<0 || c.eye>1)return; // Only directly classified eyes seed discovery.
  ++stats.draws;
  for(Id id:c.sets){
   if(!id)continue;
   auto s=sets.find(id);
   if(s==sets.end() || s->second.unknown){c.ambiguous=true;continue;}
   for(auto [slot,image]:s->second.images){
    if(!image || image==c.output || !eligible(image) || !c.producers.count(image))continue;
    if(c.input && c.input!=image)c.ambiguous=true;
    c.input=image;
   }
  }
 }
 void endPass(Id cmd,uint64_t now){
  auto i=commands.find(cmd);if(i==commands.end())return;auto& c=i->second;
  if(c.output && c.drew){
   if(c.eye>=0 && c.eye<2){
    if(c.ambiguous){++stats.ambiguous;evidence.clear();}
    else if(c.input){
     auto& e=evidence[c.input];e.eyes|=1u<<c.eye;
     const auto pass=c.producers.at(c.input);
     if(e.pass!=pass){e.pass=pass;e.observations=0;e.recording=0;}
     if(e.recording!=c.sequence){++e.observations;e.recording=c.sequence;}
     e.tick=now;++stats.observed;
    }else ++stats.noInput;
   }
   if(c.producers.size()<256)c.producers[c.output]=c.pass;else invalidate(cmd);
  }
  c.output=c.input=0;c.eye=-1;c.sets={};
 }
 int eye(Id image,uint64_t now,Id pass=0)const{
  auto e=evidence.find(image);
  if(!eligible(image) || e==evidence.end() || (pass && e->second.pass!=pass) || e->second.observations<3 ||
     now<e->second.tick || now-e->second.tick>2000)return -1;
  return e->second.eyes==1?0:e->second.eyes==2?1:-1;
 }
};
} // namespace beamng_early
