#pragma once
#include "ffr_gpu_image.hpp"
#include <array>
#include <utility>

namespace beamng_ffr {
struct UploadCommands {
 VkCommandPool pool=VK_NULL_HANDLE;
 VkCommandBuffer command=VK_NULL_HANDLE;
 VkFence fence=VK_NULL_HANDLE;
 uint32_t family=0;
};
inline void destroyUploadCommands(const Api& api,VkDevice device,UploadCommands& c){
 if(c.fence)api.destroyFence(device,c.fence,nullptr);
 if(c.pool)api.destroyCommandPool(device,c.pool,nullptr);
 c={};
}
// Caller serializes access. ONLY completed uploads may enter this cache;
// pending submissions remain owned by the layer's existing fence tracker.
// Cache is device-local and queue-family-specific, not resolution-specific:
// command buffers are reset and recorded again for the current eye images.
class UploadCommandCache {
 std::array<UploadCommands,6> idle_{};
 size_t count_=0;
 uint64_t created_=0,reused_=0;
public:
 UploadCommandCache()=default;
 UploadCommandCache(const UploadCommandCache&)=delete;
 UploadCommandCache& operator=(const UploadCommandCache&)=delete;
 size_t idleCount()const{return count_;}
 uint64_t created()const{return created_;}
 uint64_t reused()const{return reused_;}
 UploadCommands acquire(const Api& api,VkDevice device,uint32_t family){
  UploadCommands c{};c.family=family;
  try{
   if(api.resetCommandPool && api.resetFences){
    for(size_t i=0;i<count_;++i)if(idle_[i].family==family){
     c=idle_[i];idle_[i]=idle_[--count_];idle_[count_]={};
     // Fence completion was verified before recycling. Resetting this pool
     // cannot invalidate an executing application or upload command buffer.
     check(api.resetCommandPool(device,c.pool,0),"M8.27 reset upload pool");
     check(api.resetFences(device,1,&c.fence),"M8.27 reset upload fence");
     ++reused_;return c;
    }
   }
   VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   pci.queueFamilyIndex=family;pci.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
   check(api.createCommandPool(device,&pci,nullptr,&c.pool),"M8.27 create upload pool");
   VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   ai.commandPool=c.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
   check(api.allocateCommandBuffers(device,&ai,&c.command),"M8.27 allocate upload command");
   VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   check(api.createFence(device,&fci,nullptr,&c.fence),"M8.27 create upload fence");
   ++created_;return c;
  }catch(...){destroyUploadCommands(api,device,c);throw;}
 }
 // Return false without changing ownership when the fence is not complete.
 bool recycle(const Api& api,VkDevice device,UploadCommands& c,VkResult completion){
  if(completion!=VK_SUCCESS)return false;
  if(c.pool && c.command && c.fence && api.resetCommandPool && api.resetFences && count_<idle_.size()){
   idle_[count_++]=c;c={};
  }else destroyUploadCommands(api,device,c);
  return true;
 }
 // These objects have completed fences. Destroy before the device itself.
 void destroyIdle(const Api& api,VkDevice device){
  while(count_)destroyUploadCommands(api,device,idle_[--count_]);
 }
};
}
