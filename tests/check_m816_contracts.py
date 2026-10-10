"""Structural regression checks for M8.16 (not a replacement for MSVC/GPU tests)."""
from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/layer.cpp').read_text()
fb=s[s.index('extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(\n'):s.index('extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(\n')]
assert 'm814EnsureResources(device,xrW,xrH)' in fb
assert 'm83EyeFramebuffers[*out]={device,eyeFbs};' in fb
assert 'shadowFramebuffers[*out]={device,VK_NULL_HANDLE};' in fb
assert 'shadowFramebuffers[*out]={device,shadowFb};' in fb
assert 'm812FramebufferPools[*out]=spare' not in fb
assert 'd.createFramebuffer(device,&variant,alloc,&eyeFbs[eye])' in fb
assert 'd.createFramebuffer(device,&variant,alloc,&shadowFb)' in fb
assert 'if(it4->second.shadow)orphanFramebuffers.push_back(it4->second.shadow)' in s
assert 'for (auto fb : orphanFramebuffers) if (destroyFb) destroyFb(device,fb,nullptr);' in s
assert 'if (shadow && d.destroyFramebuffer) d.destroyFramebuffer(device, shadow, alloc);' in s
assert 'for(auto& eye:fresh.imagePool.extra)for(auto& image:eye)\n   image=beamng_ffr::upload' not in s
assert 'm812ImagePools[device]=M812ImagePool{};' in s
assert 'm815PollLocked(device,dispatch)' in s
assert 'M8.15 GPU_GAZE_UPLOADED' in s
assert 'M8.23 FB_STATS' in s
assert 'shadowPassReports.fetch_add(1,std::memory_order_relaxed)<12' in s
print('M8.16 framebuffer lifecycle and unchanged async path: checks passed')
