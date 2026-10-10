#include "src/ffr_pattern.hpp"
#include <iostream>
#include <cassert>
int main(){
 for(auto [w,h,tx,ty]: {std::tuple<int,int,int,int>{2595,2084,163,131}, {3892,3126,244,196}, {5190,4168,325,261}}){
  auto p=beamng_ffr::makeGaze(w,h,16,16,.43f,.66f,.05f,.10f,beamng_ffr::rate(2,2),beamng_ffr::rate(4,4));
  assert(int(p.width)==tx&&int(p.height)==ty&&int(p.texels.size())==tx*ty);
  auto center=p.texels[size_t(int(.66f*ty))*tx+size_t(int(.43f*tx))];
  assert(center==0);
  std::cout<<w<<"x"<<h<<" -> "<<p.width<<"x"<<p.height<<" rate-center "<<int(center)<<"\n";
 }
}
