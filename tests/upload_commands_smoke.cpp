#include "ffr_upload_commands.hpp"
#include <cassert>
#include <iostream>
#include <unordered_map>
#include <type_traits>

namespace {
uintptr_t nextHandle=1;
template<class T>T handle(){
 if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(nextHandle++);
 else return T(nextHandle++);
}
struct Pool {uint32_t family;bool busy=false;};
struct Copy {VkBuffer source;VkImage target;size_t bytes;};
std::unordered_map<VkCommandPool,Pool> pools;
std::unordered_map<VkCommandBuffer,VkCommandPool> commandPools;
std::unordered_map<VkCommandBuffer,std::vector<Copy>> recordedCopies;
std::unordered_map<VkBuffer,std::array<uint8_t,4>> sourceBytes;
std::unordered_map<VkImage,std::array<uint8_t,4>> targetBytes;
std::unordered_map<VkFence,bool> fences;
unsigned poolCreates=0,fenceCreates=0,resets=0,begins=0,ends=0,barriers=0,copies=0;
int fail=0;
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice,const VkCommandPoolCreateInfo* ci,const VkAllocationCallbacks*,VkCommandPool* p){
 if(fail==1)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *p=handle<VkCommandPool>();pools.emplace(*p,Pool{ci->queueFamilyIndex});++poolCreates;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo* ci,VkCommandBuffer* cmd){
 assert(pools.count(ci->commandPool));assert(ci->commandBufferCount==1);
 if(fail==2)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *cmd=handle<VkCommandBuffer>();commandPools[*cmd]=ci->commandPool;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence* f){
 if(fail==3)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *f=handle<VkFence>();fences[*f]=false;++fenceCreates;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice,VkCommandPool pool,VkCommandPoolResetFlags){
 assert(!pools.at(pool).busy);
 if(fail==4)return VK_ERROR_OUT_OF_HOST_MEMORY;
 ++resets;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info){
 assert(!pools.at(commandPools.at(cmd)).busy && info->flags==0);
 if(fail==6)return VK_ERROR_OUT_OF_HOST_MEMORY;
 recordedCopies[cmd].clear();++begins;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer cmd){
 assert(!pools.at(commandPools.at(cmd)).busy);
 if(fail==7)return VK_ERROR_OUT_OF_HOST_MEMORY;
 ++ends;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags source,VkPipelineStageFlags target,
 VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t count,const VkImageMemoryBarrier* b){
 assert(count==1);
 if(b->oldLayout==VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR){
  assert(source==VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR && target==VK_PIPELINE_STAGE_TRANSFER_BIT);
  assert(b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 }else{
  assert(b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  assert(source==VK_PIPELINE_STAGE_TRANSFER_BIT && target==VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
  assert(b->newLayout==VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
 }
 ++barriers;
}
VKAPI_ATTR void VKAPI_CALL copy(VkCommandBuffer cmd,VkBuffer source,VkImage target,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* region){
 assert(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count==1 && region->imageExtent.depth==1);
 recordedCopies[cmd].push_back({source,target,size_t(region->imageExtent.width)*region->imageExtent.height});++copies;
}
VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice,uint32_t n,const VkFence* f){
 assert(n==1 && fences.at(*f));
 if(fail==5)return VK_ERROR_OUT_OF_HOST_MEMORY;
 fences[*f]=false;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice,VkCommandPool pool,const VkAllocationCallbacks*){
 assert(!pools.at(pool).busy);assert(pools.erase(pool)==1);
 for(auto it=commandPools.begin();it!=commandPools.end();){
  if(it->second==pool){recordedCopies.erase(it->first);it=commandPools.erase(it);}else ++it;
 }
}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice,VkFence f,const VkAllocationCallbacks*){
 assert(fences.erase(f)==1);
}
void submit(const beamng_ffr::UploadCommands& c){assert(!pools.at(c.pool).busy && !fences.at(c.fence));pools.at(c.pool).busy=true;}
void complete(const beamng_ffr::UploadCommands& c){
 // Mock execution reads current source bytes, not values at recording time.
 for(const auto& op:recordedCopies[c.command]){
  assert(op.bytes<=4);std::copy_n(sourceBytes.at(op.source).begin(),op.bytes,targetBytes.at(op.target).begin());
 }
 pools.at(c.pool).busy=false;fences.at(c.fence)=true;
}
}
int main(){
 using namespace beamng_ffr;
 const VkDevice device=handle<VkDevice>();
 Api api{};api.createCommandPool=createPool;api.allocateCommandBuffers=allocate;
 api.createFence=createFence;api.resetCommandPool=resetPool;api.resetFences=resetFences;
 api.destroyCommandPool=destroyPool;api.destroyFence=destroyFence;
 api.beginCommandBuffer=begin;api.endCommandBuffer=end;api.cmdPipelineBarrier=barrier;api.cmdCopyBufferToImage=copy;
 UploadCommandCache cache;
 for(unsigned i=0;i<100;++i){
  auto c=cache.acquire(api,device,3);assert(pools.at(c.pool).family==3);submit(c);
  const unsigned prior=resets;
  assert(!cache.recycle(api,device,c,VK_NOT_READY));assert(c.pool && c.fence && cache.idleCount()==0);
  assert(!cache.recycle(api,device,c,VK_ERROR_DEVICE_LOST));assert(resets==prior);
  complete(c);assert(cache.recycle(api,device,c,VK_SUCCESS));assert(!c.pool && !c.fence);
 }
 assert(poolCreates==1 && fenceCreates==1 && resets==99);
 assert(cache.created()==1 && cache.reused()==99);
 // A command pool from family 3 must never be used by family 7.
 auto other=cache.acquire(api,device,7);assert(pools.at(other.pool).family==7);
 complete(other);cache.recycle(api,device,other,VK_SUCCESS);assert(cache.idleCount()==2);
 cache.destroyIdle(api,device);assert(pools.empty() && fences.empty());
 // All allocation and reset errors clean up partial ownership.
 for(fail=1;fail<=5;++fail){
  if(fail>=4){
   const int requested=fail;fail=0;
   auto c=cache.acquire(api,device,3);complete(c);cache.recycle(api,device,c,VK_SUCCESS);
   fail=requested;
  }
  bool rejected=false;try{(void)cache.acquire(api,device,3);}catch(const std::runtime_error&){rejected=true;}
  assert(rejected && pools.empty() && fences.empty() && cache.idleCount()==0);
 }
 fail=0;
 // Cache is bounded even after a temporary submission backlog.
 std::array<UploadCommands,8> batch{};
 for(auto& c:batch)c=cache.acquire(api,device,3);
 for(auto& c:batch){complete(c);cache.recycle(api,device,c,VK_SUCCESS);}
 assert(cache.idleCount()==6 && pools.size()==6 && fences.size()==6);
 cache.destroyIdle(api,device);
 // Drivers without reset entrypoints retain the original create/destroy path.
 api.resetFences=nullptr;
 auto c=cache.acquire(api,device,3);complete(c);cache.recycle(api,device,c,VK_SUCCESS);
 assert(cache.idleCount()==0 && pools.empty() && fences.empty());
 api.resetFences=resetFences;
 std::array<StagingSlot,2> sources{};
 std::array<GpuImage,2> targets{};
 for(int eye=0;eye<2;++eye){
  sources[eye].buffer=handle<VkBuffer>();sources[eye].capacity=4;sourceBytes[sources[eye].buffer]={};
  targets[eye].image=handle<VkImage>();targets[eye].width=targets[eye].height=1;targetBytes[targets[eye].image]={};
 }
 const auto key= gazeTransferKey(sources,targets);
 const auto priorResets=resets;
 for(unsigned i=0;i<100;++i){
  auto c=cache.acquire(api,device,3,&key);recordGazeCommands(api,c,sources,targets);
  // Values change AFTER command recording, like newly written mapped staging.
  for(int eye=0;eye<2;++eye)sourceBytes[sources[eye].buffer][0]=uint8_t(i+eye);
  submit(c);assert(!cache.recycle(api,device,c,VK_NOT_READY));complete(c);
  for(int eye=0;eye<2;++eye)assert(targetBytes[targets[eye].image][0]==uint8_t(i+eye));
  cache.recycle(api,device,c,VK_SUCCESS);
 }
 assert(begins==1 && ends==1 && barriers==4 && copies==2 && resets==priorResets);
 assert(cache.recordingReused()==99);
 // Retain recordings for three alternating staging pairs instead of resetting
 // one pool on every slot rotation. Different resource keys never reuse a recording.
 std::array<std::array<StagingSlot,2>,3> rotated{};rotated[0]=sources;
 for(int slot=1;slot<3;++slot)for(int eye=0;eye<2;++eye){
  rotated[slot][eye].buffer=handle<VkBuffer>();rotated[slot][eye].capacity=4;
  sourceBytes[rotated[slot][eye].buffer]={};
 }
 for(int i=0;i<12;++i){
  const auto currentKey=gazeTransferKey(rotated[i%3],targets);
  auto c=cache.acquire(api,device,3,&currentKey);recordGazeCommands(api,c,rotated[i%3],targets);
  submit(c);complete(c);cache.recycle(api,device,c,VK_SUCCESS);
 }
 assert(begins==3 && ends==3 && copies==6 && cache.idleCount()==3);
 // Extent changes, image changes, one-eye uploads and queue families create
 // separate recordings. Full cache repurposing remains bounded and reset-safe.
 targets[0].width=2;
 for(int i=0;i<12;++i){
  const auto currentKey=gazeTransferKey(rotated[i%3],targets);
  auto c=cache.acquire(api,device,3,&currentKey);recordGazeCommands(api,c,rotated[i%3],targets);
  submit(c);complete(c);cache.recycle(api,device,c,VK_SUCCESS);
 }
 assert(begins==6 && cache.idleCount()==6);
 targets[0].image=handle<VkImage>();targetBytes[targets[0].image]={};
 const auto changedKey=gazeTransferKey(sources,targets);
 auto changed=cache.acquire(api,device,3,&changedKey);assert(!changed.recorded);
 recordGazeCommands(api,changed,sources,targets);submit(changed);complete(changed);cache.recycle(api,device,changed,VK_SUCCESS);
 assert(resets>priorResets && cache.idleCount()==6);
 auto oneEye=sources;oneEye[1]={};const auto oneKey=gazeTransferKey(oneEye,targets);
 changed=cache.acquire(api,device,3,&oneKey);const auto priorCopies=copies;
 recordGazeCommands(api,changed,oneEye,targets);assert(copies==priorCopies+1);
 submit(changed);complete(changed);cache.recycle(api,device,changed,VK_SUCCESS);
 // A recording cannot silently switch source buffers without a reset.
 changed=cache.acquire(api,device,3,&oneKey);
 bool rejected=false;try{recordGazeCommands(api,changed,sources,targets);}catch(const std::invalid_argument&){rejected=true;}
 assert(rejected);destroyUploadCommands(api,device,changed);
 cache.destroyIdle(api,device);assert(pools.empty() && fences.empty() && commandPools.empty());
 // Exact-recording fence reset failures clean up the removed cache entry.
 const auto finalKey=gazeTransferKey(sources,targets);
 changed=cache.acquire(api,device,3,&finalKey);recordGazeCommands(api,changed,sources,targets);
 submit(changed);complete(changed);cache.recycle(api,device,changed,VK_SUCCESS);
 fail=5;rejected=false;
 try{(void)cache.acquire(api,device,3,&finalKey);}catch(const std::runtime_error&){rejected=true;}
 assert(rejected && cache.idleCount()==0 && pools.empty() && fences.empty());fail=0;
 for(int error:{6,7}){
  changed=cache.acquire(api,device,3,&key);fail=error;rejected=false;
  try{recordGazeCommands(api,changed,sources,targets);}catch(const std::runtime_error&){rejected=true;}
  assert(rejected && !changed.recorded);destroyUploadCommands(api,device,changed);fail=0;
  assert(pools.empty() && fences.empty());
 }
 std::cout<<"Upload ownership, completion gating, family isolation, failures and bounded reuse passed\n";
 std::cout<<"Recorded transfers: 100 changing payloads used one recording, immutable pending ownership, resource-key/slot/extent invalidation passed\n";
}
