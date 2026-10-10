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
std::unordered_map<VkCommandPool,Pool> pools;
std::unordered_map<VkFence,bool> fences;
unsigned poolCreates=0,fenceCreates=0,resets=0;
int fail=0;
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice,const VkCommandPoolCreateInfo* ci,const VkAllocationCallbacks*,VkCommandPool* p){
 if(fail==1)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *p=handle<VkCommandPool>();pools.emplace(*p,Pool{ci->queueFamilyIndex});++poolCreates;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo* ci,VkCommandBuffer* cmd){
 assert(pools.count(ci->commandPool));assert(ci->commandBufferCount==1);
 if(fail==2)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *cmd=handle<VkCommandBuffer>();return VK_SUCCESS;
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
VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice,uint32_t n,const VkFence* f){
 assert(n==1 && fences.at(*f));
 if(fail==5)return VK_ERROR_OUT_OF_HOST_MEMORY;
 fences[*f]=false;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice,VkCommandPool pool,const VkAllocationCallbacks*){
 assert(!pools.at(pool).busy);assert(pools.erase(pool)==1);
}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice,VkFence f,const VkAllocationCallbacks*){
 assert(fences.erase(f)==1);
}
void submit(const beamng_ffr::UploadCommands& c){assert(!pools.at(c.pool).busy && !fences.at(c.fence));pools.at(c.pool).busy=true;}
void complete(const beamng_ffr::UploadCommands& c){pools.at(c.pool).busy=false;fences.at(c.fence)=true;}
}
int main(){
 using namespace beamng_ffr;
 const VkDevice device=handle<VkDevice>();
 Api api{};api.createCommandPool=createPool;api.allocateCommandBuffers=allocate;
 api.createFence=createFence;api.resetCommandPool=resetPool;api.resetFences=resetFences;
 api.destroyCommandPool=destroyPool;api.destroyFence=destroyFence;
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
 std::cout<<"Upload ownership, completion gating, family isolation, failures and bounded reuse passed\n";
}
