#include "tests/pattern_reference.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <array>
#include <random>

// Measures CPU construction of ONE eye's attachment payload. No GPU or frame
// timing is measured. Compile with normal Release FP rules, not fast-math.
int main(){
 using namespace beamng_ffr;
 std::array<std::pair<float,float>,128> gazes;
 std::mt19937 rng(827);std::uniform_real_distribution<float> uv(0.f,1.f);
 for(auto& gaze:gazes)gaze={uv(rng),uv(rng)};
 uint64_t consumed=0;
 std::cout<<"resolution,shape,reference_us,span_us,speedup\n";
 for(auto [w,h]:{std::pair{2595u,2084u},{3892u,3126u},{5190u,4168u}})
  for(Shape shape:{Shape::Circle,Shape::Square,Shape::Ellipse}){
   for(auto [u,v]:gazes){
    const auto old=referencePattern(w,h,16,16,u,v,.25f,.60f,shape);
    const auto now=makeGaze(w,h,16,16,u,v,.25f,.60f,rate(2,2),rate(4,4),shape);
    if(old.texels!=now.texels){std::cerr<<"Byte mismatch\n";return 1;}
   }
   auto measure=[&](bool spans){
    auto start=std::chrono::steady_clock::now();
    for(auto [u,v]:gazes){
     auto p=spans?makeGaze(w,h,16,16,u,v,.25f,.60f,rate(2,2),rate(4,4),shape):
                  referencePattern(w,h,16,16,u,v,.25f,.60f,shape);
     consumed+=p.texels[size_t((u+v)*997)%p.texels.size()];
    }
    return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/gazes.size();
   };
   std::array<double,9> old{},now{};
   for(size_t i=0;i<old.size();++i){
    if(i%2){now[i]=measure(true);old[i]=measure(false);}
    else {old[i]=measure(false);now[i]=measure(true);}
   }
   std::sort(old.begin(),old.end());std::sort(now.begin(),now.end());
   std::cout<<w<<"x"<<h<<","<<shapeName(shape)<<","<<std::fixed<<std::setprecision(3)
            <<old[4]<<","<<now[4]<<","<<old[4]/now[4]<<"\n";
  }
 std::cerr<<"Consumed checksum: "<<consumed<<"\n";
}
