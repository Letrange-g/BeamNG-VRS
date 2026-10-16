#pragma once
#include "ffr_gpu_image.hpp"
#include <array>
#include <utility>

namespace beamng_ffr {
// Left eye, right eye and (M8.32) the combined pattern for unclassified passes.
constexpr size_t maxRateImages=3;
struct GazeTransferKey {
 std::array<VkBuffer,maxRateImages> sources{};
 std::array<VkImage,maxRateImages> targets{};
 std::array<uint32_t,maxRateImages> widths{},heights{};
 bool operator==(const GazeTransferKey&)const=default;
};
struct UploadCommands {
 VkCommandPool pool=VK_NULL_HANDLE;
 VkCommandBuffer command=VK_NULL_HANDLE;
 VkFence fence=VK_NULL_HANDLE;
 uint32_t family=0;
 GazeTransferKey transfer{};
 bool recorded=false;
};
inline void destroyUploadCommands(const Api& api,VkDevice device,UploadCommands& c){
 if(c.fence)api.destroyFence(device,c.fence,nullptr);
 if(c.pool)api.destroyCommandPool(device,c.pool,nullptr);
 c={};
}
// Caller serializes access. ONLY completed uploads may enter this cache;
// pending submissions remain owned by the layer's existing fence tracker.
// Cache is device-local and queue-family-specific. Exact source/image/extent
// matches retain their recording; changed resources require a pool reset.
class UploadCommandCache {
 std::array<UploadCommands,6> idle_{};
 size_t count_=0;
 uint64_t created_=0,reused_=0,recordingReused_=0;
public:
 UploadCommandCache()=default;
 UploadCommandCache(const UploadCommandCache&)=delete;
 UploadCommandCache& operator=(const UploadCommandCache&)=delete;
 size_t idleCount()const{return count_;}
 uint64_t created()const{return created_;}
 uint64_t reused()const{return reused_;}
 uint64_t recordingReused()const{return recordingReused_;}
 UploadCommands acquire(const Api& api,VkDevice device,uint32_t family,const GazeTransferKey* transfer=nullptr){
  UploadCommands c{};c.family=family;
  try{
   if(api.resetCommandPool && api.resetFences){
    // The source buffers and eye images live for the resource generation.
    // A completed executable command buffer can be resubmitted unchanged:
    // its transfer reads the newly staged bytes at GPU execution time.
    if(transfer)for(size_t i=0;i<count_;++i)
     if(idle_[i].family==family && idle_[i].recorded && idle_[i].transfer==*transfer){
      c=idle_[i];idle_[i]=idle_[--count_];idle_[count_]={};
      check(api.resetFences(device,1,&c.fence),"M8.28 reset recorded upload fence");
      ++reused_;++recordingReused_;return c;
     }
    for(size_t i=0;i<count_;++i)if(idle_[i].family==family &&
       (!transfer || !idle_[i].recorded || count_==idle_.size())){
     c=idle_[i];idle_[i]=idle_[--count_];idle_[count_]={};
     // Fence completion was verified before recycling. Resetting this pool
     // cannot invalidate an executing application or upload command buffer.
     check(api.resetCommandPool(device,c.pool,0),"M8.27 reset upload pool");
     check(api.resetFences(device,1,&c.fence),"M8.27 reset upload fence");
     c.recorded=false;c.transfer=transfer?*transfer:GazeTransferKey{};
     ++reused_;return c;
    }
   }
   VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   pci.queueFamilyIndex=family; // Long-lived cached recordings use default pool flags.
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
template<size_t N>
inline GazeTransferKey gazeTransferKey(const std::array<StagingSlot,N>& sources,
                                     const std::array<GpuImage,N>& targets){
 static_assert(N<=maxRateImages);
 GazeTransferKey key;
 for(size_t eye=0;eye<N;++eye)if(sources[eye].buffer){
  key.sources[eye]=sources[eye].buffer;key.targets[eye]=targets[eye].image;
  key.widths[eye]=targets[eye].width;key.heights[eye]=targets[eye].height;
 }
 return key;
}
template<size_t N>
inline void recordGazeCommands(const Api& api,UploadCommands& c,
                              const std::array<StagingSlot,N>& sources,
                              const std::array<GpuImage,N>& targets){
 const auto key=gazeTransferKey(sources,targets);
 if(c.recorded && c.transfer==key)return;
 if(c.recorded)throw std::invalid_argument("M8.28 recorded upload needs a pool reset");
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 // No ONE_TIME_SUBMIT flag: completed commands can be resubmitted. No
 // SIMULTANEOUS_USE flag: a pending buffer never enters the idle cache.
 check(api.beginCommandBuffer(c.command,&begin),"M8.28 begin reusable upload");
 for(size_t eye=0;eye<N;++eye)if(sources[eye].buffer)
  recordGazeTransfer(api,c.command,sources[eye],targets[eye]);
 check(api.endCommandBuffer(c.command),"M8.28 end reusable upload");
 c.transfer=key;c.recorded=true;
}
}
