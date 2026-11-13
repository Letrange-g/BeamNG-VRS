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
 double sharpWidth = 0.35;  // window share of each eye's full tangent width
 double sharpHeight = 0.35; // and height
 double feather = 0.30;     // edge blend, share of the window radius
 double deadZone = 0.03;    // tangent units (~1.7 deg) the gaze moves before the window follows
 uint32_t middleWidth = 0, middleHeight = 0; // left wide view (0 = from wideScale); the right one is 2 rows taller
 double wideScale = 0.25;   // wide view size as a share of the headset's per-eye resolution
 double middleFov = 0;      // horizontal field of view of the middle view, degrees (BeamNG's fov is
                            // horizontal); 0 = auto, from the headset (autoMiddleFov)
};

// WideResolution: unless both sizes are set, the wide views take wideScale of the
// headset's recommended per-eye size (same aspect as the eye), kept within 256-4096
// pixels and even. Without a headset size yet, 1280 x 880 as before.
// The layer finds each wide view's render target by its size alone, so that size must not
// match a target the game makes for the focus views: their own size, the internal size
// before FSR upscaling (renderScale) and the half/quarter/eighth chains built from both.
// Matt's log (FocusWidth=FocusHeight=WideResolution=0.25): the focus views were 1298x1042
// like the left wide view, so the layer took a focus target and the left eye looked zoomed.
inline bool wideSizeClashes(uint32_t w, uint32_t h, uint32_t focusW, uint32_t focusH, double renderScale) {
 for(const double scale : {1.0, renderScale})
  for(const double k : {1.0, 0.5, 0.25, 0.125}) {
   const double cw = focusW * scale * k, ch = focusH * scale * k;
   const auto within = [](double a, double b) { return std::abs(a - b) <= 4.0; };
   // The right wide view is two rows taller than the left.
   if(within(w, cw) && (within(h, ch) || within(h + 2.0, ch))) return true;
  }
 return false;
}

inline uint32_t scaledSize(uint32_t size, double share);

// Sizes the wide views from WideResolution unless WideWidth/WideHeight fixed them, then
// widens them in 8-pixel steps until no focus-view target can share their size.
// Returns true when the size had to be changed for that.
// keepWidth/keepHeight: share of the eye the wide views still cover when [Scissor] cuts
// the edges (wideKeep), so the cut-off part costs no wide-view pixels either.
inline bool resolveWideSize(Settings& s, uint32_t fullWidth, uint32_t fullHeight, double renderScale = 0.5,
                            double keepWidth = 1.0, double keepHeight = 1.0) {
 if(!(s.middleWidth && s.middleHeight)) {
  if(!fullWidth || !fullHeight) { s.middleWidth = 1280; s.middleHeight = 880; }
  else {
   const auto size = [&](uint32_t full, double keep) {
    const double v = std::min(4096.0, std::max(256.0, double(full) * s.wideScale * std::clamp(keep, 0.02, 1.0)));
    return uint32_t(std::lround(v / 2.0) * 2);
   };
   s.middleWidth = size(fullWidth, keepWidth); s.middleHeight = size(fullHeight, keepHeight);
  }
 }
 if(!fullWidth || !fullHeight) return false;
 const uint32_t focusW = scaledSize(fullWidth, s.sharpWidth), focusH = scaledSize(fullHeight, s.sharpHeight);
 bool moved = false;
 for(int i = 0; i < 64 && wideSizeClashes(s.middleWidth, s.middleHeight, focusW, focusH, renderScale); ++i) {
  s.middleWidth = s.middleWidth + 8 <= 4096 ? s.middleWidth + 8 : s.middleWidth - 8;
  moved = true;
 }
 return moved;
}

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

// [Scissor] in quad views: shares of each eye's tangent width/height cut from its edges.
// The composed eye covers only the rest (the headset shows black beyond it, as the
// M8.44 crop does in vrs mode), so the wide views and the composition skip that part.
struct Cut { double top = 0, bottom = 0, left = 0, right = 0; };

// Image rows and columns are linear in the tangent of the view angle.
inline Tangents cutTangents(const Tangents& f, const Cut& c) {
 const double w = f.right - f.left, h = f.up - f.down;
 return {f.left + c.left * w, f.right - c.right * w, f.up - c.top * h, f.down + c.bottom * h};
}

// The focus window keeps its share of the FULL eye (the same angle and the same render
// size as without the cut); as a share of the kept part that is larger.
inline double keptShare(double share, double kept) { return std::clamp(share / std::max(kept, 0.05), 0.05, 1.0); }

// A wide view's frustum is symmetric around the eye's direction, so only the cut both
// opposite edges share shrinks it: the larger of the two eyes' kept shares.
inline void wideKeep(const Cut cut[2], double& keepWidth, double& keepHeight) {
 keepWidth = keepHeight = 0;
 for(int e = 0; e < 2; ++e) {
  keepWidth = std::max(keepWidth, 1.0 - 2.0 * std::min(cut[e].left, cut[e].right));
  keepHeight = std::max(keepHeight, 1.0 - 2.0 * std::min(cut[e].top, cut[e].bottom));
 }
 keepWidth = std::clamp(keepWidth, 0.02, 1.0); keepHeight = std::clamp(keepHeight, 0.02, 1.0);
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
 const double fov = std::clamp(s.middleFov > 0 ? s.middleFov : 115.0, 30.0, 160.0) * 3.14159265358979323846 / 180.0;
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

// MiddleFov=auto: the smallest horizontal field of view (degrees) whose middle view,
// at the given resolution and facing `middle`, covers both eyes' full fields of view,
// plus `margin` (share of the tangent extent). Corners are enough: a rectangle in one
// eye's tangent plane stays a quadrilateral in the middle's, so extremes are corners.
inline double autoMiddleFov(const Tangents full[2], const Quat eyes[2], const Quat& middle,
                            uint32_t width, uint32_t height, double margin = 0.03) {
 double needX = 0, needY = 0;
 for(int e = 0; e < 2; ++e) {
  float rows[3][4]{};
  relativeRotation(middle, eyes[e], rows);
  for(double tx : {full[e].left, full[e].right})
   for(double ty : {full[e].down, full[e].up}) {
    const double d[3]{tx, ty, -1};
    double m[3]{};
    for(int r = 0; r < 3; ++r) m[r] = rows[r][0] * d[0] + rows[r][1] * d[1] + rows[r][2] * d[2];
    if(m[2] > -1e-3) return 150; // a corner at or behind 90 degrees: widest allowed
    needX = std::max(needX, std::fabs(m[0] / -m[2]));
    needY = std::max(needY, std::fabs(m[1] / -m[2]));
   }
 }
 const double aspect = double(std::max<uint32_t>(1, width)) / double(std::max<uint32_t>(1, height));
 const double halfX = std::max(needX, needY * aspect) * (1 + margin);
 return std::clamp(2 * std::atan(halfX) * 180.0 / 3.14159265358979323846, 30.0, 150.0);
}

} // namespace beamng_triple
