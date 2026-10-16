#include "unorm_pipeline_vrs.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
struct Fixture {
 VkPipelineShaderStageCreateInfo stages[2]{};
 VkPipelineVertexInputStateCreateInfo vertex{};
 VkPipelineInputAssemblyStateCreateInfo assembly{};
 VkPipelineViewportStateCreateInfo viewport{};
 VkPipelineRasterizationStateCreateInfo raster{};
 VkPipelineMultisampleStateCreateInfo samples{};
 VkPipelineDepthStencilStateCreateInfo depth{};
 VkPipelineColorBlendStateCreateInfo blend{};
 VkDynamicState dynamicStates[3]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
 VkPipelineDynamicStateCreateInfo dynamic{};
 VkGraphicsPipelineCreateInfo info{};
 Fixture() {
  stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=handle<VkShaderModule>(1);
  stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=handle<VkShaderModule>(2);
  samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;
  dynamic.dynamicStateCount=3;dynamic.pDynamicStates=dynamicStates;
  info.stageCount=2;info.pStages=stages;info.renderPass=handle<VkRenderPass>(3);
  info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;
  info.pViewportState=&viewport;info.pRasterizationState=&raster;
  info.pMultisampleState=&samples;info.pDepthStencilState=&depth;
  info.pColorBlendState=&blend;info.pDynamicState=&dynamic;
 }
};
}
int main() {
 Fixture f;assert(beamng_unorm::eligible(f.info));
 const auto state=beamng_unorm::rate4x4();
 assert(state.sType==VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR && !state.pNext);
 assert(state.fragmentSize.width==4 && state.fragmentSize.height==4);
 assert(state.combinerOps[0]==VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR &&
        state.combinerOps[1]==VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR);
 // A rate override only extends the create-info chain; original attachments,
 // depth writes, and application viewport/scissor state stay untouched.
 auto variant=f.info;variant.pNext=&state;
 assert(variant.renderPass==f.info.renderPass && variant.pViewportState==&f.viewport &&
        variant.pDynamicState==&f.dynamic && variant.pDepthStencilState==&f.depth);
 assert(f.depth.depthWriteEnable && !f.info.pNext);
 assert(!beamng_unorm::eligible(variant)); // Never stack another override.
 for(const auto flag:{VK_PIPELINE_CREATE_DERIVATIVE_BIT,VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT,
                      VK_PIPELINE_CREATE_LIBRARY_BIT_KHR}) {
  auto info=f.info;info.flags=flag;assert(!beamng_unorm::eligible(info));
 }
 auto info=f.info;info.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;assert(beamng_unorm::eligible(info));
 info=f.info;info.basePipelineHandle=handle<VkPipeline>(4);assert(!beamng_unorm::eligible(info));
 // basePipelineIndex is ignored by Vulkan without DERIVATIVE_BIT.
 info=f.info;info.basePipelineIndex=0;assert(beamng_unorm::eligible(info));
 info.renderPass=VK_NULL_HANDLE;assert(!beamng_unorm::eligible(info));
 info=f.info;info.stageCount=1;assert(!beamng_unorm::eligible(info));
 f.stages[1].stage=VK_SHADER_STAGE_COMPUTE_BIT;assert(!beamng_unorm::eligible(f.info));
 f.stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
 f.stages[1].pNext=&state;assert(!beamng_unorm::eligible(f.info));f.stages[1].pNext=nullptr;
 f.raster.rasterizerDiscardEnable=VK_TRUE;assert(!beamng_unorm::eligible(f.info));f.raster.rasterizerDiscardEnable=VK_FALSE;
 f.samples.rasterizationSamples=VK_SAMPLE_COUNT_2_BIT;assert(!beamng_unorm::eligible(f.info));
 f.samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;f.samples.sampleShadingEnable=VK_TRUE;
 assert(!beamng_unorm::eligible(f.info));f.samples.sampleShadingEnable=VK_FALSE;
 for(const auto disallowed:{VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR,
                           VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT,
                           VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE}) {
  f.dynamicStates[2]=disallowed;assert(!beamng_unorm::eligible(f.info));
 }
 f.dynamicStates[2]=VK_DYNAMIC_STATE_BLEND_CONSTANTS;
 for(const void** chain:{&f.vertex.pNext,&f.assembly.pNext,&f.viewport.pNext,&f.raster.pNext,
                        &f.samples.pNext,&f.depth.pNext,&f.blend.pNext,&f.dynamic.pNext}) {
  *chain=&state;assert(!beamng_unorm::eligible(f.info));*chain=nullptr;
 }
 f.dynamic.pDynamicStates=nullptr;assert(!beamng_unorm::eligible(f.info));
 f.info.pDynamicState=nullptr;assert(beamng_unorm::eligible(f.info));
 f.info.pDepthStencilState=nullptr;assert(beamng_unorm::eligible(f.info));
 std::cout<<"Uniform UNORM pipeline-rate eligibility and unchanged original state passed\n";
}
