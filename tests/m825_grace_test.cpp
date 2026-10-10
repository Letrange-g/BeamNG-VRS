#include "m825_gaze_hold.hpp"
#include <cassert>
#include <iostream>
int main(){
 using namespace beamng_gaze_hold;
 State left{},right{};
 auto d=choose(left,{},1000,150);
 assert(d.phase==Phase::Center && !d.sample.valid && !d.changed);
 Sample good{true,.27f,.61f,998,8};
 d=choose(left,good,1010,150);
 assert(d.phase==Phase::Live && d.changed && d.sample.valid && d.sample.sourceTickMs==998);
 d=choose(left,{},1140,150);
 assert(d.phase==Phase::Grace && d.changed && d.sample.valid && d.sample.u==.27f && d.sample.v==.61f);
 assert(d.sample.sequence==8 && d.sample.sourceTickMs==0);
 d=choose(left,{},1160,150);
 assert(d.phase==Phase::Grace && !d.changed);
 d=choose(left,{},1161,150);
 assert(d.phase==Phase::Center && d.changed && !d.sample.valid && d.sample.u==.5f && d.sample.sequence==0);
 d=choose(left,{},1162,150);
 assert(d.phase==Phase::Center && !d.changed);
 // Re-acquisition after the center fallback restores live tracking.
 d=choose(left,Sample{true,.82f,.21f,1162,10},1170,150);
 assert(d.phase==Phase::Live && d.changed && d.sample.u==.82f);
 // Two eyes are independent; a missing right eye must not inherit left gaze.
 d=choose(right,{},1170,150);
 assert(d.phase==Phase::Center && d.sample.u==.5f);
 d=choose(right,Sample{true,.15f,.75f,1180,12},1180,150);
 assert(d.phase==Phase::Live);
 d=choose(right,{},1181,0);
 assert(d.phase==Phase::Center);
 // No false grace on monotonic-clock wrap/backwards intervals.
 d=choose(left,{},1,150);
 assert(d.phase==Phase::Center);
 assert(suppressOutline(1100,1000,350));
 assert(suppressOutline(1349,1000,350));
 assert(!suppressOutline(1350,1000,350));
 assert(!suppressOutline(1000,0,350));
 assert(!suppressOutline(999,1000,350));
 assert(!suppressOutline(1100,1000,0));
 std::cout<<"M8.25 independent gaze grace and resize outline suppression OK\n";
}
