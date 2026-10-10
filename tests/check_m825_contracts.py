from pathlib import Path
s=Path('src/layer.cpp').read_text()
g=Path('src/m825_gaze_hold.hpp').read_text()
assert '#include "m825_gaze_hold.hpp"' in s
assert 'BEAMNG_VRS_GAZE_GRACE_MS' in s
assert 'BEAMNG_VRS_OUTLINE_RESIZE_PAUSE_MS' in s
assert 'beamng_gaze_hold::choose(m825GazeStates[eye],sample,now,m825GraceMs())' in s
assert 'm825EffectiveGaze(eye)' in s
assert 'const bool centeredFallback=decision.phase==beamng_gaze_hold::Phase::Center;' in s
assert 'm825StagedCenters[eye]={gaze.u,gaze.v};' in s
assert 'centerU=m825StagedCenters[state.eye].u' in s
assert 'centerV=m825StagedCenters[state.eye].v' in s
assert 'if(selected==m818SelectedEye.end() || selected->second!=eye)return;' in s
assert 'm825LastResizeMs.store(GetTickCount64(),std::memory_order_release);' in s
assert 'beamng_gaze_hold::suppressOutline(GetTickCount64()' in s
assert 'if(state.width!=m813Width() || state.height!=m813Height())return;' in s
assert 'sourceTickMs=0' in g
assert 'state.hasLast=false;' in g
assert 'M8.25 GAZE_STATE' in s and 'M8.25 OUTLINE_RESIZE_GUARD' in s
assert 'm823EnsureEyeFramebuffer' in s and 'M8.21 GAZE_STATS' in s
print('M8.25 shared grace, staged-outline synchronization and resize guards passed')
