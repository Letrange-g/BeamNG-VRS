#pragma once
#include <cstdint>

// M8.25: independent per-eye grace before reverting to fixed-center VRS.
// Timestamps are monotonic process milliseconds (GetTickCount64 in the layer).
// This helper is deliberately platform independent for deterministic tests.
namespace beamng_gaze_hold {
struct Sample {
 bool valid=false;
 float u=.5f, v=.5f;
 uint64_t sourceTickMs=0;
 int32_t sequence=0;
};
enum class Phase : uint8_t { Center, Live, Grace };
struct State {
 Sample last{};
 bool hasLast=false;
 uint64_t lastSeenMs=0;
 Phase phase=Phase::Center;
};
struct Decision {
 Sample sample{};
 Phase phase=Phase::Center;
 bool changed=false;
};
inline bool withinMs(uint64_t now,uint64_t then,uint32_t durationMs){
 return then!=0 && now>=then && (now-then)<=durationMs;
}
inline Decision choose(State& state,const Sample& raw,uint64_t now,uint32_t graceMs){
 Decision result{};
 if(raw.valid){
  state.hasLast=true;
  state.last=raw;
  state.lastSeenMs=now;
  result.sample=raw;
  result.phase=Phase::Live;
 }else if(state.hasLast && withinMs(now,state.lastSeenMs,graceMs)){
  result.sample=state.last;
  // A retained pose is not a newly sampled eye pose; exclude from source-age telemetry.
  result.sample.sourceTickMs=0;
  result.phase=Phase::Grace;
 }else{
  state.hasLast=false;
  result.sample={false,.5f,.5f,0,0};
  result.phase=Phase::Center;
 }
 result.changed=(result.phase!=state.phase);
 state.phase=result.phase;
 return result;
}
inline bool suppressOutline(uint64_t now,uint64_t lastResizeMs,uint32_t pauseMs){
 return lastResizeMs!=0 && now>=lastResizeMs && now-lastResizeMs<pauseMs;
}
} // namespace beamng_gaze_hold
