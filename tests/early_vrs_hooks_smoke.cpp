#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
namespace {
uint64_t nextImage=100,nextView=200,nextSet=300;
unsigned draws=0,writes=0,begins=0;
VkPipeline bound{};
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline p){bound=p;}
VKAPI_ATTR VkResult VKAPI_CALL image(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage* out){*out=reinterpret_cast<VkImage>(nextImage++);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL view(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* out){*out=reinterpret_cast<VkImageView>(nextView++);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer* out){*out=reinterpret_cast<VkCommandBuffer>(uintptr_t(2));return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL descriptors(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet* out){*out=reinterpret_cast<VkDescriptorSet>(nextSet++);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL update(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*){++writes;}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t){++draws;}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){++begins;}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer){}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags){return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativeProc(VkDevice,const char*){return reinterpret_cast<PFN_vkVoidFunction>(draw);}
}
int main(int argc,char** argv){
 assert(argc==2);const std::string mode=argv[1];const bool enabled=mode!="off";
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\nonexistent-early-vrs-test.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_EARLY_PASSES",mode.c_str());
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M89_STATIC_TEST","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M84_CPU_GAZE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_DEBUG_OUTLINE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");
 m814ActiveWidth=100;m814ActiveHeight=80;
 const auto device=reinterpret_cast<VkDevice>(uintptr_t(1));
 DeviceDispatch d{};d.gdpa=nativeProc;d.createImage=image;d.createImageView=view;d.allocateCommandBuffers=allocate;
 d.early_vkBeginCommandBuffer=begin;d.early_vkAllocateDescriptorSets=descriptors;d.early_vkUpdateDescriptorSets=update;
 d.early_vkCmdBindDescriptorSets=bind;d.early_vkCmdDraw=draw;d.cmdBeginRenderPass=beginPass;d.cmdEndRenderPass=endPass;
 d.early_vkResetDescriptorPool=resetPool;d.early_vkDestroyImage=destroyImage;
 d.cmdBindPipeline=bindPipeline;
 devices[device]=d;
 assert(vkGetDeviceProcAddr(device,"vkCmdDraw")==reinterpret_cast<PFN_vkVoidFunction>(enabled?vkCmdDraw:draw));
 if(enabled)assert(!vkGetDeviceProcAddr(device,"vkCmdDrawIndirectCountKHR"));
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandBufferCount=1;ai.commandPool=reinterpret_cast<VkCommandPool>(uintptr_t(4));ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
 VkCommandBuffer cmd{};assert(vkAllocateCommandBuffers(device,&ai,&cmd)==VK_SUCCESS);
 VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.extent={100,80,1};
 ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
 ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
 VkImage source{},output{};assert(vkCreateImage(device,&ii,nullptr,&source)==VK_SUCCESS);assert(vkCreateImage(device,&ii,nullptr,&output)==VK_SUCCESS);
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
 vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vi.image=source;
 VkImageView sv{},ov{};assert(vkCreateImageView(device,&vi,nullptr,&sv)==VK_SUCCESS);
 vi.image=output;assert(vkCreateImageView(device,&vi,nullptr,&ov)==VK_SUCCESS);
 const auto pass=reinterpret_cast<VkRenderPass>(uintptr_t(10)),shadow=reinterpret_cast<VkRenderPass>(uintptr_t(11));
 const auto sf=reinterpret_cast<VkFramebuffer>(uintptr_t(12)),of=reinterpret_cast<VkFramebuffer>(uintptr_t(13));
 m833ColorSlots[pass]=0;shadowPasses[pass]={device,shadow};passInfos[pass]={1,1,1,1,false,true};
 framebuffers[sf]=framebuffers[of]={100,80,1,1};m74dAttachments[sf]={{sv},pass};m74dAttachments[of]={{ov},pass};
 m75Sources[output]=0; // Direct eye discovery normally comes from OpenXR copies.
 shadowFramebuffers[sf]={device,sf};m83EyeFramebuffers[sf]={device,{sf,sf}};
 VkDescriptorSetAllocateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};di.descriptorSetCount=1;
 di.descriptorPool=reinterpret_cast<VkDescriptorPool>(uintptr_t(14));VkDescriptorSet set{};
 assert(vkAllocateDescriptorSets(device,&di,&set)==VK_SUCCESS);
 VkDescriptorImageInfo imageInfo{};imageInfo.imageView=sv;imageInfo.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;
 write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&imageInfo;
 vkUpdateDescriptorSets(device,1,&write,0,nullptr);assert(writes==1);
 VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.renderArea={{0,0},{100,80}};
 VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 auto record=[&]{
  assert(vkBeginCommandBuffer(cmd,&cb)==VK_SUCCESS);
  rp.framebuffer=sf;vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
  rp.framebuffer=of;vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,reinterpret_cast<VkPipelineLayout>(uintptr_t(15)),0,1,&set,0,nullptr);
  vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
 };
 for(int i=0;i<3;++i)record();assert(draws==6 && begins==6);
 rp.framebuffer=sf;VkRenderPassBeginInfo modified{};
 assert(m57SelectShadow(cmd,&rp,modified)==(mode=="on"));
 const auto pipeline=reinterpret_cast<VkPipeline>(uintptr_t(40)),replacement=reinterpret_cast<VkPipeline>(uintptr_t(41));
 m832ShadowSignatures[shadow]={{1}};m89Variants[pipeline]={device,replacement,shadow,VK_NULL_HANDLE,{{1}}};
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
 assert(bound==(mode=="on"?replacement:pipeline));
 if(enabled)assert(m833Trackers.at(device).stats.binds==(mode=="on"?1u:0u));
 // Learned targets never acquire original eye identity, keeping crop/outline scope unchanged.
 assert(m8ClassifiedEye(source)==-1);
 if(enabled){
  auto& t=m833Trackers.at(device);assert(t.eye(m833Id(source),GetTickCount64())==0);
  // Array writes are conservatively excluded instead of guessing binding spill.
  VkDescriptorImageInfo two[2]{imageInfo,imageInfo};write.pImageInfo=two;write.descriptorCount=2;
  vkUpdateDescriptorSets(device,1,&write,0,nullptr);assert(t.sets.at(m833Id(set)).unknown);
  record();assert(t.eye(m833Id(source),GetTickCount64())==-1);
  assert(vkResetDescriptorPool(device,di.descriptorPool,0)==VK_SUCCESS && t.sets.empty());
  vkDestroyImage(device,source,nullptr);assert(!t.images.count(m833Id(source)));
  m833ColorViews.erase(sv);m74gViews.erase(sv);
 }else assert(m833Trackers.empty());
 std::cout<<"Early VRS real-hook "<<mode<<" inference, selection and forwarding passed\n";
}
