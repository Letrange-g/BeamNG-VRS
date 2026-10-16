#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <map>
#include <list>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// Recording-time resource observations, not a render graph or a VRS classifier.
// Owned by Profiler and accessed ONLY under its mutex. No driver calls, waits,
// shader changes, eye inference, or logging on the draw/update paths.
namespace beamng_trace {
template<class T> uint64_t handle(T value) { return uint64_t(uintptr_t(value)); }
using Key = std::pair<VkDevice, uint64_t>;
struct Image {
 uint64_t id=0;
 uint32_t width=0,height=0,format=0,usage=0,samples=0,layers=0,mips=0,eyeMask=0;
 bool described=false;
};
using ImagePtr=std::shared_ptr<Image>;
struct View {
 uint64_t id=0;
 ImagePtr image;
 VkImageSubresourceRange range{};
 uint32_t format=0,type=0;
 bool partial=false;
};
using ViewPtr=std::shared_ptr<View>;
struct Uses {
 std::vector<ViewPtr> reads,storage;
 bool partial=false;
 static void add(std::vector<ViewPtr>& list,const ViewPtr& view,bool& partial) {
  if(!view || !view->image){partial=true;return;}
  if(view->partial || !view->image->described)partial=true;
  if(std::any_of(list.begin(),list.end(),[&](const auto& v){return v->id==view->id;}))return;
  if(list.size()>=32){partial=true;return;}
  list.push_back(view);
 }
 void merge(const Uses& b) {
  partial|=b.partial;
  for(const auto& v:b.reads)add(reads,v,partial);
  for(const auto& v:b.storage)add(storage,v,partial);
 }
};
struct Targets {ViewPtr color,depth;bool partial=false;};
struct BoundSets {
 VkPipelineLayout layout{};
 std::array<VkDescriptorSet,8> sets{};
 uint64_t revision=0;
 bool partial=false;
 void bind(VkPipelineLayout l,uint32_t first,uint32_t count,const VkDescriptorSet* values) {
  ++revision;
  // Different layout handles may be compatible, but we do not guess. Missing
  // retained sets are explicitly partial rather than attributed to stale binds.
  if(layout && layout!=l){sets={};partial=true;}
  layout=l;
  if(first>sets.size() || count>sets.size()-first || (count && !values)){
   sets={};partial=true;return;
  }
  for(uint32_t n=0;n<count;++n)sets[first+n]=values[n];
 }
 void unknown(){sets={};partial=true;++revision;}
};
class Resources {
 struct Descriptor {ViewPtr view;VkDescriptorType type{};};
 struct Set {
  uint64_t pool=0;bool partial=false;
  std::map<std::pair<uint32_t,uint32_t>,Descriptor> slots;
  std::list<Key>::iterator recent;
 };
 struct Pass {uint32_t color=VK_ATTACHMENT_UNUSED,depth=VK_ATTACHMENT_UNUSED;bool partial=false;};
 struct Framebuffer {std::vector<ViewPtr> views;bool partial=false;};
 struct Transfer {ImagePtr src,dst;std::string op;uint64_t count=0;uint32_t regions=0;};
 uint64_t next_=1,revision_=1,dropped_=0;
 struct Counters {
  uint64_t imageDrops=0,viewDrops=0,passDrops=0,framebufferDrops=0,slotDrops=0,transferDrops=0;
  uint64_t setEvictions=0,setRecoveries=0,setMisses=0,unknownUpdates=0,missingViews=0;
 } counters_;
 size_t setCapacity_;
 std::list<Key> recentSets_;
 std::map<Key,ImagePtr> images_;
 std::map<Key,ViewPtr> views_;
 std::map<Key,Set> sets_;
 std::map<Key,Pass> passes_;
 std::map<Key,Framebuffer> framebuffers_;
 std::map<std::tuple<uint64_t,uint64_t,std::string,uint32_t>,Transfer> transfers_;
 static bool imageType(VkDescriptorType t) {
  return t==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
   t==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
 }
 static bool candidate(const ViewPtr& v) {
  // Exclude ordinary sampled material textures. Keep render targets, depth,
  // storage and unknown imports, irrespective of format or resolution.
  return !v || !v->image || !v->image->described ||
   (v->image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_STORAGE_BIT));
 }
 ImagePtr image(VkDevice d,VkImage i) {
  if(!i)return {};
  Key k{d,handle(i)};auto p=images_.find(k);if(p!=images_.end())return p->second;
  if(images_.size()>=32768){++dropped_;++counters_.imageDrops;return {};}
  auto v=std::make_shared<Image>();v->id=next_++;images_[k]=v;return v;
 }
 ViewPtr view(VkDevice d,VkImageView v)const {
  auto p=views_.find({d,handle(v)});return p==views_.end()?ViewPtr{}:p->second;
 }
 template<class Map> static void eraseDevice(Map& values,VkDevice d) {
  for(auto i=values.begin();i!=values.end();)if(i->first.first==d)i=values.erase(i);else ++i;
 }
 using SetIterator=std::map<Key,Set>::iterator;
 SetIterator eraseSet(SetIterator it) {
  recentSets_.erase(it->second.recent);return sets_.erase(it);
 }
 void eraseSet(const Key& key){auto it=sets_.find(key);if(it!=sets_.end())eraseSet(it);}
 void touch(SetIterator it){recentSets_.splice(recentSets_.end(),recentSets_,it->second.recent);}
 SetIterator ensureSet(const Key& key,bool recovering) {
  auto it=sets_.find(key);if(it!=sets_.end()){touch(it);return it;}
  // The old cache stopped accepting new sets when full. BeamNG can retain more
  // sets than this across loading and gameplay, starving final-eye observations.
  // Evict the least recently observed entry; never increase the memory bound.
  if(sets_.size()>=setCapacity_){eraseSet(recentSets_.front());++counters_.setEvictions;++dropped_;}
  recentSets_.push_back(key);it=sets_.try_emplace(key).first;it->second.recent=std::prev(recentSets_.end());
  if(recovering){it->second.partial=true;++counters_.setRecoveries;}
  return it;
 }
public:
 // Small capacities are useful for churn tests; production keeps at most 16K.
 explicit Resources(size_t setCapacity=16384):setCapacity_(std::clamp<size_t>(setCapacity,1,16384)){}
 Resources(const Resources&)=delete;
 Resources& operator=(const Resources&)=delete;
 uint64_t revision()const{return revision_;}
 uint64_t dropped()const{return dropped_;}
 auto counters()const{return counters_;}
 size_t setCount()const{return sets_.size();}
 size_t setCapacity()const{return setCapacity_;}
 void createImage(VkDevice d,VkImage i,const VkImageCreateInfo& info) {
  images_.erase({d,handle(i)});auto p=image(d,i);if(!p)return;
  p->width=info.extent.width;p->height=info.extent.height;p->format=info.format;p->usage=info.usage;
  p->samples=info.samples;p->layers=info.arrayLayers;p->mips=info.mipLevels;p->described=true;
 }
 void destroyImage(VkDevice d,VkImage i){images_.erase({d,handle(i)});++revision_;}
 void createView(VkDevice d,VkImageView v,const VkImageViewCreateInfo& info) {
  Key k{d,handle(v)};views_.erase(k);
  if(views_.size()>=65536){++dropped_;++counters_.viewDrops;return;}
  auto p=std::make_shared<View>();p->id=next_++;p->image=image(d,info.image);
  p->range=info.subresourceRange;p->format=info.format;p->type=info.viewType;p->partial=info.pNext!=nullptr;
  views_[k]=p;
 }
 void destroyView(VkDevice d,VkImageView v){views_.erase({d,handle(v)});++revision_;}
 template<class Info> void pass(VkDevice d,VkRenderPass p,const Info& info) {
  Key k{d,handle(p)};passes_.erase(k);if(passes_.size()>=8192){++dropped_;++counters_.passDrops;return;}
  Pass value;value.partial=info.pNext || info.subpassCount!=1 || !info.pSubpasses;
  if(info.subpassCount && info.pSubpasses){const auto& s=info.pSubpasses[0];
   if(s.colorAttachmentCount && s.pColorAttachments)value.color=s.pColorAttachments[0].attachment;
   if(s.pDepthStencilAttachment)value.depth=s.pDepthStencilAttachment->attachment;
   value.partial|=s.colorAttachmentCount>1 || s.inputAttachmentCount || s.pResolveAttachments || s.preserveAttachmentCount;
  }
  passes_[k]=value;
 }
 void forgetPass(VkDevice d,VkRenderPass p){passes_.erase({d,handle(p)});}
 void framebuffer(VkDevice d,VkFramebuffer f,const VkFramebufferCreateInfo& info) {
  Key k{d,handle(f)};framebuffers_.erase(k);
  if(framebuffers_.size()>=16384){++dropped_;++counters_.framebufferDrops;return;}
  Framebuffer value;value.partial=info.pNext || (info.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) || info.attachmentCount>16;
  if(!(info.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && info.pAttachments)
   for(uint32_t n=0;n<std::min(16u,info.attachmentCount);++n)value.views.push_back(view(d,info.pAttachments[n]));
  framebuffers_[k]=std::move(value);
 }
 void forgetFramebuffer(VkDevice d,VkFramebuffer f){framebuffers_.erase({d,handle(f)});}
 Targets targets(VkDevice d,VkRenderPass pass,VkFramebuffer fb)const {
  Targets out;auto p=passes_.find({d,handle(pass)});auto f=framebuffers_.find({d,handle(fb)});
  if(p==passes_.end() || f==framebuffers_.end()){out.partial=true;return out;}
  out.partial=p->second.partial || f->second.partial;
  auto get=[&](uint32_t slot)->ViewPtr{
   if(slot==VK_ATTACHMENT_UNUSED)return {};
   if(slot>=f->second.views.size() || !f->second.views[slot]){out.partial=true;return {};}
   const auto& v=f->second.views[slot];
   if(v->partial || !v->image || !v->image->described)out.partial=true;
   return f->second.views[slot];
  };
  out.color=get(p->second.color);out.depth=get(p->second.depth);return out;
 }
 void allocateSets(VkDevice d,const VkDescriptorSetAllocateInfo& info,const VkDescriptorSet* values) {
  ++revision_;if(!values)return;
  for(uint32_t n=0;n<info.descriptorSetCount;++n){Key k{d,handle(values[n])};eraseSet(k);
   auto it=ensureSet(k,false);it->second.pool=handle(info.descriptorPool);it->second.partial=info.pNext!=nullptr;
  }
 }
 void freeSets(VkDevice d,uint32_t count,const VkDescriptorSet* values) {
  ++revision_;for(uint32_t n=0;values && n<count;++n)eraseSet({d,handle(values[n])});
 }
 void resetPool(VkDevice d,VkDescriptorPool pool) {
  // Recovered sets have unknown pool ownership. Discard them on ANY reset in
  // that device; retaining them could confuse a later reused descriptor handle.
  ++revision_;for(auto i=sets_.begin();i!=sets_.end();)if(i->first.first==d && (!i->second.pool || i->second.pool==handle(pool)))i=eraseSet(i);else ++i;
 }
 void unknown(VkDevice d,VkDescriptorSet value) {
  ++revision_;++counters_.unknownUpdates;auto p=ensureSet({d,handle(value)},true);p->second.partial=true;p->second.slots.clear();
 }
 void update(VkDevice d,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy) {
  ++revision_;
  for(uint32_t n=0;writes && n<count;++n){const auto& w=writes[n];
   if(w.pNext || w.descriptorType==VK_DESCRIPTOR_TYPE_MUTABLE_EXT){unknown(d,w.dstSet);continue;}
   if(!imageType(w.descriptorType))continue;
   // Multi-element writes can spill over layout bindings. Never invent edges.
   if(w.descriptorCount!=1 || !w.pImageInfo){unknown(d,w.dstSet);continue;}
   auto key=std::make_pair(w.dstBinding,w.dstArrayElement);
   auto v=view(d,w.pImageInfo[0].imageView);
   // Ordinary material textures must not consume the 128 candidate-image slots.
   // Erase a previous target at that slot if the descriptor is overwritten.
   if(!candidate(v)){auto dst=sets_.find({d,handle(w.dstSet)});if(dst!=sets_.end()){dst->second.slots.erase(key);touch(dst);}continue;}
   if(!v)++counters_.missingViews;
   auto dst=ensureSet({d,handle(w.dstSet)},true);
   if(dst->second.slots.size()>=128 && !dst->second.slots.count(key)){dst->second.partial=true;++dropped_;++counters_.slotDrops;continue;}
   dst->second.slots[key]={v,w.descriptorType};
  }
  for(uint32_t n=0;copy && n<copies;++n){const auto& c=copy[n];
   if(c.pNext || c.descriptorCount!=1){unknown(d,c.dstSet);continue;}
   // Snapshot BEFORE inserting the destination: at capacity that insertion may
   // evict the source. A known slot remains useful even if other slots are lost.
   Descriptor value{};bool found=false,partial=true;auto src=sets_.find({d,handle(c.srcSet)});
   if(src!=sets_.end()){touch(src);partial=src->second.partial;auto from=src->second.slots.find({c.srcBinding,c.srcArrayElement});
    if(from!=src->second.slots.end()){value=from->second;found=true;}}
   auto dst=ensureSet({d,handle(c.dstSet)},true);auto key=std::make_pair(c.dstBinding,c.dstArrayElement);dst->second.partial|=partial;
   if(!found){dst->second.slots.erase(key);continue;}
   if(dst->second.slots.size()>=128 && !dst->second.slots.count(key)){dst->second.partial=true;++dropped_;++counters_.slotDrops;continue;}
   dst->second.slots[key]=value;
  }
 }
 void collect(VkDevice d,const BoundSets& bound,Uses& out) {
  out.partial|=bound.partial;
  for(auto id:bound.sets)if(id){auto s=sets_.find({d,handle(id)});
   if(s==sets_.end()){out.partial=true;++counters_.setMisses;continue;}touch(s);out.partial|=s->second.partial;
   for(const auto& [slot,entry]:s->second.slots)if(candidate(entry.view)){
    auto& list=entry.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?out.storage:out.reads;
    Uses::add(list,entry.view,out.partial);
   }
  }
 }
 void observeEye(VkDevice d,VkImage i,int eye) {
  if(eye<0 || eye>1)return;auto p=image(d,i);if(p)p->eyeMask|=1u<<eye;
 }
 void transfer(VkDevice d,VkImage src,VkImage dst,const char* op,uint32_t regions) {
  auto s=image(d,src),t=image(d,dst);if(!s || !t)return;
  // Ignore material uploads; retain links involving any rendering/storage target
  // or an imported image whose usage is unknown.
  constexpr uint32_t mask=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
  if(s->described && t->described && !(s->usage&mask) && !(t->usage&mask))return;
  auto key=std::make_tuple(s->id,t->id,std::string(op),regions);
  if(transfers_.size()>=256 && !transfers_.count(key)){++dropped_;++counters_.transferDrops;return;}
  auto& edge=transfers_[key];edge.src=s;edge.dst=t;edge.op=op;edge.regions=regions;++edge.count;
 }
 template<class Emit> void reportTransfers(Emit emit) {
  for(const auto& [key,t]:transfers_)emit(t.src,t.dst,t.op,t.regions,t.count);
  transfers_.clear();
 }
 void destroyDevice(VkDevice d) {
  eraseDevice(images_,d);eraseDevice(views_,d);
  for(auto i=sets_.begin();i!=sets_.end();)if(i->first.first==d)i=eraseSet(i);else ++i;
  eraseDevice(passes_,d);eraseDevice(framebuffers_,d);++revision_;
 }
};
}
