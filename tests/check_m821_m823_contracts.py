from pathlib import Path
s=Path('src/layer.cpp').read_text()
assert 'BEAMNG_VRS_GAZE_UPDATE_MS' in s
assert 'beamng_gaze_timing::shouldPrepare(now,previous,m821UpdateIntervalMs())' in s
assert 'lastStagedSequence' in s
assert 'result.sourceTickMs=tick;result.sourceSequence=after' in s
assert 'slots.sourceTickMs[eye][selected]=gaze.sourceTickMs;' in s
assert 'm821RecordUploadAge' in s and 'M8.21 GAZE_STATS' in s
assert 'm823LazyPlans[*out]=std::move(plan);' in s
assert 'BEAMNG_VRS_M823_LAZY_FB' in s
assert 'm823EnsureEyeFramebuffer(device,info->framebuffer,selectedEye)' in s
assert 'm823LazyPlans.erase(framebuffer);' in s
assert 'std::lock_guard<std::mutex> lazyGuard(m823CreateMutex);' in s
assert 'M8.23 FB_STATS' in s
assert 'm815Pending.push_back' in s and 'm81DrawOutline(cmd,d)' in s
print('M8.21/M8.23 source-contract checks passed')
