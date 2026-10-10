#include "../src/m821_gaze_timing.hpp"
#include <cassert>
int main(){
 using namespace beamng_gaze_timing;
 static_assert(clampInterval(0)==2 && clampInterval(8)==8 && clampInterval(900)==50);
 static_assert(!shouldPrepare(105,100,8) && shouldPrepare(108,100,8));
 static_assert(!shouldPrepare(1,10,8));
 static_assert(ageMs(300,250)==50 && ageMs(100,200)==0);
 return 0;
}
