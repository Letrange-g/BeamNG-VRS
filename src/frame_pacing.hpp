#pragma once

// M8.52: what limits the VR frame rate, for the M8.47 log line.
//
// Each frame the OpenXR layer measures, on the game's render thread:
//   interval  time between two xrEndFrame calls (the frame rate actually reached),
//   wait      time spent blocked inside xrWaitFrame (the runtime holding the game back),
// and the D3D12 probe measures gpu, the time the game's queue was busy.
// interval - wait is the time the game was working (its CPU side, plus any wait on
// its own GPU work). period is the headset's display period from xrWaitFrame.
//
// The verdict compares each side with the headset's frame budget (period):
//   on target   the frame rate meets the headset's rate; the rest is headroom,
//   GPU         the GPU alone needs most of the budget,
//   FPS-capped  (M8.53) the frame rate is flat below the headset's rate while the GPU
//               has room: a frame limiter, usually BeamNG's own FPS limit, holds it
//               there. A user's limit set to 90 ran at a steady 75 until it was set to
//               120 and back to 90; the CPU-bound verdict pointed the wrong way,
//   CPU         the game's own work needs most of the budget while the GPU has room,
//   runtime     neither does, yet frames are slow: the runtime or compositor holds
//               the game back (reprojection at half rate, streaming such as
//               Virtual Desktop or Air Link, or another app).

#include <cmath>

namespace frame_pacing {

struct Averages {
    double intervalMs = 0;    // between xrEndFrame calls
    double waitMs = 0;        // blocked in xrWaitFrame
    double gpuMs = 0;         // game queue busy
    double periodMs = 0;      // headset display period; 0 = unknown
    double intervalSdMs = -1; // standard deviation of the interval; negative = unknown
};

// Fraction of the budget above which a side counts as the limit.
constexpr double kLimitShare = 0.85;
// Frames this close to the display period count as on target (timer jitter).
constexpr double kOnTargetSlack = 1.05;
// A frame limiter holds the interval this steady (standard deviation as a share of
// the interval). Real CPU-bound driving varies far more (physics, traffic, streaming),
// and one hitch in the window lifts the spread above it.
constexpr double kCappedSpread = 0.05;
// Intervals this close to 2x, 3x... the display period are the runtime's half or
// third rate (reprojection), which is just as steady: left to the runtime verdict.
constexpr double kMultipleSlack = 0.05;

// Steady frame rate below the headset's while the GPU has room (see M8.53 above).
inline bool capped(const Averages& a) {
    if (a.periodMs <= 0 || a.intervalSdMs < 0) return false;
    if (a.intervalMs <= a.periodMs * kOnTargetSlack) return false;
    if (a.gpuMs >= a.periodMs * kLimitShare) return false;
    if (a.intervalSdMs > a.intervalMs * kCappedSpread) return false;
    const double multiple = std::round(a.intervalMs / a.periodMs);
    if (multiple >= 2 && std::fabs(a.intervalMs - multiple * a.periodMs) <= a.periodMs * kMultipleSlack)
        return false;
    return true;
}

inline const char* verdict(const Averages& a) {
    const double budget = a.periodMs > 0 ? a.periodMs : a.intervalMs;
    if (budget <= 0) return "unknown";

    if (a.periodMs > 0 && a.intervalMs <= a.periodMs * kOnTargetSlack) return "on target";

    const double cpuMs = a.intervalMs > a.waitMs ? a.intervalMs - a.waitMs : 0;
    if (a.gpuMs >= budget * kLimitShare) return "GPU-bound";
    if (capped(a)) return "FPS-capped";
    if (cpuMs >= budget * kLimitShare) return "CPU-bound";
    return "runtime-bound";
}

} // namespace frame_pacing
