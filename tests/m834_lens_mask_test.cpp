#include "src/lens_mask.hpp"
#include <cassert>
#include <iostream>
// M8.34: the mask covers only pixels entirely outside the ellipse, never
// overlaps itself, and approaches the analytic outside area.
int main(){
 for(auto [w,h]:{std::pair<uint32_t,uint32_t>{2595,2084},{3892,3126},{5190,4168},{97,61}}){
  for(double size:{0.8,1.0,1.1,1.2,1.4}){
   const auto rects=beamng_lens::outside(w,h,size);
   std::vector<uint8_t> masked(size_t(w)*h,0);
   uint64_t count=0;
   for(const auto& r:rects){
    assert(r.width && r.height && r.x+r.width<=w && r.y+r.height<=h);
    for(uint32_t y=r.y;y<r.y+r.height;++y)for(uint32_t x=r.x;x<r.x+r.width;++x){
     assert(!masked[size_t(y)*w+x]);masked[size_t(y)*w+x]=1;++count;
    }
   }
   const double cx=w/2.0,cy=h/2.0,a=size*w/2.0,b=size*h/2.0;
   uint64_t outsideCenters=0;
   for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
    // Every corner of a masked pixel lies outside the ellipse.
    const double px[2]={double(x),double(x+1)},py[2]={double(y),double(y+1)};
    bool anyInside=false;
    for(double qx:px)for(double qy:py){
     const double dx=(qx-cx)/a,dy=(qy-cy)/b;
     if(dx*dx+dy*dy<1.0)anyInside=true;
    }
    if(masked[size_t(y)*w+x])assert(!anyInside);
    const double dx=(x+.5-cx)/a,dy=(y+.5-cy)/b;
    outsideCenters+=dx*dx+dy*dy>1.0;
   }
   // Conservative banding keeps a little extra, but most of the outside area is masked.
   assert(count<=outsideCenters);
   if(w>1000 && outsideCenters>size_t(w)*h/100)assert(count*100>=outsideCenters*90);
   if(w==5190 && size==1.0)std::cout<<"5190x4168 size=1.0 masked "<<(100.0*count/(double(w)*h))<<"% in "<<rects.size()<<" rects\n";
  }
 }
 assert(beamng_lens::outside(5190,4168,1.5).empty());
 assert(beamng_lens::outside(5190,4168,0.4).empty());
 assert(beamng_lens::outside(0,10,1.0).empty());
 std::cout<<"M8.34 lens mask tests passed\n";
}
