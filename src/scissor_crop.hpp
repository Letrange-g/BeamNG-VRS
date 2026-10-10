#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace beamng_scissor {
struct Rect { uint32_t x, y, width, height; };
struct Margins { double top, bottom, left, right; };
struct Crop {
 Rect keep;
 std::array<Rect,4> outside;
};
inline uint32_t edge(uint32_t size, double fraction) {
 return static_cast<uint32_t>(size * (std::isfinite(fraction) ? std::clamp(fraction,0.0,0.49) : 0.0));
}
inline Crop make(uint32_t width, uint32_t height, Margins margins) {
 const auto top=edge(height,margins.top), bottom=edge(height,margins.bottom);
 const auto left=edge(width,margins.left), right=edge(width,margins.right);
 const Rect keep{left,top,width-left-right,height-top-bottom};
 // Top/bottom cover the full width. Side bars occupy only the kept rows,
 // so the four borders partition the discarded pixels without overlap.
 return {keep,{{{0,0,width,top},{0,height-bottom,width,bottom},
               {0,top,left,keep.height},{width-right,top,right,keep.height}}}};
}
}
