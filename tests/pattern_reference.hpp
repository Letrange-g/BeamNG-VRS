#pragma once
#include "ffr_pattern.hpp"

// Original per-texel implementation, retained as an independent regression
// oracle and benchmark baseline. No optimized span logic belongs here.
inline beamng_ffr::Pattern referencePattern(uint32_t w,uint32_t h,uint32_t tw,uint32_t th,
 float u,float v,float center,float middle,beamng_ffr::Shape shape,
 uint8_t middleRate=beamng_ffr::rate(2,2),uint8_t outerRate=beamng_ffr::rate(4,4)) {
 using namespace beamng_ffr;
 Pattern p{1+(w-1)/tw,1+(h-1)/th,tw,th,{}};
 p.texels.resize(size_t(p.width)*p.height);
 for(uint32_t y=0;y<p.height;++y)for(uint32_t x=0;x<p.width;++x){
  const float tx=tileUV(x,tw,w),ty=tileUV(y,th,h);
  p.texels[size_t(y)*p.width+x]=inside(tx,ty,u,v,w,h,center,shape)?rate(1,1):
   inside(tx,ty,u,v,w,h,middle,shape)?middleRate:outerRate;
 }
 return p;
}
