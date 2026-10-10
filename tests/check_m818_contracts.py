from pathlib import Path
v=Path('src/layer.cpp').read_text()
x=Path('src/openxr_probe.cpp').read_text()
h=Path('src/beamng_gaze_shared.hpp').read_text()
m=Path('src/m818_stencil_mask.hpp').read_text()
assert 'BEAMNG_VRS_M818_STENCIL' in v and 'BEAMNG_VRS_M818_STENCIL' in x
assert 'XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR' in x
assert 'XR_TYPE_VISIBILITY_MASK_KHR' in x
assert 'm818Publish(session,d,views)' in x
assert 'beamng_visibility_mesh::mappingName' in v and 'beamng_visibility_mesh::mappingName' in x
assert 'VK_FORMAT_D24_UNORM_S8_UINT' in v
assert 'stencilLoadOp!=VK_ATTACHMENT_LOAD_OP_LOAD' in v
assert 'm818MaskedVariantsByPass' in v
assert 'VkClearAttachment clear{}' in v
assert 'clear.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT' in v
assert 'VK_COMPARE_OP_EQUAL' in v
assert 'm818StencilPipelineBinds.fetch_add' in v
assert 'contents==VK_SUBPASS_CONTENTS_INLINE' in v
assert 'begin->contents==VK_SUBPASS_CONTENTS_INLINE' in v
assert 'm818MaskedCommands.erase(cmd)' in v
assert 'if(!m818Enabled() || !useShadow' in v
assert 'm815Pending.push_back' in v
assert 'm814EnsureResources(device,xrW,xrH)' in v
assert 'm81DrawOutline(cmd,d)' in v
assert 'maxVertices = 512' in h
assert 'std::vector<Strip> ' not in m # no accidental impossible conversion
print('M8.18 OpenXR mesh, stencil gates, pipeline bindings and baseline checks passed')
