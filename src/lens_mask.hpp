#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// M8.34: lens-corner mask. The kept area is an ellipse centered in the eye
// image with diameters size*width and size*height. size=1 touches the four
// image edges and removes about 21% of the pixels; size>=sqrt(2) removes none.
namespace beamng_lens {
struct Rect { uint32_t x, y, width, height; };
constexpr double minSize=0.5, maxSize=1.5;

// Rectangles covering only pixels that lie entirely outside the ellipse.
// Rows are grouped in bands; each band keeps the widest span any of its rows
// needs, so the mask never covers a pixel the ellipse touches.
inline std::vector<Rect> outside(uint32_t width, uint32_t height, double size, uint32_t band=16) {
 std::vector<Rect> rects;
 if(!width || !height || !band || !std::isfinite(size) || size<minSize || size>maxSize) return rects;
 const double cx=width/2.0, cy=height/2.0, a=size*width/2.0, b=size*height/2.0;
 for(uint32_t y0=0;y0<height;y0+=band){
  const uint32_t y1=std::min(height,y0+band);
  // The band row nearest the center has the widest ellipse extent. Use the
  // pixel edge nearest the center, not its center, to stay conservative.
  const double nearest=std::clamp(cy,double(y0),double(y1));
  const double dy=(nearest-cy)/b;
  uint32_t left=width, right=0; // the whole band is outside unless the ellipse reaches it
  if(dy*dy<1.0){
   const double half=a*std::sqrt(1.0-dy*dy);
   left=uint32_t(std::clamp(std::floor(cx-half),0.0,double(width)));
   right=uint32_t(std::clamp(std::ceil(cx+half),0.0,double(width)));
  }
  auto add=[&](uint32_t x,uint32_t w){
   if(!w)return;
   // Merge with a rectangle ending directly above that spans the same columns.
   for(size_t i=rects.size(),n=0;i-- && n<4;++n){
    auto& r=rects[i];
    if(r.x==x && r.width==w && r.y+r.height==y0){r.height+=y1-y0;return;}
   }
   rects.push_back({x,y0,w,y1-y0});
  };
  if(right<=left){add(0,width);continue;}
  add(0,left);
  add(right,width-right);
 }
 return rects;
}
}
