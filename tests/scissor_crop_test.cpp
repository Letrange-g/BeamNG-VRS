#include "scissor_crop.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <random>

bool contains(beamng_scissor::Rect r,uint32_t x,uint32_t y) {
 return x>=r.x && y>=r.y && x-r.x<r.width && y-r.y<r.height;
}
void partition(uint32_t width,uint32_t height,beamng_scissor::Margins margins) {
 const auto crop=beamng_scissor::make(width,height,margins);
 uint64_t area=uint64_t(crop.keep.width)*crop.keep.height;
 for(const auto r:crop.outside){
  assert(uint64_t(r.x)+r.width<=width && uint64_t(r.y)+r.height<=height);
  area+=uint64_t(r.width)*r.height;
 }
 assert(area==uint64_t(width)*height);
 if(width && height)assert(crop.keep.width && crop.keep.height);
 for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
  unsigned count=contains(crop.keep,x,y);
  for(const auto r:crop.outside)count+=contains(r,x,y);
  assert(count==1); // Every pixel is either retained or cleared exactly once.
 }
}
int main(){
 const auto legacy=beamng_scissor::make(5190,4168,{.30,.30,0,0});
 assert(legacy.keep.x==0 && legacy.keep.y==1250 && legacy.keep.width==5190 && legacy.keep.height==1668);
 assert(legacy.outside[2].width==0 && legacy.outside[3].width==0);
 const auto asymmetric=beamng_scissor::make(100,100,{.10,.20,.30,.05});
 assert(asymmetric.keep.x==30 && asymmetric.keep.y==10 && asymmetric.keep.width==65 && asymmetric.keep.height==70);
 for(const auto margins:{beamng_scissor::Margins{0,0,0,0},{.30,.30,.05,.10},{0,0,.10,.20},{.49,.49,.49,.49}})
  for(const auto width:{0u,1u,3u,17u,63u})for(const auto height:{0u,1u,5u,31u})partition(width,height,margins);
 // Environment readers already sanitize inputs; geometry also stays bounded.
 partition(19,13,{-1,1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()});
 std::mt19937 random(0x829);
 for(unsigned i=0;i<500;++i){
  beamng_scissor::Margins margins{double(random()%50)/100,double(random()%50)/100,
                                 double(random()%50)/100,double(random()%50)/100};
  partition(1+random()%63,1+random()%45,margins);
 }
 std::cout<<"Four-edge crop: bounded, non-overlapping full pixel partition, asymmetric margins, tiny images and randomized cases passed\n";
}
