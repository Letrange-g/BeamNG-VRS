#include "src/ffr_pattern.hpp"
#include <cassert>
#include <iostream>
// M8.32: the combined pattern used for unclassified eye-sized passes must never
// shade a tile more coarsely than either eye's own pattern.
int main(){
 using beamng_ffr::rate;
 const uint8_t rates[]={rate(1,1),rate(2,2),rate(4,4),rate(1,2),rate(2,1),rate(4,2),rate(2,4)};
 for(uint8_t a:rates)for(uint8_t b:rates){
  const uint8_t c=beamng_ffr::finerRate(a,b);
  assert((c&3)==std::min(a&3,b&3));
  assert(((c>>2)&3)==std::min((a>>2)&3,(b>>2)&3));
  assert(beamng_ffr::finerRate(b,a)==c);
 }
 for(auto [w,h]:{std::pair<uint32_t,uint32_t>{2595,2084},{5190,4168}}){
  const auto left=beamng_ffr::makeGaze(w,h,16,16,.40f,.50f,.25f,.60f);
  const auto right=beamng_ffr::makeGaze(w,h,16,16,.60f,.45f,.25f,.60f);
  std::vector<uint8_t> combined;
  beamng_ffr::combineFiner(left.texels,right.texels,combined);
  assert(combined.size()==left.texels.size());
  size_t finer=0;
  for(size_t i=0;i<combined.size();++i){
   assert(combined[i]<=left.texels[i] && combined[i]<=right.texels[i]);
   finer+=combined[i]<left.texels[i] || combined[i]<right.texels[i];
  }
  assert(finer>0);
  // Identical eyes combine to the same payload byte for byte.
  beamng_ffr::combineFiner(left.texels,left.texels,combined);
  assert(combined==left.texels);
 }
 bool threw=false;
 std::vector<uint8_t> out;
 try{beamng_ffr::combineFiner(std::vector<uint8_t>(4),std::vector<uint8_t>(5),out);}
 catch(const std::invalid_argument&){threw=true;}
 assert(threw);
 std::cout<<"M8.32 combined pattern tests passed\n";
}
