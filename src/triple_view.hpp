#pragma once

// M8.54 triple view: the game's two eye views shrink to a small sharp window
// around the gaze, and one extra wide, low-resolution "middle" view (a Lua
// render view, one camera between the eyes) fills the rest of each eye.
// The OpenXR layer narrows each eye's field of view in xrLocateViews, and at
// xrEndFrame composes a full-field-of-view image per eye from the middle view
// and the sharp window, which it submits instead of the game's eye images.
//
// This header holds the portable parts: the window placement, the gaze
// dead-zone and the shader constants. Angles are handled as tangents, in
// which image rows and columns are linear (left < right, down < up).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace beamng_triple {

struct Tangents { double left = -1, right = 1, up = 1, down = -1; };

inline Tangents fromAngles(double left, double right, double up, double down) {
 return {std::tan(left), std::tan(right), std::tan(up), std::tan(down)};
}

struct Settings {
 bool enabled = false;
 bool middle = true;        // false: black around the sharp window, no middle view.
 bool gaze = true;          // false: the window stays straight ahead.
 bool roundShape = true;    // ellipse inside the window; false: the whole rectangle.
 double sharpWidth = 0.40;  // window share of each eye's full tangent width
 double sharpHeight = 0.40; // and height
 double feather = 0.15;     // edge blend, share of the window radius
 double deadZone = 0.03;    // tangent units (~1.7 deg) the gaze moves before the window follows
 uint32_t middleWidth = 1600, middleHeight = 1152;
 double middleFov = 115;    // horizontal field of view of the middle view, degrees (BeamNG's fov is horizontal)
};

// A window of the given share of `full`, centred on (cx, cy) and slid back
// inside `full` where it would cross an edge.
inline Tangents window(const Tangents& full, double shareX, double shareY, double cx, double cy) {
 shareX = std::clamp(shareX, 0.05, 1.0);
 shareY = std::clamp(shareY, 0.05, 1.0);
 const double halfW = (full.right - full.left) * shareX / 2, halfH = (full.up - full.down) * shareY / 2;
 if(!std::isfinite(cx)) cx = 0;
 if(!std::isfinite(cy)) cy = 0;
 cx = std::clamp(cx, full.left + halfW, full.right - halfW);
 cy = std::clamp(cy, full.down + halfH, full.up - halfH);
 return {cx - halfW, cx + halfW, cy + halfH, cy - halfH};
}

inline uint32_t scaledSize(uint32_t size, double share) {
 return std::max<uint32_t>(16, uint32_t(std::lround(size * std::clamp(share, 0.05, 1.0))));
}

// Keeps the window still against eye-tracker noise: both eyes' centres jump to
// the new gaze together once either eye's gaze has moved past the dead zone.
// Saccades hide the jump. Without gaze for `returnMs`, the window recentres.
struct GazeFilter {
 double cx[2]{0, 0}, cy[2]{0, 0};
 bool seeded = false;
 uint64_t lastValidMs = 0;
 void update(const double gx[2], const double gy[2], uint32_t validMask, double deadZone, uint64_t nowMs,
             uint64_t returnMs = 500) {
  if((validMask & 3u) == 3u) {
   lastValidMs = nowMs;
   bool move = !seeded;
   for(int i = 0; i < 2; ++i)
    if(std::hypot(gx[i] - cx[i], gy[i] - cy[i]) > deadZone) move = true;
   if(move) {
    for(int i = 0; i < 2; ++i) { cx[i] = gx[i]; cy[i] = gy[i]; }
    seeded = true;
   }
  } else if(seeded && nowMs - lastValidMs > returnMs) {
   for(int i = 0; i < 2; ++i) { cx[i] = 0; cy[i] = 0; }
   seeded = false;
  }
 }
};

// Tangent half-extents of the middle view (symmetric frustum). Like Torque3D, BeamNG
// takes a render view's fov as the horizontal angle and derives the vertical one from
// the resolution (M8.54 first test: read as vertical, the periphery looked magnified).
inline void middleHalf(const Settings& s, double& halfX, double& halfY) {
 const double fov = std::clamp(s.middleFov, 30.0, 160.0) * 3.14159265358979323846 / 180.0;
 halfX = std::tan(fov / 2);
 halfY = halfX * double(std::max<uint32_t>(1, s.middleHeight)) / double(std::max<uint32_t>(1, s.middleWidth));
}

// Share of the eye's full field of view (by tangent area) that the middle view
// covers, for eyes looking the same way as the middle view. A value below 1
// means black edges: raise MiddleFov or the width-to-height ratio.
inline double coverage(const Tangents& full, double halfX, double halfY) {
 const double w = full.right - full.left, h = full.up - full.down;
 if(w <= 0 || h <= 0) return 0;
 const double x = std::max(0.0, std::min(full.right, halfX) - std::max(full.left, -halfX));
 const double y = std::max(0.0, std::min(full.up, halfY) - std::max(full.down, -halfY));
 return (x * y) / (w * h);
}

// Root constants of the composition shader (32 floats = 8 float4 registers).
struct Constants {
 float out[4];    // full eye field of view: left, right, up, down tangents
 float win[4];    // sharp window: left, right, up, down tangents
 float rect[4];   // sharp window in its texture: u0, v0, u1, v1
 float middle[4]; // half-extent x, half-extent y, feather, middle on (1/0)
 float rot0[4];   // rows of the rotation from this eye's view space to the middle view's
 float rot1[4];
 float rot2[4];
 float flags[4];  // round shape, linearize sharp, linearize middle, 0
};
static_assert(sizeof(Constants) == 32 * sizeof(float));

struct Quat { double x = 0, y = 0, z = 0, w = 1; };

// Rotation matrix (rows) taking directions in `eye` space to `middle` space,
// both given as orientations in the same reference space.
inline void relativeRotation(const Quat& middle, const Quat& eye, float rows[3][4]) {
 auto matrix = [](const Quat& q, double m[3][3]) {
  const double x = q.x, y = q.y, z = q.z, w = q.w;
  m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
  m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
  m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
 };
 double a[3][3], b[3][3];
 matrix(middle, a);
 matrix(eye, b);
 // rows = transpose(a) * b
 for(int r = 0; r < 3; ++r) {
  for(int c = 0; c < 3; ++c) {
   double sum = 0;
   for(int k = 0; k < 3; ++k) sum += a[k][r] * b[k][c];
   rows[r][c] = float(sum);
  }
  rows[r][3] = 0;
 }
}

} // namespace beamng_triple
