#pragma once
#include <cstdint>
#include <cmath>
namespace beamng_gaze_freshness {
inline bool fresh(uint64_t now,uint64_t tick,uint64_t maximumAge=100){
 return tick && now>=tick && now-tick<=maximumAge;
}
inline bool validProjection(uint64_t now,uint64_t poseTick,uint64_t projectionTick,
 uint32_t mask,int eye,float u,float v){
 return eye>=0 && eye<2 && (mask&(1u<<eye)) && fresh(now,poseTick) && fresh(now,projectionTick) &&
  std::isfinite(u) && std::isfinite(v) && u>=0.f && u<=1.f && v>=0.f && v<=1.f;
}
}
