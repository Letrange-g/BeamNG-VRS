from pathlib import Path
s=Path('src/layer.cpp').read_text()
p=Path('src/ffr_pattern.hpp').read_text()
assert 'enum class Shape : uint8_t { Circle, Square, Ellipse }' in p
assert 'dx*dx+dy*dy<=halfHeight*halfHeight' in p
assert 'std::max(std::fabs(dx),std::fabs(dy))<=halfHeight' in p
assert 'const float bound=.5f*size;' in p
assert 'BEAMNG_VRS_SHAPE' in s and 'M8.24 SHAPE shape=' in s
assert 'm824Shape());' in s
assert 'beamng_ffr::inside(u,v,centerU,centerV,state.width,state.height,size,shape)' in s
assert 'const bool centeredFallback=decision.phase==beamng_gaze_hold::Phase::Center;' in s
assert 'if(lastStagedSequence[eye].load(std::memory_order_relaxed)==0)continue;' in s
assert 'gaze.u=.5f;gaze.v=.5f;gaze.sourceSequence=0;gaze.sourceTickMs=0;' in s
assert 'if(centeredFallback)' in s and 'M8.24 GAZE_FALLBACK' in s
assert 'centerU=m825StagedCenters[state.eye].u;' in s and 'centerV=m825StagedCenters[state.eye].v;' in s
assert 'if(source && m821SampleTime>=source)' in s
assert 'm823EnsureEyeFramebuffer' in s
assert 'M8.21 GAZE_STATS' in s
print('M8.24 geometry and centered fallback preserved under M8.25')
