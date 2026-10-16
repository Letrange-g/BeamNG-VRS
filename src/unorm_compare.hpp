#pragma once
#include <cstdint>

namespace beamng_unorm {
// Recording-time A/B scheduling. Each selected scope keeps its decision until
// endPass, so crossing a phase boundary never changes shading midway through it.
struct ComparePhase { bool on=false; int measure=-1; };
struct CompareClock {
 uint64_t start=0; bool started=false;
 ComparePhase phase(uint64_t now) {
  if(!started || now<start){start=now;started=true;}
  const auto age=now-start;
  const bool on=((age/10000)%2)!=0;
  return {on,age%10000>=2000?int(on):-1};
 }
};
}
