#pragma once
#include <cstdint>
namespace beamng_gaze_timing {
// The render-pass hooks may run thousands of times per second; only prepare
// a new gaze pattern at this bounded cadence. Default is 8 ms (~125 Hz cap).
inline constexpr uint32_t clampInterval(unsigned long requested){
 return requested<2 ? 2u : requested>50 ? 50u : static_cast<uint32_t>(requested);
}
inline constexpr bool shouldPrepare(uint64_t now,uint64_t previous,uint32_t interval){
 return now>=previous && now-previous>=interval;
}
inline constexpr uint64_t ageMs(uint64_t now,uint64_t sourceTick){
 return now>=sourceTick ? now-sourceTick : 0;
}
}
