#pragma once
// M8.19: Conservative outline from XR_KHR_visibility_mask VISIBLE_TRIANGLE_MESH.
// Convex hull can *include* extra hidden pixels, but never intentionally removes
// an area included in the runtime's visible triangle mesh.
#include <algorithm>
#include <cmath>
#include <vector>
namespace beamng_visibility_geometry {
struct Point { float x=0,y=0; };
inline float cross(const Point& a,const Point& b,const Point& c) {
 return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
inline bool convexHull(const std::vector<Point>& vertices, std::vector<Point>& hull,
                       size_t maxOutput=512) {
 hull.clear();
 if(vertices.size()<3 || vertices.size()>8192)return false;
 std::vector<Point> points=vertices;
 for(const Point& p:points)if(!std::isfinite(p.x)||!std::isfinite(p.y))return false;
 std::sort(points.begin(),points.end(),[](const Point& a,const Point& b){
  return a.x<b.x||(a.x==b.x&&a.y<b.y);
 });
 points.erase(std::unique(points.begin(),points.end(),[](const Point& a,const Point& b){
  return a.x==b.x&&a.y==b.y;
 }),points.end());
 if(points.size()<3)return false;
 std::vector<Point> ring;
 ring.reserve(points.size()*2);
 for(const auto& p:points){
  while(ring.size()>=2 && cross(ring[ring.size()-2],ring.back(),p)<=0.f)ring.pop_back();
  ring.push_back(p);
 }
 const size_t lower=ring.size();
 for(size_t i=points.size()-1;i-->0;){
  const Point& p=points[i];
  while(ring.size()>lower && cross(ring[ring.size()-2],ring.back(),p)<=0.f)ring.pop_back();
  ring.push_back(p);
 }
 if(ring.size()>=2)ring.pop_back(); // duplicate first point
 if(ring.size()<3 || ring.size()>maxOutput)return false; // fail-open on complex hull
 hull=std::move(ring);
 return true;
}
} // namespace beamng_visibility_geometry
