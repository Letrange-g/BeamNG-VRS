#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

// Diagnostic only. Never wait for a query, fence, queue or device. Query pools
// belong to ONE primary command-buffer recording. Read/destroy them
// only when the application legally resets/frees that recording (or its pool /
// device). A successful submission is required before reading any query result.
// This avoids query reuse races and does not replace application fences.
namespace beamng_profile {
template<class T> uint64_t id(T h){return uint64_t(uintptr_t(h));}
inline bool elapsed(uint64_t a,uint64_t b,unsigned bits,double period,double& ms){
 if(!bits || bits>64 || !std::isfinite(period) || period<=0)return false;
 const auto mask=bits==64?~uint64_t(0):(uint64_t(1)<<bits)-1;
 ms=double((b-a)&mask)*period/1e6;
 // A wrap may be ambiguous on very short counters. Reject implausible samples.
 return std::isfinite(ms) && ms>=0 && ms<1000;
}
struct Api {
 PFN_vkCreateQueryPool create=nullptr;PFN_vkDestroyQueryPool destroy=nullptr;
 PFN_vkGetQueryPoolResults results=nullptr;PFN_vkCmdResetQueryPool reset=nullptr;
 PFN_vkCmdWriteTimestamp stamp=nullptr;
 bool complete()const{return create && destroy && results && reset && stamp;}
};
struct Meta {
 uint64_t pass=0,pipeline=0;
 uint32_t width=0,height=0,colors=0,samples=0,format=0,subpasses=0,reject=~0u;
 bool depth=false,compute=false,secondary=false,rateAttachment=false;
 int eye=-1;
 auto key()const{return std::tie(compute,pass,pipeline,width,height,colors,samples,format,subpasses,depth,secondary,rateAttachment,eye,reject);}
 bool operator<(const Meta& b)const{return key()<b.key();}
};
struct Event { Meta meta;uint64_t draws=0,vrsDraws=0;bool complete=false; };
struct Summary { double total=0,maximum=0;uint64_t count=0,draws=0,vrsDraws=0; };
class Profiler {
 static constexpr unsigned maxEvents=128,maxLive=16;
 struct Pool { VkDevice device{};unsigned bits=0;bool usable=false; };
 struct Record {
  VkDevice device{};VkCommandPool owner{};VkQueryPool queries{};
  bool primary=false,selected=false,submitted=false,invalid=false,inside=false,vrs=false;
  unsigned bits=0;int active=-1;
  uint64_t graphicsPipeline=0,computePipeline=0;
  std::vector<Event> events;
 };
 struct Device {
  Api api;double period=0;std::vector<VkQueueFamilyProperties> families;
  unsigned live=0;
 };
 std::mutex mutex_;
 std::map<VkDevice,Device> devices_;
 std::map<std::pair<VkDevice,VkCommandPool>,Pool> pools_;
 std::map<VkCommandBuffer,Record> commands_;
 std::map<std::pair<VkDevice,VkRenderPass>,Meta> passes_;
 std::map<std::pair<VkDevice,VkPipeline>,uint64_t> pipelines_;
 std::map<Meta,Summary> summary_;
 uint64_t nextId_=1,recordings_=0,selected_=0,completed_=0,unavailable_=0,dropped_=0,unsupported_=0,failures_=0;
 uint32_t random_=0x83a52e19;
 std::chrono::steady_clock::time_point lastReport_{};
 void (*log_)(const char*)=nullptr;
 unsigned random(){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return random_;}
 void report(bool force=false){
  const auto now=std::chrono::steady_clock::now();
  if(!log_ || (!force && now-lastReport_<std::chrono::seconds(5)))return;
  lastReport_=now;
  char b[800]{};
  std::snprintf(b,sizeof(b),"M8.35 GPU_PROFILE recordings=%llu selected=%llu samples=%llu unavailable=%llu dropped=%llu unsupported=%llu failures=%llu groups=%zu (counters cumulative; ranking since previous report; sampled GPU spans, not frame totals)",
   (unsigned long long)recordings_,(unsigned long long)selected_,(unsigned long long)completed_,(unsigned long long)unavailable_,(unsigned long long)dropped_,(unsigned long long)unsupported_,(unsigned long long)failures_,summary_.size());log_(b);
  std::vector<std::pair<Meta,Summary>> ranked(summary_.begin(),summary_.end());
  std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.second.total>b.second.total;});
  for(size_t i=0;i<std::min<size_t>(20,ranked.size());++i){const auto& m=ranked[i].first;const auto& s=ranked[i].second;
   std::snprintf(b,sizeof(b),"M8.35 GPU_PASS rank=%zu kind=%s pass=%llu firstPipeline=%llu size=%ux%u colors=%u firstColorFormat=%u samplesMask=%u depth=%u subpasses=%u eye=%d secondary=%u rateAttachment=%u shadowReject=0x%X count=%llu avgMs=%.4f maxMs=%.4f sampledTotalMs=%.3f drawCalls=%llu vrsDrawCalls=%llu",
    i+1,m.compute?"compute":"graphics",(unsigned long long)m.pass,(unsigned long long)m.pipeline,m.width,m.height,m.colors,m.format,m.samples,unsigned(m.depth),m.subpasses,m.eye,unsigned(m.secondary),unsigned(m.rateAttachment),m.reject,
    (unsigned long long)s.count,s.total/double(s.count),s.maximum,s.total,(unsigned long long)s.draws,(unsigned long long)s.vrsDraws);log_(b);
  }
  summary_.clear(); // Startup/loading passes must not dominate later stationary-scene windows.
 }
 void retire(Record& r){
  auto d=devices_.find(r.device);if(!r.queries || d==devices_.end())return;
  auto& dev=d->second;
  if(r.submitted && !r.invalid && !r.events.empty()){
   // Two uint64s per query: timestamp and availability. NEVER WAIT_BIT.
   std::vector<std::array<uint64_t,2>> data(r.events.size()*2);
   const auto result=dev.api.results(r.device,r.queries,0,uint32_t(data.size()),data.size()*sizeof(data[0]),data.data(),sizeof(data[0]),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
   if(result==VK_SUCCESS || result==VK_NOT_READY){
    for(size_t i=0;i<r.events.size();++i){const auto& e=r.events[i];double ms=0;
     if(!e.complete || !data[2*i][1] || !data[2*i+1][1] || !elapsed(data[2*i][0],data[2*i+1][0],r.bits,dev.period,ms)){++unavailable_;continue;}
     if(summary_.size()>=512 && !summary_.count(e.meta)){++dropped_;continue;}
     auto& s=summary_[e.meta];s.total+=ms;s.maximum=std::max(s.maximum,ms);++s.count;s.draws+=e.draws;s.vrsDraws+=e.vrsDraws;++completed_;
    }
   }else ++failures_;
  }
  dev.api.destroy(r.device,r.queries,nullptr);r.queries=VK_NULL_HANDLE;--dev.live;
 }
 void start(VkCommandBuffer cmd,Record& r,Meta meta,bool force){
  if(!r.selected || r.invalid || r.inside || r.active>=0 || (!force && (random()&3)))return;
  if(r.events.size()>=maxEvents){++dropped_;return;}
  auto d=devices_.find(r.device);if(d==devices_.end())return;auto& dev=d->second;
  if(!r.queries){
   if(dev.live>=maxLive){++dropped_;return;}
   VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=maxEvents*2;
   if(dev.api.create(r.device,&info,nullptr,&r.queries)!=VK_SUCCESS || !r.queries){++failures_;r.selected=false;return;}
   ++dev.live;dev.api.reset(cmd,r.queries,0,maxEvents*2);
  }
  r.active=int(r.events.size());r.events.push_back({meta});
  // Both boundaries use BOTTOM_OF_PIPE. Spans remain susceptible to pipeline
  // overlap; do not add them up or interpret them as exclusive shader duration.
  dev.api.stamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.queries,unsigned(r.active)*2);
 }
 void finish(VkCommandBuffer cmd,Record& r){
  if(r.active<0)return;
  auto d=devices_.find(r.device);if(d==devices_.end())return;
  d->second.api.stamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.queries,unsigned(r.active)*2+1);
  r.events[size_t(r.active)].complete=true;r.active=-1;
 }
public:
 explicit Profiler(void(*logger)(const char*)=nullptr):log_(logger){}
 void device(VkDevice device,Api api,double period,std::vector<VkQueueFamilyProperties> families){
  std::lock_guard<std::mutex> g(mutex_);
  if(devices_.size()<32 && api.complete() && std::isfinite(period) && period>0)devices_[device]={api,period,std::move(families),0};
  else ++unsupported_;
 }
 void pool(VkDevice d,VkCommandPool p,uint32_t family,VkCommandPoolCreateFlags flags){
  std::lock_guard<std::mutex> g(mutex_);auto dev=devices_.find(d);
  if(dev==devices_.end() || pools_.size()>=8192)return;
  unsigned bits=family<dev->second.families.size()?dev->second.families[family].timestampValidBits:0;
  pools_[{d,p}]={d,bits,bits>0 && bits<=64 && !(flags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT)};
 }
 void allocate(VkDevice d,VkCommandPool p,VkCommandBuffer c,bool primary){
  std::lock_guard<std::mutex> g(mutex_);if(commands_.size()>=8192)return;
  Record r;r.device=d;r.owner=p;r.primary=primary;commands_[c]=std::move(r);
 }
 void begin(VkCommandBuffer c,const VkCommandBufferBeginInfo* info,bool force=false){
  std::lock_guard<std::mutex> g(mutex_);auto it=commands_.find(c);if(it==commands_.end())return;
  auto& r=it->second;retire(r);auto d=r.device;auto p=r.owner;bool primary=r.primary;r={};r.device=d;r.owner=p;r.primary=primary;++recordings_;
  auto pool=pools_.find({d,p});
  bool supported=primary && info && !info->pNext && !(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) && pool!=pools_.end() && pool->second.usable;
  if(!supported)++unsupported_;
  else {r.bits=pool->second.bits;r.selected=force || !(random()&15);if(r.selected)++selected_;}
  report();
 }
 void reset(VkCommandBuffer c,bool erase=false){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end())return;
  retire(i->second);i->second.selected=false;i->second.invalid=true;if(erase)commands_.erase(i);report();
 }
 std::vector<VkCommandBuffer> resetPool(VkDevice d,VkCommandPool p,bool erase=false){
  std::lock_guard<std::mutex> g(mutex_);std::vector<VkCommandBuffer> removed;
  for(auto i=commands_.begin();i!=commands_.end();)if(i->second.device==d && i->second.owner==p){retire(i->second);i->second.selected=false;i->second.invalid=true;if(erase){removed.push_back(i->first);i=commands_.erase(i);}else ++i;}else ++i;
  if(erase)pools_.erase({d,p});
  report();return removed;
 }
 void submitted(VkCommandBuffer c){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end())return;
  // Replays overwrite queries; exclude their measurements. Simultaneous use is never sampled.
  if(i->second.submitted)i->second.invalid=true;
  i->second.submitted=true;
 }
 template<class Info> void pass(VkDevice d,VkRenderPass p,const Info& info){
  std::lock_guard<std::mutex> g(mutex_);if(passes_.size()>=8192)return;
  Meta m;m.pass=nextId_++;m.subpasses=info.subpassCount;
  for(uint32_t i=0;info.pSubpasses && i<info.subpassCount;++i){const auto& s=info.pSubpasses[i];m.colors=std::max(m.colors,s.colorAttachmentCount);
   m.depth|=s.pDepthStencilAttachment && s.pDepthStencilAttachment->attachment!=VK_ATTACHMENT_UNUSED;
   for(uint32_t j=0;s.pColorAttachments && info.pAttachments && j<s.colorAttachmentCount;++j){auto a=s.pColorAttachments[j].attachment;
    if(a<info.attachmentCount){if(!m.format)m.format=info.pAttachments[a].format;m.samples|=info.pAttachments[a].samples;}
   }
  }
  passes_[{d,p}]=m;
 }
 void forgetPass(VkDevice d,VkRenderPass p){std::lock_guard<std::mutex> g(mutex_);passes_.erase({d,p});}
 void forgetPipeline(VkDevice d,VkPipeline p){std::lock_guard<std::mutex> g(mutex_);pipelines_.erase({d,p});}
 void legacy(VkCommandBuffer c,const VkRenderPassBeginInfo* info,int eye,uint32_t reject,bool secondary,bool shadow,bool force=false){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end() || !info)return;
  auto& r=i->second;auto p=passes_.find({r.device,info->renderPass});
  if(p!=passes_.end()){
   auto m=p->second;m.width=info->renderArea.extent.width;m.height=info->renderArea.extent.height;m.eye=eye;m.reject=reject;m.secondary=secondary;m.rateAttachment=shadow;
   start(c,r,m,force);
  }else ++unsupported_;
  r.inside=true;
 }
 void end(VkCommandBuffer c){std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i!=commands_.end()){finish(c,i->second);i->second.inside=false;}}
 void dynamic(VkCommandBuffer c){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end())return;
  // Do not insert commands between suspended/resumed dynamic-rendering segments.
  i->second.selected=false;++unsupported_;
 }
 void bind(VkCommandBuffer c,VkPipelineBindPoint point,VkPipeline pipeline,bool vrs){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end() || !i->second.selected)return;
  auto& r=i->second;auto key=std::make_pair(r.device,pipeline);auto p=pipelines_.find(key);
  if(p==pipelines_.end() && pipelines_.size()<8192)p=pipelines_.emplace(key,nextId_++).first;
  uint64_t n=p==pipelines_.end()?0:p->second;
  if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){r.graphicsPipeline=n;r.vrs=vrs;}
  if(point==VK_PIPELINE_BIND_POINT_COMPUTE)r.computePipeline=n;
 }
 void draw(VkCommandBuffer c){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end() || i->second.active<0)return;
  auto& r=i->second;auto& e=r.events[size_t(r.active)];if(e.meta.compute)return;
  if(!e.draws)e.meta.pipeline=r.graphicsPipeline;
  ++e.draws;if(r.vrs && e.meta.rateAttachment)++e.vrsDraws;
 }
 void compute(VkCommandBuffer c,bool force=false){
  std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i==commands_.end())return;
  Meta m;m.compute=true;m.pipeline=i->second.computePipeline;start(c,i->second,m,force);
 }
 void endCompute(VkCommandBuffer c){std::lock_guard<std::mutex> g(mutex_);auto i=commands_.find(c);if(i!=commands_.end() && !i->second.inside)finish(c,i->second);}
 void destroyDevice(VkDevice d){
  std::lock_guard<std::mutex> g(mutex_);
  for(auto i=commands_.begin();i!=commands_.end();)if(i->second.device==d){retire(i->second);i=commands_.erase(i);}else ++i;
  for(auto i=pools_.begin();i!=pools_.end();)if(i->second.device==d)i=pools_.erase(i);else ++i;
  for(auto i=passes_.begin();i!=passes_.end();)if(i->first.first==d)i=passes_.erase(i);else ++i;
  for(auto i=pipelines_.begin();i!=pipelines_.end();)if(i->first.first==d)i=pipelines_.erase(i);else ++i;
  devices_.erase(d);report(true);
 }
};
}
