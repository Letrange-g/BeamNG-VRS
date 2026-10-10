#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>

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
inline bool inside(float u, float v, float gazeU, float gazeV,
                   uint32_t pixelWidth, uint32_t pixelHeight,
                   float size, Shape shape) {
 if(size<=0.f || !pixelWidth || !pixelHeight)return false;
 const float du=u-gazeU, dv=v-gazeV;
 const float halfHeight=.5f*float(pixelHeight)*size;
 if(shape==Shape::Ellipse) {
  const float bound=.5f*size;
  return du*du+dv*dv<=bound*bound;
 }
 const float dx=du*float(pixelWidth),dy=dv*float(pixelHeight);
 if(shape==Shape::Square)return std::max(std::fabs(dx),std::fabs(dy))<=halfHeight;
 return dx*dx+dy*dy<=halfHeight*halfHeight;
}
inline float tileUV(uint32_t tileIndex, uint32_t tileSize, uint32_t imageSize) {
 const uint32_t start=tileIndex*tileSize;
 const uint32_t end=std::min(start+tileSize,imageSize);
 return (.5f*(float(start)+float(end)))/float(imageSize);
}
inline Pattern makeGaze(uint32_t pixelWidth, uint32_t pixelHeight,
                        uint32_t tileWidth, uint32_t tileHeight,
                        float gazeU, float gazeV, float centerSize, float middleSize,
                        uint8_t middleRate=rate(2,2), uint8_t outerRate=rate(4,4),
                        Shape shape=Shape::Circle) {
 if (!pixelWidth || !pixelHeight || !tileWidth || !tileHeight ||
     !std::isfinite(centerSize) || !std::isfinite(middleSize) ||
     centerSize<0.f || middleSize<centerSize || middleSize>1.f ||
     !std::isfinite(gazeU) || !std::isfinite(gazeV) ||
     gazeU<0.f || gazeU>1.f || gazeV<0.f || gazeV>1.f)
   throw std::invalid_argument("invalid foveation dimensions/size/gaze");
 Pattern p{(pixelWidth + tileWidth - 1) / tileWidth,
           (pixelHeight + tileHeight - 1) / tileHeight, tileWidth, tileHeight, {}};
 p.texels.resize(size_t(p.width)*p.height);
 for(uint32_t y=0;y<p.height;++y)for(uint32_t x=0;x<p.width;++x){
  const float u=tileUV(x,tileWidth,pixelWidth);
  const float v=tileUV(y,tileHeight,pixelHeight);
  p.texels[size_t(y)*p.width+x]=inside(u,v,gazeU,gazeV,pixelWidth,pixelHeight,centerSize,shape)?rate(1,1):
    inside(u,v,gazeU,gazeV,pixelWidth,pixelHeight,middleSize,shape)?middleRate:outerRate;
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
} // namespace beamng_ffr
