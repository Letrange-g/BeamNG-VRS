#include "../src/layer.cpp"
#include <cassert>
#include <iostream>

// Exercise the public Vulkan adapters with a fake driver. In particular, a
// recorded producer/consumer link is not evidence until its queue submission
// succeeds, and shared scene targets must never acquire a left/right eye label.
namespace {
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
uintptr_t nextImage=100,nextView=200,nextSet=300,nextCommand=400,nextFramebuffer=500;
VkResult submitResult=VK_SUCCESS,framebufferResult=VK_SUCCESS;
VkImageView lastRateView{};
std::unordered_map<VkCommandBuffer,VkFramebuffer> activeFramebuffer;
std::unordered_map<VkCommandBuffer,VkPipeline> boundPipeline;
VkRect2D lastScissor{};
unsigned drawCalls=0,submitCalls=0,blackBarCalls=0;

VKAPI_ATTR VkResult VKAPI_CALL image(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage* out) {
 *out=handle<VkImage>(nextImage++);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL view(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* out) {
 *out=handle<VkImageView>(nextView++);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
 for(uint32_t i=0;i<info->commandBufferCount;++i)out[i]=handle<VkCommandBuffer>(nextCommand++);
 return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL descriptors(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out) {
 for(uint32_t i=0;i<info->descriptorSetCount;++i)out[i]=handle<VkDescriptorSet>(nextSet++);
 return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*) {}
VKAPI_ATTR void VKAPI_CALL bindDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*) {}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cmd,VkPipelineBindPoint,VkPipeline pipeline) { boundPipeline[cmd]=pipeline; }
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t) { ++drawCalls; }
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,VkSubpassContents) {
 activeFramebuffer[cmd]=info->framebuffer;
}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL scissor(VkCommandBuffer,uint32_t,uint32_t count,const VkRect2D* rects) {
 assert(count==1);lastScissor=rects[0];
}
VKAPI_ATTR void VKAPI_CALL clear(VkCommandBuffer,uint32_t,const VkClearAttachment*,uint32_t,const VkClearRect*) { ++blackBarCalls; }
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence) { ++submitCalls;return submitResult; }
VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence) { ++submitCalls;return submitResult; }
VKAPI_ATTR VkResult VKAPI_CALL createFramebuffer(VkDevice,const VkFramebufferCreateInfo* info,const VkAllocationCallbacks*,VkFramebuffer* out) {
 assert(info->attachmentCount>0);lastRateView=info->pAttachments[info->attachmentCount-1];
 if(framebufferResult!=VK_SUCCESS)return framebufferResult;
 *out=handle<VkFramebuffer>(nextFramebuffer++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice,VkFramebuffer,const VkAllocationCallbacks*) {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nativeProc(VkDevice,const char*) { return reinterpret_cast<PFN_vkVoidFunction>(draw); }

void exercise(bool separateCommands,unsigned submitApi) {
 const auto device=handle<VkDevice>(1);
 const auto queue=handle<VkQueue>(2);
 // Each case has fresh evidence, while the fake driver gives all new objects
 // unique handles to avoid sharing descriptor/image state between scenarios.
 m837Trackers.erase(device);
 DeviceDispatch dispatch{};dispatch.gdpa=nativeProc;
 dispatch.createImage=image;dispatch.createImageView=view;dispatch.allocateCommandBuffers=allocate;
 dispatch.early_vkBeginCommandBuffer=begin;dispatch.early_vkAllocateDescriptorSets=descriptors;
 dispatch.early_vkUpdateDescriptorSets=update;dispatch.early_vkCmdBindDescriptorSets=bindDescriptors;
 dispatch.early_vkCmdDraw=draw;dispatch.cmdBindPipeline=bindPipeline;
 dispatch.cmdBeginRenderPass=beginPass;dispatch.cmdEndRenderPass=endPass;
 dispatch.cmdSetScissor=scissor;dispatch.cmdClearAttachments=clear;
 dispatch.m812gQueueSubmit=submit;dispatch.m812gQueueSubmit2=submit2;dispatch.m812gQueueSubmit2KHR=submit2;
 dispatch.createFramebuffer=createFramebuffer;dispatch.destroyFramebuffer=destroyFramebuffer;
 devices[device]=dispatch;m812gQueueDevices[queue]=device;
 assert(vkGetDeviceProcAddr(device,"vkCmdDraw")==reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw));
 assert(!vkGetDeviceProcAddr(device,"vkCmdDrawIndirectCountKHR"));

 VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 allocateInfo.commandBufferCount=2;allocateInfo.commandPool=handle<VkCommandPool>(3);
 allocateInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
 VkCommandBuffer commands[2]{};assert(vkAllocateCommandBuffers(device,&allocateInfo,commands)==VK_SUCCESS);
 const auto producer=commands[0],consumer=separateCommands?commands[1]:commands[0];
 VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
 imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.extent={100,80,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;
 imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.format=VK_FORMAT_R16G16B16A16_SFLOAT;
 imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
 VkImage source{},outputs[2]{};assert(vkCreateImage(device,&imageInfo,nullptr,&source)==VK_SUCCESS);
 assert(m837SceneImages.count(source));
 imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
 for(auto& output:outputs) {
  assert(vkCreateImage(device,&imageInfo,nullptr,&output)==VK_SUCCESS);
  assert(!m837SceneImages.count(output));
 }
 VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
 viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 viewInfo.image=source;viewInfo.format=VK_FORMAT_R16G16B16A16_SFLOAT;
 VkImageView sourceView{},outputViews[2]{};assert(vkCreateImageView(device,&viewInfo,nullptr,&sourceView)==VK_SUCCESS);
 viewInfo.format=imageInfo.format;
 for(unsigned eye=0;eye<2;++eye) {
  viewInfo.image=outputs[eye];assert(vkCreateImageView(device,&viewInfo,nullptr,&outputViews[eye])==VK_SUCCESS);
 }

 const auto pass=handle<VkRenderPass>(10),shadow=handle<VkRenderPass>(11);
 const auto sourceFb=handle<VkFramebuffer>(12),commonFb=handle<VkFramebuffer>(14);
 const VkFramebuffer outputFbs[2]={handle<VkFramebuffer>(13),handle<VkFramebuffer>(18)};
 m833ColorSlots[pass]=0;m837ScenePasses.insert(pass);shadowPasses[pass]={device,shadow};
 passInfos[pass]={2,1,1,1,true,true};
 framebuffers[sourceFb]={100,80,1,2};
 const auto depthView=handle<VkImageView>(15);
 m74dAttachments[sourceFb]={{sourceView,depthView},pass};
 for(unsigned eye=0;eye<2;++eye) {
  framebuffers[outputFbs[eye]]={100,80,1,2};m74dAttachments[outputFbs[eye]]={{outputViews[eye],depthView},pass};
 }
 shadowFramebuffers[sourceFb]={device,VK_NULL_HANDLE};
 m83EyeFramebuffers[sourceFb]={device,{handle<VkFramebuffer>(16),handle<VkFramebuffer>(17),commonFb}};
 // Only the two final outputs have actual OpenXR eye identities.
 m75Sources[outputs[0]]=0;m75Sources[outputs[1]]=1;
 const auto pipeline=handle<VkPipeline>(20),replacement=handle<VkPipeline>(21);
 m832ShadowSignatures[shadow]={{1}};m89Variants[pipeline]={device,replacement,shadow,VK_NULL_HANDLE,{{1}}};

 VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
 setInfo.descriptorSetCount=1;setInfo.descriptorPool=handle<VkDescriptorPool>(22);VkDescriptorSet set{};
 assert(vkAllocateDescriptorSets(device,&setInfo,&set)==VK_SUCCESS);
 VkDescriptorImageInfo sample{};sample.imageView=sourceView;sample.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;
 write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&sample;
 vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 VkCommandBufferBeginInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 VkRenderPassBeginInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
 passInfo.renderPass=pass;passInfo.renderArea={{0,0},{100,80}};

 auto record=[&](bool selected,unsigned eye=0) {
  assert(vkBeginCommandBuffer(producer,&commandInfo)==VK_SUCCESS);
  if(separateCommands)assert(vkBeginCommandBuffer(consumer,&commandInfo)==VK_SUCCESS);
  passInfo.framebuffer=sourceFb;vkCmdBeginRenderPass(producer,&passInfo,VK_SUBPASS_CONTENTS_INLINE);
  assert(activeFramebuffer[producer]==(selected?commonFb:sourceFb));
  assert(bool(m837ActiveShared.count(producer))==selected);
  assert(!m818SelectedEye.count(producer));assert(m8ClassifiedEye(source)==-1);
  assert(!m75Sources.count(source));
  vkCmdBindPipeline(producer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
  assert(boundPipeline[producer]==(selected?replacement:pipeline));
  const auto barsBefore=blackBarCalls;
  // The existing unknown-eye path applies only the common top/bottom crop.
  // Learning the shared target must not introduce either eye's outer crop.
  assert(cropStates.count(producer) && cropStates.at(producer).eye==-1);
  const VkRect2D full{{0,0},{100,80}};vkCmdSetScissor(producer,0,1,&full);
  assert(lastScissor.offset.x==0 && lastScissor.offset.y==24 && lastScissor.extent.width==100 && lastScissor.extent.height==32);
  for(unsigned i=0;i<128;++i)vkCmdDraw(producer,3,1,0,0);
  vkCmdEndRenderPass(producer);
  assert(blackBarCalls==barsBefore+1);
  assert(!m837ActiveShared.count(producer));
  passInfo.framebuffer=outputFbs[eye];vkCmdBeginRenderPass(consumer,&passInfo,VK_SUBPASS_CONTENTS_INLINE);
  assert(cropStates.count(consumer) && cropStates.at(consumer).eye==int(eye));
  vkCmdBindDescriptorSets(consumer,VK_PIPELINE_BIND_POINT_GRAPHICS,handle<VkPipelineLayout>(23),0,1,&set,0,nullptr);
  vkCmdDraw(consumer,3,1,0,0);vkCmdEndRenderPass(consumer);
 };
 auto send=[&] {
  const uint32_t count=separateCommands?2:1;
  if(submitApi==0) {
   VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=count;info.pCommandBuffers=commands;
   return vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE);
  }
  VkCommandBufferSubmitInfo buffers[2]={{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
  for(uint32_t i=0;i<count;++i)buffers[i].commandBuffer=commands[i];
  VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=count;info.pCommandBufferInfos=buffers;
  return submitApi==1?vkQueueSubmit2(queue,1,&info,VK_NULL_HANDLE):vkQueueSubmit2KHR(queue,1,&info,VK_NULL_HANDLE);
 };
 // Merely recording (or failing submission) must not train a target.
 for(unsigned i=0;i<4;++i)record(false);
 submitResult=VK_ERROR_DEVICE_LOST;assert(send()==VK_ERROR_DEVICE_LOST);record(false);
 submitResult=VK_SUCCESS;
 for(unsigned i=0;i<3;++i) {record(false,i%2);assert(send()==VK_SUCCESS);}
 record(true);

 // The common map is separate from both gaze-following maps. Lazy creation
 // must fail closed, then append the uploaded fixed map when creation works.
 m83EyeFramebuffers[sourceFb].eyes[2]=VK_NULL_HANDLE;
 m823LazyPlans.erase(sourceFb);
 record(false); // A learned target without a usable framebuffer stays original.
 m823LazyPlans[sourceFb]={device,shadow,0,100,80,1,{sourceView,depthView},false,{}};
 const auto fixedRate=handle<VkImageView>(30);
 m83EyePatterns[device][0].view=handle<VkImageView>(31);m83EyePatterns[device][1].view=handle<VkImageView>(32);
 uploadedPatterns[device].view=VK_NULL_HANDLE;
 assert(!m823EnsureEyeFramebuffer(device,sourceFb,2));
 uploadedPatterns[device].view=fixedRate;
 framebufferResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
 assert(!m823EnsureEyeFramebuffer(device,sourceFb,2));assert(!m83EyeFramebuffers[sourceFb].eyes[2]);
 framebufferResult=VK_SUCCESS;
 assert(m823EnsureEyeFramebuffer(device,sourceFb,2));assert(lastRateView==fixedRate);
 assert(m83EyeFramebuffers[sourceFb].eyes[2]);
 assert(!m823EnsureEyeFramebuffer(device,sourceFb,3));
}
}

int main() {
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\nonexistent-shared-scene-test.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_SHARED_SCENE","1");
 SetEnvironmentVariableA("BEAMNG_VRS_EARLY_PASSES","off");
 SetEnvironmentVariableA("BEAMNG_VRS_PROFILE_GPU","0");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M89_STATIC_TEST","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M84_CPU_GAZE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_M812H_FENCE_TEST","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE","1");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_TOP","0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_BOTTOM","0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_DEBUG_OUTLINE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");
 m814ActiveWidth=100;m814ActiveHeight=80;
 assert(m837Enabled() && !m833Enabled() && !m835Enabled());
 assert(m817MessageLevel("M8.37 SHARED_SCENE selected=3 binds=128")==1);
 exercise(false,0);exercise(true,0);exercise(true,1);exercise(true,2);
 assert(drawCalls>0 && submitCalls==16);
 std::cout<<"Shared scene real-hook submitted learning, fixed VRS map, crop isolation and queue aliases passed\n";
}
