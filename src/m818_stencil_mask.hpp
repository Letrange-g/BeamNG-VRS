#pragma once
// Conservative scanline approximation of XR_KHR_visibility_mask's LINE_LOOP.
// OpenXR vertices are on the view-space z=-1 plane; projection via tan(FOV)
// must precede the Vulkan scissor/stencil mapping. No graphics API dependencies.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace beamng_stencil {
struct Point { float x=0, y=0; };
struct Fov { float left=0, right=0, down=0, up=0; }; // tangents
struct Strip { uint32_t x=0, y=0, width=0, height=0; };
inline bool makeStrips(const Point* points, size_t count, const Fov& f,
                      uint32_t width,uint32_t height,std::vector<Strip>& strips,
                      uint32_t bands=48,float safetyMargin=0.022f) {
 strips.clear();
 if(!points || count<3 || count>512 || !width || !height ||
    !std::isfinite(f.left) || !std::isfinite(f.right) ||
    !std::isfinite(f.up) || !std::isfinite(f.down) ||
    f.right<=f.left || f.up<=f.down || bands<1 || bands>256)return false;
 std::vector<Point> projected(count);
 for(size_t i=0;i<count;++i){
  const auto p=points[i];
  if(!std::isfinite(p.x)||!std::isfinite(p.y))return false;
  projected[i]={((p.x-f.left)/(f.right-f.left))*float(width),
                ((f.up-p.y)/(f.up-f.down))*float(height)};
 }
 const int32_t padding=std::max<int32_t>(3, int32_t(float(width)*std::clamp(safetyMargin,0.f,.10f)));
 for(uint32_t b=0;b<bands;++b){
  uint32_t y0=uint32_t(uint64_t(b)*height/bands),y1=uint32_t(uint64_t(b+1)*height/bands);
  if(y1<=y0)continue;
  float minX=float(width),maxX=0;bool any=false;
  // Include edge extrema at every interior polygon vertex, not just strip center.
  std::vector<float> ys={float(y0)+0.01f, (float(y0)+float(y1))*.5f,float(y1)-0.01f};
  for(const auto& p:projected)if(p.y>float(y0) && p.y<float(y1)) ys.push_back(p.y);
  for(float y:ys){
   std::vector<float> hits;
   for(size_t i=0;i<count;++i){
    const Point& a=projected[i];const Point& c=projected[(i+1)%count];
    if((a.y<=y && c.y>y)||(c.y<=y && a.y>y))
     hits.push_back(a.x+(y-a.y)*(c.x-a.x)/(c.y-a.y));
   }
   // Conservative union of all visible intervals; do not clip any interior island.
   if(hits.size()>=2){
    auto ex=std::minmax_element(hits.begin(),hits.end());
    minX=std::min(minX,*ex.first);maxX=std::max(maxX,*ex.second);any=true;
   }
  }
  if(!any){ // Visibility entirely absent in this band: do not clip it (fail open).
   strips.push_back({0,y0,width,y1-y0});continue;
  }
  int64_t x0=std::clamp<int64_t>(int64_t(std::floor(minX))-padding,0,width);
  int64_t x1=std::clamp<int64_t>(int64_t(std::ceil(maxX))+padding,0,width);
  if(x1<=x0){strips.push_back({0,y0,width,y1-y0});continue;}
  strips.push_back({uint32_t(x0),y0,uint32_t(x1-x0),y1-y0});
 }
 return strips.size()>0;
}
}
