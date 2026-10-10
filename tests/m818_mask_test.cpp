#include "../src/m818_stencil_mask.hpp"
#include <cassert>
#include <iostream>
#include <vector>
int main(){
 using namespace beamng_stencil;
 Point square[]={{-1,-1},{1,-1},{1,1},{-1,1}};
 std::vector<Strip> s;
 assert(makeStrips(square,4,{-1,1,-1,1},2595,2084,s));
 assert(s.size()==48);
 for(const auto& r:s){assert(r.width==2595); assert(r.height>0);assert(r.x==0);}
 Point hex[]={{-0.7f,-0.8f},{0.7f,-0.8f},{0.9f,0.f},{0.7f,0.8f},{-0.7f,0.8f},{-0.9f,0.f}};
 for(auto [w,h]:{std::pair{2595u,2084u},std::pair{3892u,3126u},std::pair{5190u,4168u}}){
  assert(makeStrips(hex,6,{-1,1,-1,1},w,h,s));
  assert(s.size()==48);
  bool clipped=false;
  for(const auto& r:s){assert(r.x+r.width<=w);assert(r.y+r.height<=h);if(r.width<w)clipped=true;}
  assert(clipped);
 }
 assert(!makeStrips(nullptr,0,{-1,1,-1,1},5190,4168,s));
 std::cout<<"M8.18 conservative mask scanlines pass for 0.5, 0.75, 1.0"<<std::endl;
}
