#include "gaze_freshness.hpp"
#include "m825_gaze_hold.hpp"
#include <cassert>
#include <limits>
#include <iostream>
int main(){
 using namespace beamng_gaze_freshness;
 assert(validProjection(1000,1000,1000,3,0,.25f,.75f));
 assert(validProjection(1100,1000,1000,3,1,0.f,1.f));
 // A freshly sampled raw pose must NOT renew old projected UV coordinates.
 assert(!validProjection(1101,1101,1000,3,0,.25f,.75f));
 assert(!validProjection(1101,1000,1101,3,0,.25f,.75f));
 assert(!validProjection(1000,0,1000,3,0,.5f,.5f));
 assert(!validProjection(1000,1001,1000,3,0,.5f,.5f));
 assert(!validProjection(1000,1000,1001,3,0,.5f,.5f));
 assert(!validProjection(1000,1000,1000,1,1,.5f,.5f));
 assert(!validProjection(1000,1000,1000,3,2,.5f,.5f));
 assert(!validProjection(1000,1000,1000,3,-1,.5f,.5f));
 for(float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
  assert(!validProjection(1000,1000,1000,3,0,bad,.5f));
  assert(!validProjection(1000,1000,1000,3,0,.5f,bad));
 }
 beamng_gaze_hold::State state;
 beamng_gaze_hold::Sample sample{true,.25f,.75f,1000,2};
 auto result=beamng_gaze_hold::choose(state,sample,1000,30);assert(result.phase==beamng_gaze_hold::Phase::Live);
 result=beamng_gaze_hold::choose(state,sample,1100,30);assert(result.phase==beamng_gaze_hold::Phase::Live);
 sample.valid=validProjection(1101,1101,1000,3,0,sample.u,sample.v);
 result=beamng_gaze_hold::choose(state,sample,1101,30);assert(result.phase==beamng_gaze_hold::Phase::Grace);
 result=beamng_gaze_hold::choose(state,sample,1131,30);assert(result.phase==beamng_gaze_hold::Phase::Center);
 std::cout<<"Independent pose/projection freshness and grace fallback passed\n";
}
