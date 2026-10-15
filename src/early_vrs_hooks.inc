// Vulkan adapter for early_vrs.hpp. Included after lookupDevice/lookupCommand.
// All tracking calls are opt-in, CPU-only, and protected by the layer mutex.
namespace {
int m8ClassifiedEye(VkImage);
beamng_early::Tracker* m833Command(VkCommandBuffer cmd){
 auto d=commandDevices.find(cmd);if(d==commandDevices.end())return nullptr;
 auto t=m833Trackers.find(d->second);return t==m833Trackers.end()?nullptr:&t->second;
}
VkImage m833Output(const VkRenderPassBeginInfo* info){
 if(!info || info->pNext || info->renderArea.offset.x || info->renderArea.offset.y ||
    info->renderArea.extent.width!=m813Width() || info->renderArea.extent.height!=m813Height())return VK_NULL_HANDLE;
 auto fb=framebuffers.find(info->framebuffer);auto slot=m833ColorSlots.find(info->renderPass);
 auto views=m74dAttachments.find(info->framebuffer);
 if(fb==framebuffers.end() || fb->second.layers!=1 || fb->second.width!=m813Width() || fb->second.height!=m813Height() ||
    slot==m833ColorSlots.end() || views==m74dAttachments.end() || slot->second>=views->second.views.size())return VK_NULL_HANDLE;
 auto view=views->second.views[slot->second];if(!m833ColorViews.count(view))return VK_NULL_HANDLE;
 auto image=m74gViews.find(view);return image==m74gViews.end()?VK_NULL_HANDLE:image->second.image;
}
void m833Begin(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,bool inlinePass){
 beamng_cpu::Sample timing(beamng_cpu::Pass,m833Enabled() && m817LogLevel()>=1);
 if(!m833Enabled())return;std::lock_guard<std::mutex> guard(mu);
 auto t=m833Command(cmd);if(!t)return;t->extent(m813Width(),m813Height());
 auto image=inlinePass?m833Output(info):VK_NULL_HANDLE;
 t->beginPass(m833Id(cmd),m833Id(image),image?m8ClassifiedEye(image):-1,info?m833Id(info->renderPass):0);
}
void m833End(VkCommandBuffer cmd){
 if(!m833Enabled())return;VkDevice device=VK_NULL_HANDLE;
 {beamng_cpu::Sample timing(beamng_cpu::Pass,m817LogLevel()>=1);
  std::lock_guard<std::mutex> guard(mu);
  auto t=m833Command(cmd);if(t)t->endPass(m833Id(cmd),GetTickCount64());
  auto d=commandDevices.find(cmd);if(d!=commandDevices.end())device=d->second;
 }
 if(device)m833Report(device);
}
void m833Invalidate(VkCommandBuffer cmd){
 if(!m833Enabled())return;std::lock_guard<std::mutex> guard(mu);
 auto t=m833Command(cmd);if(t)t->invalidate(m833Id(cmd));
}
int m833Eye(VkDevice device,VkCommandBuffer cmd,const VkRenderPassBeginInfo* info){
 // Called inside m57SelectShadow's mutex. This is the ONLY widened classifier;
 // crop and debug outlines keep their original eye/output scope.
 if(m833Mode()!=2 || !m833Enabled())return -1;
 auto t=m833Trackers.find(device);if(t==m833Trackers.end())return -1;
 t->second.extent(m813Width(),m813Height());
 const auto image=m833Output(info);const auto command=t->second.commands.find(m833Id(cmd));
 // A previously learned image must not widen a secondary-only or otherwise
 // unsupported pass when the game later changes how it records that target.
 if(!image || command==t->second.commands.end() || !command->second.recording || command->second.output!=m833Id(image))return -1;
 const auto eye=t->second.eye(m833Id(image),GetTickCount64(),info?m833Id(info->renderPass):0);
 return eye;
}
// Resolve only the needed function pointer, and update tracking under the same
// lock. The previous path copied the whole dispatch table and locked mu twice
// for every draw/bind/unsupported command, even outside candidate passes.
template<class Fn,class Work>
Fn m834Command(VkCommandBuffer cmd,Fn DeviceDispatch::*member,beamng_cpu::Category category,Work work){
 beamng_cpu::Sample timing(category,m833Enabled() && m817LogLevel()>=1);
 std::lock_guard<std::mutex> guard(mu);
 const auto c=commandDevices.find(cmd);if(c==commandDevices.end())return nullptr;
 const auto d=devices.find(c->second);if(d==devices.end())return nullptr;
 if(m833Enabled()){
  auto t=m833Trackers.find(c->second);if(t!=m833Trackers.end())work(t->second);
 }
 return d->second.*member;
}
void m834Report(){
 // No per-draw logging. Snapshot pool state separately from the layer mutex.
 uint64_t hits,misses;size_t idle;
 {std::lock_guard<std::mutex> guard(m823CreateMutex);
  hits=m834FramebufferCache.hits;misses=m834FramebufferCache.misses;idle=m834FramebufferCache.idle();}
 char b[400]{};sprintf_s(b,"M8.34 FB_REUSE hits=%llu misses=%llu idle=%zu capacity=64 driverCreates=%llu driverCreateMs=%.3f (cumulative CPU wall time, not GPU)",
  hits,misses,idle,m834FbCreateCalls.load(),double(m834FbCreateNs.load())/1e6);log(b);
 if(m833Enabled() && m817LogLevel()>=1){
  const char* names[]={"draw","bind","update","pass"};
  for(unsigned i=0;i<beamng_cpu::Count;++i){const auto& c=beamng_cpu::counters[i];const auto n=c.samples.load();
   sprintf_s(b,"M8.34 CPU_SAMPLE category=%s samples=%llu avgUs=%.3f maxUs=%.3f every=1024 (CPU wall time incl lock waits; not GPU or total layer cost)",
    names[i],n,n?double(c.ns.load())/double(n)/1000.0:0.0,double(c.maxNs.load())/1000.0);log(b);
  }
 }
}
void m833Report(VkDevice device){
 if(!m833Enabled())return;
 // Brief reporting once every five seconds, including zero coverage. No
 // per-draw file logging. Counters describe recording, never GPU duration.
 static std::atomic<uint64_t> last{0};auto now=GetTickCount64();auto prev=last.load();
 if(now-prev<5000 || !last.compare_exchange_strong(prev,now))return;
 beamng_early::Stats s{};size_t learned=0,images=0;unsigned ready=0;
 {std::lock_guard<std::mutex> guard(mu);auto t=m833Trackers.find(device);if(t==m833Trackers.end())return;
  s=t->second.stats;learned=t->second.evidence.size();images=t->second.images.size();
  for(const auto& [id,e]:t->second.evidence)if(t->second.eye(id,now)>=0)++ready;
 }
 char b[480]{};sprintf_s(b,"M8.33 EARLY_VRS mode=%s eyeDraws=%llu links=%llu images=%zu learned=%zu ready=%u selected=%llu binds=%llu ambiguous=%llu noInput=%llu unsupported=%llu descriptorScans=%llu scanSkips=%llu (recorded, not GPU timings)",
  m833Mode()==2?"on":"trace",s.draws,s.observed,images,learned,ready,s.selected,s.binds,s.ambiguous,s.noInput,s.unsupported,s.descriptorScans,s.scanSkips);log(b);
}
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo* i){
 auto d=lookupCommand(c);if(!d.early_vkBeginCommandBuffer)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkBeginCommandBuffer(c,i);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833ActiveEarlier.erase(c);auto t=m833Command(c);if(t)t->resetCommand(m833Id(c),r==VK_SUCCESS);}
 return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer c,VkCommandBufferResetFlags f){
 auto d=lookupCommand(c);if(!d.early_vkResetCommandBuffer)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkResetCommandBuffer(c,f);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833ActiveEarlier.erase(c);auto t=m833Command(c);if(t)t->resetCommand(m833Id(c));}return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device,VkCommandPool p,VkCommandPoolResetFlags f){
 auto d=lookupDevice(device);if(!d.early_vkResetCommandPool)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkResetCommandPool(device,p,f);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833Trackers[device].resetCommands(m833Id(p));}return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device,VkCommandPool p,const VkAllocationCallbacks* a){
 auto d=lookupDevice(device);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);auto& t=m833Trackers[device];
  for(const auto& [id,c]:t.commands)if(c.pool==m833Id(p))commandDevices.erase(reinterpret_cast<VkCommandBuffer>(uintptr_t(id)));
  t.resetCommands(m833Id(p),true);
 }if(d.early_vkDestroyCommandPool)d.early_vkDestroyCommandPool(device,p,a);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* o){
 auto d=lookupDevice(device);if(!d.early_vkAllocateDescriptorSets)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkAllocateDescriptorSets(device,i,o);
 if(m833Enabled() && r==VK_SUCCESS && i && o){std::lock_guard<std::mutex> g(mu);
  auto& t=m833Trackers[device];for(uint32_t j=0;j<i->descriptorSetCount;++j)t.allocateSet(m833Id(o[j]),m833Id(i->descriptorPool));
 }return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device,VkDescriptorPool p,uint32_t n,const VkDescriptorSet* s){
 auto d=lookupDevice(device);if(!d.early_vkFreeDescriptorSets)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkFreeDescriptorSets(device,p,n,s);
 if(m833Enabled() && s){std::lock_guard<std::mutex> g(mu);for(uint32_t j=0;j<n;++j)m833Trackers[device].eraseSet(m833Id(s[j]));}return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device,VkDescriptorPool p,VkDescriptorPoolResetFlags f){
 auto d=lookupDevice(device);if(!d.early_vkResetDescriptorPool)return VK_ERROR_INITIALIZATION_FAILED;
 auto r=d.early_vkResetDescriptorPool(device,p,f);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833Trackers[device].resetDescriptors(m833Id(p));}return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device,VkDescriptorPool p,const VkAllocationCallbacks* a){
 auto d=lookupDevice(device);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833Trackers[device].resetDescriptors(m833Id(p));}
 if(d.early_vkDestroyDescriptorPool)d.early_vkDestroyDescriptorPool(device,p,a);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device,uint32_t n,const VkWriteDescriptorSet* w,uint32_t m,const VkCopyDescriptorSet* cp){
 auto d=lookupDevice(device);if(d.early_vkUpdateDescriptorSets)d.early_vkUpdateDescriptorSets(device,n,w,m,cp);
 beamng_cpu::Sample timing(beamng_cpu::Update,m833Enabled() && m817LogLevel()>=1);
 if(!m833Enabled())return;std::lock_guard<std::mutex> g(mu);auto& t=m833Trackers[device];
 for(uint32_t j=0;w && j<n;++j){const auto& v=w[j];
  // Multi-element writes can spill across bindings. Do not guess without a
  // descriptor-layout interpreter; the whole set becomes unknown until freed.
  if(v.pNext || v.descriptorType==VK_DESCRIPTOR_TYPE_MUTABLE_EXT){t.unknown(m833Id(v.dstSet));continue;}
  if(v.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && v.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
     v.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && v.descriptorType!=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)continue;
  if(v.descriptorCount!=1 || !v.pImageInfo){t.unknown(m833Id(v.dstSet));continue;}
  VkImage image=VK_NULL_HANDLE;
  if(v.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || v.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE){
   auto view=m74gViews.find(v.pImageInfo->imageView);
   if(view!=m74gViews.end() && view->second.device==device && m833ColorViews.count(view->first))image=view->second.image;
  }
  t.write(m833Id(v.dstSet),v.dstBinding,v.dstArrayElement,m833Id(image));
 }
 for(uint32_t j=0;cp && j<m;++j){const auto& v=cp[j];
  if(v.pNext || v.descriptorCount!=1)t.unknown(m833Id(v.dstSet));
  else t.copy(m833Id(v.srcSet),v.srcBinding,v.srcArrayElement,m833Id(v.dstSet),v.dstBinding,v.dstArrayElement);
 }
}
#define EARLY_TEMPLATE(name) \
extern "C" VKAPI_ATTR void VKAPI_CALL name(VkDevice device,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void* data){ \
 auto d=lookupDevice(device);if(d.early_##name)d.early_##name(device,s,t,data); \
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833Trackers[device].unknown(m833Id(s));} }
EARLY_TEMPLATE(vkUpdateDescriptorSetWithTemplate)
EARLY_TEMPLATE(vkUpdateDescriptorSetWithTemplateKHR)
#undef EARLY_TEMPLATE
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet* s,uint32_t dc,const uint32_t* offsets){
 auto fn=m834Command(c,&DeviceDispatch::early_vkCmdBindDescriptorSets,beamng_cpu::Bind,[&](auto& t){
  if(p!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
  if(n>8 || (n && !s)){t.invalidate(m833Id(c));return;}
  std::array<uint64_t,8> ids{};for(uint32_t j=0;j<n;++j)ids[j]=m833Id(s[j]);
  t.bind(m833Id(c),m833Id(l),first,n,ids.data());
 });
 if(fn)fn(c,p,l,first,n,s,dc,offsets);
}
#define EARLY_FORWARD(name,signature,arguments,tracking) \
extern "C" VKAPI_ATTR void VKAPI_CALL name signature {auto fn=m834Command(c,&DeviceDispatch::early_##name,beamng_cpu::Draw,[&](auto& t){tracking;});if(fn)fn arguments;}
EARLY_FORWARD(vkCmdPushDescriptorSetKHR,(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t s,uint32_t n,const VkWriteDescriptorSet* w),(c,p,l,s,n,w),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdPushDescriptorSetWithTemplateKHR,(VkCommandBuffer c,VkDescriptorUpdateTemplate t,VkPipelineLayout l,uint32_t s,const void* data),(c,t,l,s,data),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdExecuteCommands,(VkCommandBuffer c,uint32_t n,const VkCommandBuffer* s),(c,n,s),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdDispatch,(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z),(c,x,y,z),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdDispatchIndirect,(VkCommandBuffer c,VkBuffer b,VkDeviceSize o),(c,b,o),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdDraw,(VkCommandBuffer c,uint32_t v,uint32_t n,uint32_t first,uint32_t instance),(c,v,n,first,instance),if(v && n)t.draw(m833Id(c)))
EARLY_FORWARD(vkCmdDrawIndexed,(VkCommandBuffer c,uint32_t v,uint32_t n,uint32_t first,int32_t offset,uint32_t instance),(c,v,n,first,offset,instance),if(v && n)t.draw(m833Id(c)))
EARLY_FORWARD(vkCmdDrawIndirect,(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t stride),(c,b,o,n,stride),if(n)t.draw(m833Id(c)))
EARLY_FORWARD(vkCmdDrawIndexedIndirect,(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t stride),(c,b,o,n,stride),if(n)t.draw(m833Id(c)))
#define EARLY_COUNT(name) EARLY_FORWARD(name,(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkBuffer cb,VkDeviceSize co,uint32_t n,uint32_t stride),(c,b,o,cb,co,n,stride),if(n)t.draw(m833Id(c)))
EARLY_COUNT(vkCmdDrawIndirectCount)
EARLY_COUNT(vkCmdDrawIndexedIndirectCount)
EARLY_COUNT(vkCmdDrawIndirectCountKHR)
EARLY_COUNT(vkCmdDrawIndexedIndirectCountKHR)
#undef EARLY_COUNT
EARLY_FORWARD(vkCmdClearColorImage,(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,uint32_t n,const VkImageSubresourceRange* r),(c,i,l,v,n,r),t.invalidate(m833Id(c)))
EARLY_FORWARD(vkCmdClearDepthStencilImage,(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,uint32_t n,const VkImageSubresourceRange* r),(c,i,l,v,n,r),t.invalidate(m833Id(c)))
#undef EARLY_FORWARD
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device,VkImage i,const VkAllocationCallbacks* a){
 auto d=lookupDevice(device);
 if(m833Enabled()){std::lock_guard<std::mutex> g(mu);m833Trackers[device].eraseImage(m833Id(i));m833ForgetImage(i);}
 if(d.early_vkDestroyImage)d.early_vkDestroyImage(device,i,a);
}
