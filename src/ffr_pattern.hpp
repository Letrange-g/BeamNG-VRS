#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <utility>

// CPU-side shading-rate attachment payload for VK_FORMAT_R8_UINT.
// Vulkan encodes width in bits 0-1 and height in bits 2-3, as log2(pixel size).
namespace beamng_ffr {
struct Pattern {
 uint32_t width, height, tileWidth, tileHeight;
 std::vector<uint8_t> texels;
};
enum class Shape : uint8_t { Circle, Square, Ellipse };
inline const char* shapeName(Shape shape) {
 switch(shape) {
  case Shape::Circle: return "circle";
  case Shape::Square: return "square";
  case Shape::Ellipse: return "ellipse";
 }
 return "circle";
}
inline uint8_t rate(uint32_t x, uint32_t y) {
 if (x != 1 && x != 2 && x != 4) throw std::invalid_argument("unsupported rate width");
 if (y != 1 && y != 2 && y != 4) throw std::invalid_argument("unsupported rate height");
 auto exponent = [](uint32_t n) -> uint8_t { return n == 1 ? 0 : n == 2 ? 1 : 2; };
 return static_cast<uint8_t>(exponent(x) | (exponent(y) << 2));
}
// M8.24: sizes are DIAMETERS (or square side lengths) divided by the
// physical pixel height of ONE eye image. size=0.5 means 0.5*height pixels.
// Ellipse is the deliberate viewport-relative variant: width=size*width,
// height=size*height. The exact same test drives static VRS, gaze VRS and
// the red preview so the debug boundary is not misleading.
// M8.45: keptU/keptV are the fractions of the full eye that the image shows
// (D3D12 FOV crop). Sizes stay relative to the full eye, so the sharp area
// covers the same part of the lens with or without the crop.
inline bool inside(float u, float v, float gazeU, float gazeV,
                   uint32_t pixelWidth, uint32_t pixelHeight,
                   float size, Shape shape, float keptU=1.f, float keptV=1.f) {
 if(size<=0.f || !pixelWidth || !pixelHeight)return false;
 const float du=u-gazeU, dv=v-gazeV;
 const float halfHeight=.5f*float(pixelHeight)*size/keptV;
 if(shape==Shape::Ellipse) {
  const float bound=.5f*size;
  const float eu=du*keptU, ev=dv*keptV;
  return eu*eu+ev*ev<=bound*bound;
 }
 const float dx=du*float(pixelWidth),dy=dv*float(pixelHeight);
 if(shape==Shape::Square)return std::max(std::fabs(dx),std::fabs(dy))<=halfHeight;
 return dx*dx+dy*dy<=halfHeight*halfHeight;
}
inline float tileUV(uint32_t tileIndex, uint32_t tileSize, uint32_t imageSize) {
 const uint32_t start=tileIndex*tileSize;
 const uint32_t end=start+std::min(tileSize,imageSize-start);
 return (.5f*(float(start)+float(end)))/float(imageSize);
}
namespace detail {
// Each supported shape intersects a row in one contiguous interval. Search
// its two edges with the SAME float predicate used by the debug outline.
// This avoids sqrt/rounding approximations that could change boundary texels.
template<class Contains>
inline std::pair<uint32_t,uint32_t> rowSpan(uint32_t width, uint32_t nearest,
                                          Contains contains) {
 if(!contains(nearest))return {0,0};
 uint32_t lo=0,hi=nearest;
 while(lo<hi){
  const uint32_t mid=lo+(hi-lo)/2;
  if(contains(mid))hi=mid;else lo=mid+1;
 }
 const uint32_t begin=lo;
 lo=nearest+1;hi=width;
 while(lo<hi){
  const uint32_t mid=lo+(hi-lo)/2;
  if(contains(mid))lo=mid+1;else hi=mid;
 }
 return {begin,lo};
}
}
inline Pattern makeGaze(uint32_t pixelWidth, uint32_t pixelHeight,
                        uint32_t tileWidth, uint32_t tileHeight,
                        float gazeU, float gazeV, float centerSize, float middleSize,
                        uint8_t middleRate=rate(2,2), uint8_t outerRate=rate(4,4),
                        Shape shape=Shape::Circle, float keptU=1.f, float keptV=1.f) {
 if (!(keptU>0.f && keptU<=1.f && keptV>0.f && keptV<=1.f))
   throw std::invalid_argument("invalid kept eye fraction");
 if (!pixelWidth || !pixelHeight || !tileWidth || !tileHeight ||
     !std::isfinite(centerSize) || !std::isfinite(middleSize) ||
     centerSize<0.f || middleSize<centerSize || middleSize>1.f ||
     !std::isfinite(gazeU) || !std::isfinite(gazeV) ||
     gazeU<0.f || gazeU>1.f || gazeV<0.f || gazeV>1.f)
   throw std::invalid_argument("invalid foveation dimensions/size/gaze");
 Pattern p{1+(pixelWidth-1)/tileWidth,
           1+(pixelHeight-1)/tileHeight, tileWidth, tileHeight, {}};
 p.texels.assign(size_t(p.width)*p.height,outerRate);
 if(middleSize==0.f)return p;
 std::vector<float> columns(p.width);
 for(uint32_t x=0;x<p.width;++x)columns[x]=tileUV(x,tileWidth,pixelWidth);
 const auto after=std::lower_bound(columns.begin(),columns.end(),gazeU);
 uint32_t nearest=after==columns.end()?p.width-1:uint32_t(after-columns.begin());
 if(nearest && std::fabs(columns[nearest-1]-gazeU)<std::fabs(columns[nearest]-gazeU))--nearest;
 for(uint32_t y=0;y<p.height;++y){
  const float v=tileUV(y,tileHeight,pixelHeight);
  auto fillRegion=[&](float size,uint8_t value){
   if(size==0.f)return;
   const auto [begin,end]=detail::rowSpan(p.width,nearest,[&](uint32_t x){
    return inside(columns[x],v,gazeU,gazeV,pixelWidth,pixelHeight,size,shape,keptU,keptV);
   });
   auto row=p.texels.begin()+size_t(y)*p.width;
   std::fill(row+begin,row+end,value);
  };
  fillRegion(middleSize,middleRate);
  fillRegion(centerSize,rate(1,1));
 }
 return p;
}
inline Pattern make(uint32_t pixelWidth, uint32_t pixelHeight,
                    uint32_t tileWidth = 16, uint32_t tileHeight = 16,
                    float centerSize = 0.20f, float middleSize = 0.42f,
                    Shape shape=Shape::Circle) {
 return makeGaze(pixelWidth,pixelHeight,tileWidth,tileHeight,
                 .5f,.5f,centerSize,middleSize,rate(2,2),rate(4,4),shape);
}
// M8.32: shading-rate texels for passes whose eye is unknown. Each tile keeps
// the FINER rate of the two eyes on each axis, so neither eye loses detail.
inline uint8_t finerRate(uint8_t a, uint8_t b) {
 return static_cast<uint8_t>(std::min(a&3,b&3) | (std::min((a>>2)&3,(b>>2)&3)<<2));
}
inline void combineFiner(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right,
                         std::vector<uint8_t>& out) {
 if(left.size()!=right.size())throw std::invalid_argument("combined eye patterns differ in size");
 out.resize(left.size());
 for(size_t i=0;i<left.size();++i)out[i]=finerRate(left[i],right[i]);
}
} // namespace beamng_ffr
