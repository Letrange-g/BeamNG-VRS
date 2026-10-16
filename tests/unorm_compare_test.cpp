#include "unorm_compare.hpp"
#include <cassert>
int main(){
 beamng_unorm::CompareClock clock;
 assert(clock.phase(100).measure==-1 && !clock.phase(100).on);
 assert(clock.phase(2099).measure==-1);
 assert(clock.phase(2100).measure==0);
 assert(clock.phase(10099).measure==0);
 auto p=clock.phase(10100);assert(p.on && p.measure==-1);
 assert(clock.phase(12100).measure==1);
 assert(clock.phase(20100).measure==-1 && !clock.phase(20100).on);
 assert(clock.phase(22100).measure==0);
 assert(clock.phase(50).measure==-1); // Defensive clock rollback restarts warmup.
}
