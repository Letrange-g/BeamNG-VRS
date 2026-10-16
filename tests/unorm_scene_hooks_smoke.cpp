#include "../src/layer.cpp"
#include <cassert>
#include <iostream>

// Real layer entrypoints against a fake Vulkan driver. The UNORM experiment
// must retain the application's render pass/framebuffer, making every skipped
// or failed pipeline replacement a legal original-pipeline fallback.
namespace {
template<class T> T handle(uintptr_t id){return reinterpret_cast<T>(id);}
uintptr_t nextHandle=100;
VkResult submitResult=VK_SUCCESS;
bool failUniform=false;
unsigned draws=0,submits=0,uniformCreates=0;
struct PipelineDescription { VkRenderPass pass{}; uint32_t rate=1; VkRect2D scissor{}; };
std::unordered_map<VkPipeline,PipelineDescription> pipelineDescriptions;
std::unordered_map<VkRenderPass,VkFormat> passFormats;
std::unordered_map<VkCommandBuffer,VkRenderPass> activePass;
std::unordered_map<VkCommandBuffer,VkFramebuffer> activeFramebuffer;
std::unordered_map<VkCommandBuffer,VkPipeline> bound;
std::unordered_set<VkPipeline> destroyedPipelines;

VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage* out){
 *out=handle<VkImage>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* out){
 *out=handle<VkImageView>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createPass(VkDevice,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks*,VkRenderPass* out){
 assert(info->attachmentCount==2 && info->subpassCount==1);
 *out=handle<VkRenderPass>(nextHandle++);passFormats[*out]=info->pAttachments[0].format;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createShadow(VkDevice,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks*,VkRenderPass* out){
 assert(info->attachmentCount==3 && info->pSubpasses[0].pNext);
 *out=handle<VkRenderPass>(nextHandle++);passFormats[*out]=info->pAttachments[0].format;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice,VkRenderPass,const VkAllocationCallbacks*){}
VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo* info,const VkAllocationCallbacks*,VkPipeline* out){
 assert(count==1);
 const auto* rate=findChain(info->pNext,VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR);
 const auto* fsr=reinterpret_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(rate);
 const bool uniform=fsr && fsr->fragmentSize.width==4 && fsr->fragmentSize.height==4;
 if(uniform){
  ++uniformCreates;
  assert(passFormats.at(info->renderPass)==VK_FORMAT_R16G16B16A16_UNORM);
  assert(m838UnormPasses.count(info->renderPass)); // Original pass, never its shadow.
  assert(fsr->combinerOps[0]==VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR);
  assert(fsr->combinerOps[1]==VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR);
  assert(info->pViewportState && info->pViewportState->pScissors);
  const auto& s=info->pViewportState->pScissors[0];
  assert(s.offset.x==0 && s.offset.y==24 && s.extent.width==100 && s.extent.height==32);
  if(failUniform){*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
 }
 *out=handle<VkPipeline>(nextHandle++);
 pipelineDescriptions[*out]={info->renderPass,uniform?4u:1u,info->pViewportState->pScissors[0]};
 return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice,VkPipeline pipeline,const VkAllocationCallbacks*){destroyedPipelines.insert(pipeline);}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){
 for(uint32_t i=0;i<info->commandBufferCount;++i)out[i]=handle<VkCommandBuffer>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer,VkCommandBufferResetFlags){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL allocateDescriptors(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out){
 for(uint32_t i=0;i<info->descriptorSetCount;++i)out[i]=handle<VkDescriptorSet>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*){}
VKAPI_ATTR void VKAPI_CALL bindDescriptors(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer cmd,VkPipelineBindPoint,VkPipeline pipeline){bound[cmd]=pipeline;}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,VkSubpassContents){
 activePass[cmd]=info->renderPass;activeFramebuffer[cmd]=info->framebuffer;
}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer){}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t){++draws;}
VKAPI_ATTR void VKAPI_CALL clear(VkCommandBuffer,uint32_t,const VkClearAttachment*,uint32_t,const VkClearRect*){}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence){++submits;return submitResult;}

void exercise(bool baseline){
 const auto device=handle<VkDevice>(1);
 const auto queue=handle<VkQueue>(2);
 DeviceDispatch d{};d.vrsEnabled=true;d.unorm4x4Supported=true;
 d.createImage=createImage;d.createImageView=createView;d.early_vkDestroyImage=destroyImage;
 d.createRenderPass=createPass;d.createRenderPass2=createShadow;d.destroyRenderPass=destroyPass;
 d.createPipelines=createPipelines;d.destroyPipeline=destroyPipeline;
 d.allocateCommandBuffers=allocate;d.early_vkBeginCommandBuffer=begin;d.early_vkResetCommandBuffer=reset;
 d.early_vkAllocateDescriptorSets=allocateDescriptors;d.early_vkUpdateDescriptorSets=update;
 d.early_vkCmdBindDescriptorSets=bindDescriptors;d.early_vkCmdDraw=draw;
 d.cmdBeginRenderPass=beginPass;d.cmdEndRenderPass=endPass;d.cmdBindPipeline=bindPipeline;
 d.cmdClearAttachments=clear;d.m812gQueueSubmit=submit;
 devices[device]=d;m812gQueueDevices[queue]=device;
 VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 allocation.commandPool=handle<VkCommandPool>(3);allocation.commandBufferCount=3;
 allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;VkCommandBuffer cmds[3]{};
 assert(vkAllocateCommandBuffers(device,&allocation,cmds)==VK_SUCCESS);

 auto makeImage=[&](VkFormat format,bool depth=false){
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;
  info.format=format;info.extent={100,80,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
  info.usage=depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImage image{};assert(vkCreateImage(device,&info,nullptr,&image)==VK_SUCCESS);return image;
 };
 const auto depth=makeImage(VK_FORMAT_D24_UNORM_S8_UINT,true);
 // Final eye composition uses a different depth image in the captured game.
 // Reusing the scene depth here would correctly invalidate the preceding
 // UNORM/SFLOAT association when that unrelated eye pass writes depth.
 const auto eyeDepth=makeImage(VK_FORMAT_D24_UNORM_S8_UINT,true);
 const auto unorm=makeImage(VK_FORMAT_R16G16B16A16_UNORM),scene=makeImage(VK_FORMAT_R16G16B16A16_SFLOAT);
 const VkImage outputs[]={makeImage(VK_FORMAT_R8G8B8A8_UNORM),makeImage(VK_FORMAT_R8G8B8A8_UNORM)};
 assert(m838UnormImages.count(unorm) && !m837SceneImages.count(unorm));
 assert(m837SceneImages.count(scene) && !m838UnormImages.count(scene));
 auto makeView=[&](VkImage image,VkFormat format,bool depth=false){
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=image;info.viewType=VK_IMAGE_VIEW_TYPE_2D;
  const VkImageAspectFlags aspects=depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
  info.format=format;info.subresourceRange={aspects,0,1,0,1};
  VkImageView view{};assert(vkCreateImageView(device,&info,nullptr,&view)==VK_SUCCESS);return view;
 };
 const auto depthView=makeView(depth,VK_FORMAT_D24_UNORM_S8_UINT,true);
 const auto eyeDepthView=makeView(eyeDepth,VK_FORMAT_D24_UNORM_S8_UINT,true);
 const auto unormView=makeView(unorm,VK_FORMAT_R16G16B16A16_UNORM),sceneView=makeView(scene,VK_FORMAT_R16G16B16A16_SFLOAT);
 const VkImageView outputViews[]={makeView(outputs[0],VK_FORMAT_R8G8B8A8_UNORM),makeView(outputs[1],VK_FORMAT_R8G8B8A8_UNORM)};
 auto makePass=[&](VkFormat format){
  VkAttachmentDescription attachments[2]{};
  attachments[0].format=format;attachments[1].format=VK_FORMAT_D24_UNORM_S8_UINT;
  for(auto& a:attachments){a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
  attachments[0].initialLayout=attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[1].initialLayout=attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthRef{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;
  sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depthRef;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=2;info.pAttachments=attachments;
  info.subpassCount=1;info.pSubpasses=&sub;VkRenderPass pass{};
  assert(vkCreateRenderPass(device,&info,nullptr,&pass)==VK_SUCCESS);return pass;
 };
 const auto unormPass=makePass(VK_FORMAT_R16G16B16A16_UNORM),scenePass=makePass(VK_FORMAT_R16G16B16A16_SFLOAT),eyePass=makePass(VK_FORMAT_R8G8B8A8_UNORM);
 assert(m838UnormPasses.count(unormPass) && !m838UnormPasses.count(scenePass) && !m838UnormPasses.count(eyePass));
 assert(m838DepthSlots.at(unormPass)==1 && m838DepthSlots.at(scenePass)==1);
 auto makeFramebuffer=[&](VkRenderPass pass,VkImageView color,VkImageView depthAttachment){
  const auto fb=handle<VkFramebuffer>(nextHandle++);framebuffers[fb]={100,80,1,2};m74dAttachments[fb]={{color,depthAttachment},pass};
  // Even when all shadow alternatives exist, the UNORM stage must stay on the
  // original pass and framebuffer. SharedScene=false also keeps SFLOAT original.
  shadowFramebuffers[fb]={device,handle<VkFramebuffer>(nextHandle++)};
  m83EyeFramebuffers[fb]={device,{handle<VkFramebuffer>(nextHandle++),handle<VkFramebuffer>(nextHandle++),handle<VkFramebuffer>(nextHandle++)}};
  return fb;
 };
 const auto unormFb=makeFramebuffer(unormPass,unormView,depthView),sceneFb=makeFramebuffer(scenePass,sceneView,depthView);
 const VkFramebuffer eyeFbs[]={makeFramebuffer(eyePass,outputViews[0],eyeDepthView),makeFramebuffer(eyePass,outputViews[1],eyeDepthView)};
 m75Sources[outputs[0]]=0;m75Sources[outputs[1]]=1;

 VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
 for(auto& stage:stages){stage.module=handle<VkShaderModule>(nextHandle++);stage.pName="main";}
 VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport{0,0,100,80,0,1};VkRect2D scissors{{0,0},{100,80}};
 VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
 viewports.viewportCount=viewports.scissorCount=1;viewports.pViewports=&viewport;viewports.pScissors=&scissors;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.lineWidth=1;raster.polygonMode=VK_POLYGON_MODE_FILL;
 VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depthState.depthTestEnable=VK_TRUE;depthState.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
 VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=0xf;
 VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blendState.attachmentCount=1;blendState.pAttachments=&blend;
 VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
 pipelineInfo.stageCount=2;pipelineInfo.pStages=stages;pipelineInfo.pVertexInputState=&vertex;pipelineInfo.pInputAssemblyState=&assembly;
 pipelineInfo.pViewportState=&viewports;pipelineInfo.pRasterizationState=&raster;pipelineInfo.pMultisampleState=&multisample;
 pipelineInfo.pDepthStencilState=&depthState;pipelineInfo.pColorBlendState=&blendState;pipelineInfo.layout=handle<VkPipelineLayout>(nextHandle++);
 auto makePipeline=[&](VkRenderPass pass){pipelineInfo.renderPass=pass;VkPipeline pipeline{};
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline)==VK_SUCCESS);return pipeline;};
 const auto pipeline=makePipeline(unormPass),scenePipeline=makePipeline(scenePass),eyePipeline=makePipeline(eyePass);
 assert(m838Variants.count(pipeline) && !m838Variants.count(scenePipeline) && !m838Variants.count(eyePipeline));
 const auto coarse=m838Variants.at(pipeline).pipeline;
 assert(pipelineDescriptions.at(coarse).pass==unormPass && pipelineDescriptions.at(coarse).rate==4);
 failUniform=true;const auto failed=makePipeline(unormPass);failUniform=false;assert(!m838Variants.count(failed));
 devices[device].unorm4x4Supported=false;const auto unsupported=makePipeline(unormPass);devices[device].unorm4x4Supported=true;
 assert(!m838Variants.count(unsupported));

 VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};setInfo.descriptorPool=handle<VkDescriptorPool>(nextHandle++);
 setInfo.descriptorSetCount=1;VkDescriptorSet set{};assert(vkAllocateDescriptorSets(device,&setInfo,&set)==VK_SUCCESS);
 VkDescriptorImageInfo sampled{};sampled.imageView=sceneView;sampled.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
 write.descriptorCount=1;write.pImageInfo=&sampled;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
 VkCommandBufferBeginInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 auto start=[&](VkCommandBuffer cmd,VkRenderPass pass,VkFramebuffer fb){
  VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};info.renderPass=pass;info.framebuffer=fb;info.renderArea={{0,0},{100,80}};
  vkCmdBeginRenderPass(cmd,&info,VK_SUBPASS_CONTENTS_INLINE);
  if(pass==unormPass || pass==scenePass){assert(activePass[cmd]==pass && activeFramebuffer[cmd]==fb);assert(!m89ActiveShadows.count(cmd));}
 };
 auto record=[&](bool selected,unsigned eye=0){
  for(const auto cmd:cmds)assert(vkBeginCommandBuffer(cmd,&commandInfo)==VK_SUCCESS);
  start(cmds[0],unormPass,unormFb);
  vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);assert(bound[cmds[0]]==(selected?coarse:pipeline));
  assert(!m818SelectedEye.count(cmds[0]) && !m75Sources.count(unorm) && m8ClassifiedEye(unorm)==-1);
  assert(cropStates.at(cmds[0]).eye==-1);
  for(unsigned i=0;i<128;++i)vkCmdDraw(cmds[0],3,1,0,0);
  // This pipeline participates in the learned heavy stage, but its clone
  // failed. It must still remain original after the stage is fully trained.
  vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,failed);assert(bound[cmds[0]]==failed);
  vkCmdDraw(cmds[0],3,1,0,0);vkCmdEndRenderPass(cmds[0]);
  start(cmds[1],scenePass,sceneFb);vkCmdBindPipeline(cmds[1],VK_PIPELINE_BIND_POINT_GRAPHICS,scenePipeline);
  assert(bound[cmds[1]]==scenePipeline);
  for(unsigned i=0;i<128;++i)vkCmdDraw(cmds[1],3,1,0,0);vkCmdEndRenderPass(cmds[1]);
  start(cmds[2],eyePass,eyeFbs[eye]);vkCmdBindPipeline(cmds[2],VK_PIPELINE_BIND_POINT_GRAPHICS,eyePipeline);
  vkCmdBindDescriptorSets(cmds[2],VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineInfo.layout,0,1,&set,0,nullptr);
  vkCmdDraw(cmds[2],3,1,0,0);vkCmdEndRenderPass(cmds[2]);
 };
 auto send=[&]{VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=3;info.pCommandBuffers=cmds;return vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE);};
 for(unsigned i=0;i<4;++i)record(false,i%2);
 submitResult=VK_ERROR_DEVICE_LOST;assert(send()==VK_ERROR_DEVICE_LOST);submitResult=VK_SUCCESS;
 for(unsigned i=0;i<3;++i){record(false,i%2);assert(send()==VK_SUCCESS);}
 assert(m837Trackers.at(device).readyUnorm(m833Id(unorm),m833Id(unormPass),m833Id(depth),GetTickCount64()));
 record(!baseline);

 // An unlearned first pipeline, or any missing compatible variant, must use
 // the original pipeline and never change the original rendering scope.
 assert(vkBeginCommandBuffer(cmds[0],&commandInfo)==VK_SUCCESS);start(cmds[0],unormPass,unormFb);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,failed);assert(bound[cmds[0]]==failed);
 vkCmdDraw(cmds[0],3,1,0,0);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);assert(bound[cmds[0]]==pipeline);
 vkCmdEndRenderPass(cmds[0]);
 assert(vkBeginCommandBuffer(cmds[0],&commandInfo)==VK_SUCCESS);start(cmds[0],unormPass,unormFb);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(cmds[0],3,1,0,0);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,unsupported);assert(bound[cmds[0]]==unsupported);
 vkCmdEndRenderPass(cmds[0]);
 // Re-record/reset clears selection. A pipeline bound outside a render pass
 // stays original; starting another pass cannot carry a previous 4x4 binding.
 assert(vkResetCommandBuffer(cmds[0],0)==VK_SUCCESS);
 assert(vkBeginCommandBuffer(cmds[0],&commandInfo)==VK_SUCCESS);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);assert(bound[cmds[0]]==pipeline);
 start(cmds[0],unormPass,unormFb);assert(bound[cmds[0]]==pipeline);vkCmdEndRenderPass(cmds[0]);
 assert(vkBeginCommandBuffer(cmds[0],&commandInfo)==VK_SUCCESS);start(cmds[0],unormPass,unormFb);
 vkCmdBindPipeline(cmds[0],VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);assert(bound[cmds[0]]==(baseline?pipeline:coarse));
 vkCmdDraw(cmds[0],3,1,0,0);vkCmdEndRenderPass(cmds[0]);
 start(cmds[0],scenePass,sceneFb);assert(bound[cmds[0]]==pipeline);vkCmdEndRenderPass(cmds[0]);
 vkDestroyImage(device,depth,nullptr);
 assert(!m837Trackers.at(device).readyUnorm(m833Id(unorm),m833Id(unormPass),m833Id(depth),GetTickCount64()));
 vkDestroyPipeline(device,pipeline,nullptr);assert(!m838Variants.count(pipeline) && destroyedPipelines.count(coarse));
 vkDestroyRenderPass(device,unormPass,nullptr);assert(!m838UnormPasses.count(unormPass) && !m838DepthSlots.count(unormPass));
 assert(draws>0 && submits==4 && uniformCreates>=2);
}
}

int main(int argc,char** argv){
 const bool baseline=argc>1 && std::string(argv[1])=="baseline";
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\nonexistent-unorm-scene-test.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_UNORM_SCENE","1");SetEnvironmentVariableA("BEAMNG_VRS_COMPARE_UNORM","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SHARED_SCENE","0");SetEnvironmentVariableA("BEAMNG_VRS_EARLY_PASSES","off");
 SetEnvironmentVariableA("BEAMNG_VRS_PROFILE_GPU","0");SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M89_STATIC_TEST","1");SetEnvironmentVariableA("BEAMNG_VRS_M811_BASELINE",baseline?"1":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_M84_CPU_GAZE","0");SetEnvironmentVariableA("BEAMNG_VRS_M812H_FENCE_TEST","0");
 SetEnvironmentVariableA("BEAMNG_VRS_ATTACHMENT_PIPELINES","0");SetEnvironmentVariableA("BEAMNG_VRS_UNIFORM_4X4","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE","1");SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_TOP","0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_BOTTOM","0.30");SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_LEFT","0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_RIGHT","0");SetEnvironmentVariableA("BEAMNG_VRS_DEBUG_OUTLINE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");m814ActiveWidth=100;m814ActiveHeight=80;
 assert(m838Enabled() && !m838Compare() && m837Enabled() && !m837ApplyShared() && !m833Enabled() && !m835Enabled());
 exercise(baseline);
 std::cout<<"UNORM real-hook learning, original-pass fallback, crop preservation and lifetime passed"<<(baseline?" (baseline)":"")<<"\n";
}
