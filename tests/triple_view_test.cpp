// M8.54 triple view: window placement, gaze dead zone, coverage and rotation.
#undef NDEBUG
#include "triple_view.hpp"
#include <cassert>
#include <cmath>
using namespace beamng_triple;
static bool near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }
int main() {
 const Tangents full{-1.2, 1.0, 1.1, -1.3};
 // Centred window keeps its share.
 auto w = window(full, 0.4, 0.5, 0, 0);
 assert(near(w.right - w.left, 2.2 * 0.4) && near(w.up - w.down, 2.4 * 0.5));
 assert(near((w.left + w.right) / 2, 0) && near((w.up + w.down) / 2, 0));
 // Gaze at the edge: the window slides back inside.
 w = window(full, 0.4, 0.4, 5, -5);
 assert(near(w.right, full.right) && near(w.down, full.down));
 assert(w.left > full.left && w.up < full.up);
 // Non-finite gaze recentres; share 1 is the full field of view.
 w = window(full, 1, 1, NAN, INFINITY);
 assert(near(w.left, full.left) && near(w.right, full.right) && near(w.up, full.up) && near(w.down, full.down));
 assert(scaledSize(2000, 0.4) == 800 && scaledSize(10, 0.4) == 16);

 // Dead zone: small moves are ignored, a move by either eye moves both.
 GazeFilter g;
 double gx[2]{0.1, 0.12}, gy[2]{0, 0};
 g.update(gx, gy, 3, 0.03, 0);
 assert(g.seeded && near(g.cx[0], 0.1) && near(g.cx[1], 0.12));
 double sx[2]{0.11, 0.13};
 g.update(sx, gy, 3, 0.03, 10);
 assert(near(g.cx[0], 0.1));
 double bx[2]{0.11, 0.2};
 g.update(bx, gy, 3, 0.03, 20);
 assert(near(g.cx[0], 0.11) && near(g.cx[1], 0.2));
 // One eye missing: hold, then recentre after returnMs.
 g.update(bx, gy, 1, 0.03, 300);
 assert(near(g.cx[1], 0.2));
 g.update(bx, gy, 0, 0.03, 600);
 assert(!g.seeded && near(g.cx[0], 0) && near(g.cy[1], 0));

 // Coverage of the middle view.
 Settings s; s.middleFov = 90; s.middleWidth = 1000; s.middleHeight = 1000;
 double hx = 0, hy = 0; middleHalf(s, hx, hy);
 assert(near(hx, 1, 1e-9) && near(hy, 1, 1e-9));
 s.middleWidth = 2000; middleHalf(s, hx, hy); // fov is horizontal: the height follows the aspect
 assert(near(hx, 1, 1e-9) && near(hy, 0.5, 1e-9));
 s.middleWidth = 1000; middleHalf(s, hx, hy);
 assert(near(coverage({-1, 1, 1, -1}, hx, hy), 1));
 assert(near(coverage({-2, 2, 1, -1}, hx, hy), 0.5));

 // Auto FOV: two parallel eyes, each -1.4..1.0 wide (mirrored) and +-1 high, at 16:9.
 {
  const Tangents eyes[2]{{-1.4, 1.0, 1.0, -1.0}, {-1.0, 1.4, 1.0, -1.0}};
  const Quat q[2]{};
  const double fov = autoMiddleFov(eyes, q, q[0], 1600, 900, 0);
  // Height needs 1.0 * 16/9 = 1.778 > width's 1.4: the height decides.
  assert(near(std::tan(fov / 2 * 3.14159265358979323846 / 180), 16.0 / 9.0, 1e-6));
  // Square target: the width decides; a 3% margin widens it.
  assert(near(std::tan(autoMiddleFov(eyes, q, q[0], 1000, 1000, 0) / 2 * 3.14159265358979323846 / 180), 1.4, 1e-6));
  assert(near(std::tan(autoMiddleFov(eyes, q, q[0], 1000, 1000) / 2 * 3.14159265358979323846 / 180), 1.4 * 1.03, 1e-6));
 }
 // Same orientation: identity. Eye yawed 90 deg left: its forward (-z) is the middle's -x.
 float rows[3][4]{};
 relativeRotation({}, {}, rows);
 for(int r = 0; r < 3; ++r) for(int c = 0; c < 3; ++c) assert(near(rows[r][c], r == c ? 1 : 0, 1e-6));
 const double h = std::sqrt(0.5);
 relativeRotation({}, {0, h, 0, h}, rows); // +90 deg about y (turn left)
 const double f[3]{0, 0, -1};
 double m[3]{};
 for(int r = 0; r < 3; ++r) m[r] = rows[r][0] * f[0] + rows[r][1] * f[1] + rows[r][2] * f[2];
 assert(near(m[0], -1, 1e-6) && near(m[1], 0, 1e-6) && near(m[2], 0, 1e-6));
 {Settings w; resolveWideSize(w, 5190, 4168); assert(w.middleWidth == 1298 && w.middleHeight == 1042);}
 {Settings w; w.wideScale = 0.5; resolveWideSize(w, 5190, 4168); assert(w.middleWidth == 2596 && w.middleHeight == 2084);}
 {Settings w; w.wideScale = 0.1; resolveWideSize(w, 1000, 900); assert(w.middleWidth == 256 && w.middleHeight == 256);}
 {Settings w; w.middleWidth = 1280; w.middleHeight = 880; resolveWideSize(w, 5190, 4168); assert(w.middleWidth == 1280 && w.middleHeight == 880);}
 {Settings w; w.sharpWidth = 0.25; w.sharpHeight = 0.25; assert(resolveWideSize(w, 5190, 4168));
  assert(w.middleWidth != 1298 && !wideSizeClashes(w.middleWidth, w.middleHeight, 1298, 1042, 0.5));}
 {Settings w; assert(!resolveWideSize(w, 5190, 4168)); assert(w.middleWidth == 1298);}
 {Settings w; w.sharpWidth = 0.5; w.sharpHeight = 0.5; resolveWideSize(w, 5190, 4168, 1.0); // 2595x2084 at half: 1297.5x1042
  assert(!wideSizeClashes(w.middleWidth, w.middleHeight, 2595, 2084, 1.0));}
 {Settings w; resolveWideSize(w, 0, 0); assert(w.middleWidth == 1280 && w.middleHeight == 880);}
 return 0;
}
