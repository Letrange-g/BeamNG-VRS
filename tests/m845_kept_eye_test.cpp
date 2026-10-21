// M8.45: with the FOV crop the image is the kept part of the full eye; the
// sharp area must cover the same part of the full eye as without the crop.
#include "ffr_pattern.hpp"
#include <cassert>
#include <iostream>

int main(){
 using namespace beamng_ffr;
 const uint32_t fw=1600,fh=1000,left=80,top=300,w=1440,h=400; // kept 90% x 40%
 const float ku=float(w)/fw,kv=float(h)/fh;
 for(Shape shape:{Shape::Circle,Shape::Square,Shape::Ellipse})
  for(float size:{.25f,.55f,.9f}){
   const float gu=.45f,gv=.55f; // gaze in the full eye
   const float cu=(gu*fw-left)/w,cv=(gv*fh-top)/h;
   unsigned mismatches=0,covered=0;
   for(uint32_t y=0;y<h;y+=2)for(uint32_t x=0;x<w;x+=2){
    const bool full=inside((x+left+.5f)/fw,(y+top+.5f)/fh,gu,gv,fw,fh,size,shape);
    const bool crop=inside((x+.5f)/w,(y+.5f)/h,cu,cv,w,h,size,shape,ku,kv);
    mismatches+=full!=crop;covered+=crop;
   }
   assert(covered>0 && mismatches<=w/2); // only float rounding on the boundary
  }
 // Defaults keep the uncropped behaviour, and the middle band may exceed the kept height.
 const auto a=makeGaze(320,200,16,16,.5f,.5f,.25f,.55f);
 const auto b=makeGaze(320,200,16,16,.5f,.5f,.25f,.55f,rate(2,2),rate(4,4),Shape::Circle,1.f,1.f);
 assert(a.texels==b.texels);
 const auto wide=makeGaze(320,80,16,16,.5f,.5f,.25f,.55f,rate(2,2),rate(4,4),Shape::Circle,1.f,.4f);
 assert(wide.texels[size_t(0)*wide.width+wide.width/2]==rate(2,2)); // middle reaches the top edge
 std::cout<<"M8.45 kept-eye foveation sizes passed\n";
}
