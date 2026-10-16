from pathlib import Path
s=Path('src/layer.cpp').read_text()
x=Path('src/openxr_probe.cpp').read_text()
assert 'BEAMNG_VRS_LOG_LEVEL' in s and 'BEAMNG_VRS_LOG_LEVEL' in x
assert 'M8.17 STATS uploads=' in s
assert 'M8.17 VISIBILITY_MASK runtimeAdvertised=' in x
assert '"XR_KHR_visibility_mask"' in x
assert 'if(!m817Verbose())return;' in s
transfer = s[s.index('void m74hTrace('):s.index('extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(', s.index('void m74hTrace('))]
assert transfer.index('m75TraceTransfer(src,dst);') < transfer.index('if(!m817Verbose())return;'), 'Keep gaze source classification before the debug logging gate'
assert 'm815Pending.push_back' in s and 'm815PollLocked(device,dispatch)' in s, 'Async fence workflow preserved'
assert 'm814EnsureResources(device,xrW,xrH)' in s, 'Dynamic resolution workflow preserved'
assert 'm81DrawOutline(cmd,d)' in s
assert 'const VkRect2D clipped=intersectRect(rects[0],area)' in s
assert 'runtimeVisibilityMask=true' in x
assert 'm814Publish(ci->width,ci->height)' in x
assert 'fopen_s(&stream.file' in s and 'fopen_s(&stream.file' in x
assert 'fclose(f)' not in s and 'fclose(f)' not in x
print('M8.17 logging, scissor, visibility-mask probe and async path: structural checks passed')
