#pragma once
#include <vulkan/vulkan.h>

// Uniform pipeline-rate experiment for an already identified scene pass.
// This helper neither identifies render targets nor enables device features.
// The caller must check pipelineFragmentShadingRate and query 4x4 support for
// one sample, then keep the original render pass, viewport, and scissor state.
namespace beamng_unorm {

inline bool eligible(const VkGraphicsPipelineCreateInfo& info) {
 // This pilot has no interpreter for extension state, libraries, derivatives,
 // or application-supplied shading rates. Reject rather than rewrite them.
 constexpr VkPipelineCreateFlags allowedFlags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
 if(info.pNext || (info.flags & ~allowedFlags) || info.basePipelineHandle ||
    !info.renderPass || !info.pStages || !info.stageCount) return false;
 bool vertex=false,fragment=false;
 for(uint32_t i=0;i<info.stageCount;++i) {
  const auto& stage=info.pStages[i];
  if(stage.pNext || stage.flags || !stage.module) return false;
  switch(stage.stage) {
   case VK_SHADER_STAGE_VERTEX_BIT: if(vertex)return false;vertex=true;break;
   case VK_SHADER_STAGE_FRAGMENT_BIT: if(fragment)return false;fragment=true;break;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
   case VK_SHADER_STAGE_GEOMETRY_BIT: break;
   default: return false; // No mesh/task pipelines in this experiment.
  }
 }
 if(!vertex || !fragment) return false;
 if(!info.pVertexInputState || info.pVertexInputState->pNext ||
    !info.pInputAssemblyState || info.pInputAssemblyState->pNext ||
    (info.pTessellationState && info.pTessellationState->pNext) ||
    !info.pViewportState || info.pViewportState->pNext ||
    !info.pRasterizationState || info.pRasterizationState->pNext ||
    info.pRasterizationState->rasterizerDiscardEnable ||
    !info.pMultisampleState || info.pMultisampleState->pNext ||
    info.pMultisampleState->rasterizationSamples!=VK_SAMPLE_COUNT_1_BIT ||
    info.pMultisampleState->sampleShadingEnable ||
    (info.pDepthStencilState && info.pDepthStencilState->pNext) ||
    !info.pColorBlendState || info.pColorBlendState->pNext) return false;
 if(info.pDynamicState) {
  const auto& dynamic=*info.pDynamicState;
  if(dynamic.pNext || (dynamic.dynamicStateCount && !dynamic.pDynamicStates)) return false;
  for(uint32_t i=0;i<dynamic.dynamicStateCount;++i) {
   switch(dynamic.pDynamicStates[i]) {
    case VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR:
    case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT:
    case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: return false;
    default: break;
   }
  }
 }
 return true;
}

inline VkPipelineFragmentShadingRateStateCreateInfoKHR rate4x4() {
 VkPipelineFragmentShadingRateStateCreateInfoKHR rate{};
 rate.sType=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
 rate.fragmentSize={4,4};
 // Preserve the pipeline rate at both combining steps. No attachment, eye
 // classification, gaze map, or primitive-rate modification is introduced.
 rate.combinerOps[0]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
 rate.combinerOps[1]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
 return rate;
}
} // namespace beamng_unorm
