#include "pattern_reference.hpp"
#include <cassert>
#include <iostream>
#include <random>
#include <limits>
#include <tuple>

int main(){
 using namespace beamng_ffr;
 unsigned cases=0;
 auto check=[&](uint32_t w,uint32_t h,uint32_t tw,uint32_t th,float u,float v,
                float center,float middle,Shape shape,uint8_t mid=5,uint8_t outer=10){
  const auto expected=referencePattern(w,h,tw,th,u,v,center,middle,shape,mid,outer);
  const auto actual=makeGaze(w,h,tw,th,u,v,center,middle,mid,outer,shape);
  assert(actual.width==expected.width && actual.height==expected.height);
  assert(actual.texels==expected.texels);++cases;
 };
 for(Shape shape:{Shape::Circle,Shape::Square,Shape::Ellipse}){
  for(auto [w,h,tw,th]:{std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>{1,1,16,16},
   {33,17,16,16},{7,93,1,13},{2595,2084,16,16},{3892,3126,16,16},
   {5190,4168,16,16},{4168,5190,7,31},{0xffffffffu,0xfffffffeu,0x80000000u,0x80000000u}}){
   for(auto [u,v]:{std::pair{0.f,0.f},{1.f,1.f},{.5f,.5f},{.999f,.001f},{.43f,.66f}})
    for(auto [center,middle]:{std::pair{0.f,0.f},{0.f,.6f},{.25f,.6f},{.6f,.6f},{1.f,1.f}})
     check(w,h,tw,th,u,v,center,middle,shape);
  }
  // Exact boundary equality and the adjacent representable sizes/poses.
  for(float size:{.125f,.25f,.5f,1.f})for(float s:{std::nextafter(size,0.f),size,std::nextafter(size,1.f)}){
   check(32,32,1,1,.5f,.5f,s,s,shape);
   check(32,32,1,1,std::nextafter(.5f,0.f),std::nextafter(.5f,1.f),s,s,shape);
  }
 }
 std::mt19937 rng(827);
 std::uniform_real_distribution<float> uv(0.f,1.f);
 for(unsigned i=0;i<1200;++i){
  const float a=uv(rng),b=uv(rng);
  check(1+rng()%5190,1+rng()%4168,1+rng()%64,1+rng()%64,
        uv(rng),uv(rng),std::min(a,b),std::max(a,b),Shape(i%3),uint8_t(rng()%11),uint8_t(rng()%11));
 }
 std::cout<<"Pattern byte equivalence: "<<cases<<" cases passed\n";
}
