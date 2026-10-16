#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
namespace {
unsigned created=0,read=0,destroyed=0,draws=0,dispatches=0,submitted=0,ends=0;
unsigned descriptorUpdates=0,descriptorBinds=0;bool failDescriptors=false;
uintptr_t handleSequence=1000;uint64_t ticks=0;
std::map<VkQueryPool,std::array<uint64_t,256>> values;
template<class T>T h(uintptr_t value){return reinterpret_cast<T>(value);}
VKAPI_ATTR VkResult VKAPI_CALL queryCreate(VkDevice,const VkQueryPoolCreateInfo*,const VkAllocationCallbacks*,VkQueryPool* p){*p=h<VkQueryPool>(handleSequence++);values[*p]={};++created;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL queryDestroy(VkDevice,VkQueryPool p,const VkAllocationCallbacks*){assert(values.erase(p)==1);++destroyed;}
VKAPI_ATTR void VKAPI_CALL queryReset(VkCommandBuffer,VkQueryPool,uint32_t,uint32_t){}
VKAPI_ATTR void VKAPI_CALL timestamp(VkCommandBuffer,VkPipelineStageFlagBits,VkQueryPool p,uint32_t i){values.at(p)[i]=ticks;ticks+=1000000;}
VKAPI_ATTR VkResult VKAPI_CALL results(VkDevice,VkQueryPool p,uint32_t,uint32_t count,size_t,void* data,VkDeviceSize,VkQueryResultFlags flags){
 assert((flags&VK_QUERY_RESULT_WAIT_BIT)==0);++read;auto out=static_cast<std::array<uint64_t,2>*>(data);
 for(uint32_t i=0;i<count;++i)out[i]={values.at(p)[i],1};return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* p){*p=h<VkCommandPool>(3);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*){}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer* c){*c=h<VkCommandBuffer>(4);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL createPass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass* p){*p=h<VkRenderPass>(5);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer){++ends;}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer,VkPipelineBindPoint,VkPipeline){}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t){++draws;}
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer,uint32_t,uint32_t,uint32_t){++dispatches;}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence){++submitted;return VK_SUCCESS;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL proc(VkDevice,const char*){return nullptr;}
VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet* out){if(failDescriptors)return VK_ERROR_OUT_OF_POOL_MEMORY;*out=h<VkDescriptorSet>(20);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL resetDescriptors(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags){return failDescriptors?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL updateSets(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*){++descriptorUpdates;}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){++descriptorBinds;}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage* out){*out=h<VkImage>(21);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* out){*out=h<VkImageView>(22);return VK_SUCCESS;}
}
int main(int argc,char**){
 const bool enabled=argc==1;
 assert(m817MessageLevel("M8.36 GPU_LINK")==1);
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\absent-profile-test.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_PROFILE_GPU",enabled?"1":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_EARLY_PASSES","off");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","0");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");
 const auto device=h<VkDevice>(1);const auto queue=h<VkQueue>(2);
 DeviceDispatch d{};d.gdpa=proc;d.early_vkCreateCommandPool=createPool;d.early_vkDestroyCommandPool=destroyPool;
 d.allocateCommandBuffers=allocate;d.early_vkBeginCommandBuffer=begin;d.createRenderPass=createPass;
 d.cmdBeginRenderPass=beginPass;d.cmdEndRenderPass=endPass;d.cmdBindPipeline=bind;
 d.early_vkCmdDraw=draw;d.early_vkCmdDispatch=dispatch;d.m812gQueueSubmit=submit;devices[device]=d;m812gQueueDevices[queue]=device;
 d.early_vkAllocateDescriptorSets=allocateSets;d.early_vkUpdateDescriptorSets=updateSets;d.early_vkCmdBindDescriptorSets=bindSets;d.early_vkResetDescriptorPool=resetDescriptors;
 d.createImage=createImage;d.createImageView=createView;devices[device]=d;
 if(!enabled){
  assert(!vkGetDeviceProcAddr(device,"vkAllocateDescriptorSets") && !vkGetDeviceProcAddr(device,"vkCmdDraw"));
  assert(!m835Enabled() && m833Trackers.empty());
  std::cout<<"Disabled profiling leaves additional diagnostic hooks inactive\n";return 0;
 }
 assert(vkGetDeviceProcAddr(device,"vkCmdDraw")==reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw));
 assert(vkGetDeviceProcAddr(device,"vkAllocateDescriptorSets")==reinterpret_cast<PFN_vkVoidFunction>(vkAllocateDescriptorSets));
 VkQueueFamilyProperties family{};family.timestampValidBits=64;family.queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
 m835Profiler.device(device,{queryCreate,queryDestroy,results,queryReset,timestamp},1,{family});
 VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageInfo.extent={100,80,1};imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;VkImage image{};
 assert(vkCreateImage(device,&imageInfo,nullptr,&image)==VK_SUCCESS);
 VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};viewInfo.image=image;VkImageView view{};assert(vkCreateImageView(device,&viewInfo,nullptr,&view)==VK_SUCCESS);
 VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};setInfo.descriptorPool=h<VkDescriptorPool>(23);setInfo.descriptorSetCount=1;VkDescriptorSet set{};
 assert(vkAllocateDescriptorSets(device,&setInfo,&set)==VK_SUCCESS);
 VkDescriptorImageInfo descriptor{};descriptor.imageView=view;VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.descriptorCount=1;write.pImageInfo=&descriptor;
 vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 auto observed=[&](){beamng_trace::Uses use;beamng_trace::BoundSets bound;bound.bind(h<VkPipelineLayout>(24),0,1,&set);m835Profiler.resources([&](auto& t){t.collect(device,bound,use);});return use;};
 assert(observed().reads.size()==1);failDescriptors=true;
 assert(vkResetDescriptorPool(device,setInfo.descriptorPool,0)!=VK_SUCCESS);assert(observed().reads.size()==1);failDescriptors=false;
 VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};VkCommandPool pool{};assert(vkCreateCommandPool(device,&pi,nullptr,&pool)==VK_SUCCESS);
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
 VkCommandBuffer cmd{};assert(vkAllocateCommandBuffers(device,&ai,&cmd)==VK_SUCCESS);
 VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R8G8B8A8_UNORM;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
 VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;
 VkRenderPass pass{};assert(vkCreateRenderPass(device,&rp,nullptr,&pass)==VK_SUCCESS);
 VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};info.renderPass=pass;info.renderArea.extent={100,80};
 VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
 for(unsigned i=0;i<200;++i){
  assert(vkBeginCommandBuffer(cmd,&cb)==VK_SUCCESS);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipelineLayout>(24),0,1,&set,0,nullptr);
  vkCmdBeginRenderPass(cmd,&info,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(6));vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,h<VkPipeline>(7));vkCmdDispatch(cmd,1,1,1);
  assert(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)==VK_SUCCESS);
 }
 vkDestroyCommandPool(device,pool,nullptr);
 assert(created>0 && read>0 && created==destroyed && values.empty());
 assert(draws==200 && dispatches==200 && submitted==200 && ends==200);
 assert(descriptorBinds==200 && descriptorUpdates==1);
 assert(vkResetDescriptorPool(device,setInfo.descriptorPool,0)==VK_SUCCESS);assert(observed().partial && observed().reads.empty());
 assert(m833Trackers.empty() && !commandDevices.count(cmd));
 std::cout<<"GPU profiling actual hook dispatch, timestamps and pool cleanup passed\n";
}
