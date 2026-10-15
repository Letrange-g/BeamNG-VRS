// Real pipeline/pass hooks with a fake driver: compatible render-pass handles
// must reuse VRS pipelines, including after the creation pass is destroyed.
#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
#include <new>

namespace {std::atomic<uint64_t> allocations{0};}
void* operator new(size_t n){allocations.fetch_add(1);if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);}
void operator delete[](void* p,size_t)noexcept{std::free(p);}

namespace {
uintptr_t nextPass=100,nextPipeline=10000;
unsigned pipelineCreates=0;
VkPipeline bound{};
VKAPI_ATTR VkResult VKAPI_CALL makePass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass* out){
 *out=reinterpret_cast<VkRenderPass>(nextPass++);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL makeShadow(VkDevice,const VkRenderPassCreateInfo2* ci,const VkAllocationCallbacks*,VkRenderPass* out){
 assert(ci->subpassCount==1 && ci->pSubpasses[0].pNext);
 const auto* rate=static_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(ci->pSubpasses[0].pNext);
 assert(rate->sType==VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
 assert(rate->shadingRateAttachmentTexelSize.width==16 && rate->shadingRateAttachmentTexelSize.height==16);
 assert(ci->pAttachments[ci->attachmentCount-1].format==VK_FORMAT_R8_UINT);
 *out=reinterpret_cast<VkRenderPass>(nextPass++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice,VkRenderPass,const VkAllocationCallbacks*){}
VKAPI_ATTR VkResult VKAPI_CALL makePipeline(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo* ci,const VkAllocationCallbacks*,VkPipeline* out){
 assert(count==1);
 // Copies must actually select the attachment's shading rate.
 if(ci->pNext){
  const auto* rate=static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR*>(ci->pNext);
  assert(rate->sType==VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR);
  assert(rate->combinerOps[1]==VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR);
 }
 ++pipelineCreates;*out=reinterpret_cast<VkPipeline>(nextPipeline++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*){}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline pipeline){bound=pipeline;}
}

int main(){
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\nonexistent-vrs-compatibility-test.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M89_STATIC_TEST","1");
 SetEnvironmentVariableA("BEAMNG_VRS_ATTACHMENT_PIPELINES","0");
 const auto device=reinterpret_cast<VkDevice>(uintptr_t(1));
 const auto cmd=reinterpret_cast<VkCommandBuffer>(uintptr_t(2));
 DeviceDispatch dispatch{};dispatch.vrsEnabled=true;dispatch.createRenderPass=makePass;
 dispatch.createRenderPass2=makeShadow;dispatch.destroyRenderPass=destroyPass;
 dispatch.createPipelines=makePipeline;dispatch.destroyPipeline=destroyPipeline;dispatch.cmdBindPipeline=bindPipeline;
 devices[device]=dispatch;commandDevices[cmd]=device;

 VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R8G8B8A8_UNORM;
 attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
 attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
 VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
 sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
 VkSubpassDependency dependency{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0};
 VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
 ci.attachmentCount=1;ci.pAttachments=&attachment;ci.subpassCount=1;ci.pSubpasses=&sub;
 ci.dependencyCount=1;ci.pDependencies=&dependency;
 auto create=[&]{VkRenderPass pass{};assert(vkCreateRenderPass(device,&ci,nullptr,&pass)==VK_SUCCESS);return pass;};
 const auto first=create();const auto firstShadow=shadowPasses.at(first).shadow;
 VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipelineInfo.renderPass=first;
 VkPipeline original{};assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&original)==VK_SUCCESS);
 assert(pipelineCreates==2 && m89Variants.count(original));const auto replacement=m89Variants.at(original).pipeline;
 auto bind=[&](VkRenderPass shadow,VkPipeline expected){
  m89ActiveShadows[cmd]=shadow;vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==expected);
 };
 bind(firstShadow,replacement);
 // Only permitted layout/load/store differences; a distinct shadow handle.
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;color.layout=VK_IMAGE_LAYOUT_GENERAL;
 const auto second=create();const auto secondShadow=shadowPasses.at(second).shadow;
 assert(firstShadow!=secondShadow);bind(secondShadow,replacement);
 const auto before=allocations.load();
 for(unsigned i=0;i<1000;++i)vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);
 assert(allocations.load()==before && pipelineCreates==2);
 vkDestroyRenderPass(device,first,nullptr);assert(!m832ShadowSignatures.count(firstShadow));
 bind(secondShadow,replacement); // Lifetime must not depend on the old handle.

 // Recycle BOTH old handles with an incompatible format. Equality of the old
 // shadow handle is no longer sufficient to substitute the saved pipeline.
 nextPass=reinterpret_cast<uintptr_t>(first);attachment.format=VK_FORMAT_R16G16B16A16_SFLOAT;
 auto recycled=create();assert(recycled==first && shadowPasses.at(recycled).shadow==firstShadow);
 bind(firstShadow,original);vkDestroyRenderPass(device,recycled,nullptr);
 attachment.format=VK_FORMAT_R8G8B8A8_UNORM;
 nextPass=200;
 auto mismatch=[&]{auto pass=create();bind(shadowPasses.at(pass).shadow,original);vkDestroyRenderPass(device,pass,nullptr);};
 attachment.flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;mismatch();attachment.flags=0;
 dependency.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT;mismatch();dependency.dependencyFlags=0;
 dependency.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;mismatch();dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
 dependency.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mismatch();dependency.srcAccessMask=0;
 ci.dependencyCount=0;mismatch();ci.dependencyCount=1;
 sub.flags=VK_SUBPASS_DESCRIPTION_PER_VIEW_ATTRIBUTES_BIT_NVX;mismatch();sub.flags=0;
 color.attachment=VK_ATTACHMENT_UNUSED;mismatch();color.attachment=0;
 // Multisampling/extension state remain outside the supported scope.
 attachment.samples=VK_SAMPLE_COUNT_4_BIT;auto multisample=create();assert(!shadowPasses.count(multisample));
 vkDestroyRenderPass(device,multisample,nullptr);attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
 ci.pNext=&multiview;auto extended=create();assert(!shadowPasses.count(extended));
 assert(m88Signature(ci).fields.empty());vkDestroyRenderPass(device,extended,nullptr);ci.pNext=nullptr;
 // Missing metadata, no active shadow, another device, and compute binds must
 // never inherit graphics VRS substitution.
 bind(reinterpret_cast<VkRenderPass>(uintptr_t(999)),original);
 m89ActiveShadows.erase(cmd);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==original);
 const auto otherDevice=reinterpret_cast<VkDevice>(uintptr_t(3));devices[otherDevice]=dispatch;commandDevices[cmd]=otherDevice;
 bind(secondShadow,original);commandDevices[cmd]=device;
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,original);assert(bound==original);
 bind(secondShadow,replacement);
 vkDestroyPipeline(device,original,nullptr);assert(!m89Variants.count(original));
 vkDestroyRenderPass(device,second,nullptr);assert(m832ShadowSignatures.empty());
 // Device teardown also owns any remaining compatibility metadata.
 create();assert(!m832ShadowSignatures.empty());vkDestroyDevice(device,nullptr);
 assert(m832ShadowSignatures.empty());
 std::cout<<"VRS compatible-pass selection and lifetime passed\n";
}
