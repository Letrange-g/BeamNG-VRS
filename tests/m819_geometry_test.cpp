#include "m819_visibility_geometry.hpp"
#include "m818_stencil_mask.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using beamng_visibility_geometry::Point;
int main(){
 std::vector<Point> cloud={{0,0},{0,1},{1,0},{1,1},{.3f,.3f},{.8f,.3f},{0,0}};
 std::vector<Point> hull;
 assert(beamng_visibility_geometry::convexHull(cloud,hull));
 assert(hull.size()==4);
 // The outer polygon must conservatively contain all input triangle vertices.
 for(const auto& p:cloud){
  for(size_t i=0;i<hull.size();++i){
   const auto& a=hull[i];const auto& b=hull[(i+1)%hull.size()];
   assert(beamng_visibility_geometry::cross(a,b,p)>=-1e-5f);
  }
 }
 std::vector<beamng_stencil::Point> points;
 for(auto p:hull)points.push_back({p.x,p.y});
 std::vector<beamng_stencil::Strip> strips;
 beamng_stencil::Fov f{0,1,0,1};
 for(auto [w,h]:{std::pair<unsigned,unsigned>{5190,4168},{3892,3126},{2595,2084}}){
  assert(beamng_stencil::makeStrips(points.data(),points.size(),f,w,h,strips,48,0.025f));
  assert(strips.size()==48);
  for(const auto& r:strips)assert(r.x+r.width<=w && r.y+r.height<=h);
 }
 std::vector<Point> bad={{0,0},{NAN,1},{1,1}};
 assert(!beamng_visibility_geometry::convexHull(bad,hull));
 std::cout<<"M8.19 conservative visible-mesh hull and 3 resolution tests passed\n";
}
