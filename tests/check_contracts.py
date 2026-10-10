"""Small structural smoke checks for the experimental patch (not GPU validation)."""
from pathlib import Path
s = Path(__file__).resolve().parents[1].joinpath('src/layer.cpp').read_text()
assert 'BEAMNG_VRS_M815_ASYNC' in s
assert 'm815SlotStates' in s and 'm815Pending' in s
assert 'if(!slots.busy[eye][candidate])' in s
assert 'slotState.busy[eye][uploadSlots[eye]]=true' in s
assert 'dispatch.m812hGetFenceStatus(device,it->fence)' in s
assert 'queues->second.size()==1' in s
assert 'M8.15 GPU_GAZE_UPLOADED' in s
assert 'SYNC=%s pending=%zu cpuUs=%llu' in s
assert 'if(!async){\n  const VkResult idle=api.deviceWaitIdle(device);' in s
assert 'if(async){\n   // The fence will release this pool' in s
pos=s.index('extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t count')
block=s[pos:s.index('extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(',pos)]
assert block.index('std::lock_guard<std::mutex> queueSerial(m814ResourceMutex)') < block.index('m812liveBeforeSubmit(q)') < block.index('VkResult r=next(q,count,infos,fence)')
print('M8.15 code-contract smoke checks passed')
