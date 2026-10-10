#include "ffr_pattern.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
 using namespace beamng_ffr;
 assert(rate(1,1)==0 && rate(2,2)==5 && rate(4,4)==10);
 for(auto [w,h]: {std::pair<unsigned,unsigned>{2595,2084},{3892,3126},{5190,4168}}) {
  const float size=.5f;
  const float pxR=.25f*h;
  const float horz=pxR/float(w);
  const float vert=pxR/float(h);
  // 0.5 means physical diameter 0.5*eye height, with a square side of the same size.
  assert(inside(.5f+horz*.99f,.5f,.5f,.5f,w,h,size,Shape::Circle));
  assert(!inside(.5f+horz*1.01f,.5f,.5f,.5f,w,h,size,Shape::Circle));
  assert(inside(.5f+horz*.99f,.5f+vert*.99f,.5f,.5f,w,h,size,Shape::Square));
  assert(!inside(.5f+horz*1.01f,.5f,.5f,.5f,w,h,size,Shape::Square));
  assert(!inside(.5f+horz*.9f,.5f+vert*.9f,.5f,.5f,w,h,size,Shape::Circle));
  // Ellipse: 50% viewport width and 50% viewport height.
  assert(inside(.5f+.25f*.99f,.5f,.5f,.5f,w,h,size,Shape::Ellipse));
  assert(!inside(.5f+.25f*1.01f,.5f,.5f,.5f,w,h,size,Shape::Ellipse));
  for(Shape shape: {Shape::Circle,Shape::Square,Shape::Ellipse}) {
   const auto p=makeGaze(w,h,16,16,.5f,.5f,.5f,.8f,rate(2,2),rate(4,4),shape);
   const auto centered=make(w,h,16,16,.5f,.8f,shape);
   assert(p.texels==centered.texels);
   assert(p.width==(w+15)/16&&p.height==(h+15)/16);
   unsigned fine=0,mid=0,outer=0;
   for(uint32_t y=0;y<p.height;++y)for(uint32_t x=0;x<p.width;++x){
    const uint8_t actual=p.texels[size_t(y)*p.width+x];
    const float u=tileUV(x,16,w),v=tileUV(y,16,h);
    assert(actual==(inside(u,v,.5f,.5f,w,h,.5f,shape)?rate(1,1):
       inside(u,v,.5f,.5f,w,h,.8f,shape)?rate(2,2):rate(4,4)));
    fine+=(actual==0);mid+=(actual==5);outer+=(actual==10);
   }
   // Each zone really has pixels and the shape selection changes coverage.
   assert(fine>0&&mid>0&&outer>0);
   std::cout<<w<<"x"<<h<<" "<<shapeName(shape)<<": 1x1="<<fine<<" 2x2="<<mid<<" 4x4="<<outer<<"\n";
  }
  const auto moved=makeGaze(w,h,16,16,.80f,.12f,.5f,.8f,rate(2,2),rate(4,4),Shape::Circle);
  assert(moved.texels!=make(w,h,16,16,.5f,.8f,Shape::Circle).texels);
 }
 const auto noCenter=make(2595,2084,16,16,0.f,.2f);
 for(auto v:noCenter.texels)assert(v!=rate(1,1));
 bool rejected=false;
 try {(void)make(2595,2084,16,16,.8f,.2f);}catch(const std::invalid_argument&){rejected=true;}
 assert(rejected);
 std::cout<<"M8.24 shape/size tests passed\n";
}
