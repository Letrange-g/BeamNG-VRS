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
//   on target  the frame rate meets the headset's rate; the rest is headroom,
//   GPU        the GPU alone needs most of the budget,
//   CPU        the game's own work needs most of the budget while the GPU has room,
//   runtime    neither does, yet frames are slow: the runtime or compositor holds
//              the game back (reprojection at half rate, streaming such as
//              Virtual Desktop or Air Link, or another app).

namespace frame_pacing {

struct Averages {
    double intervalMs = 0; // between xrEndFrame calls
    double waitMs = 0;     // blocked in xrWaitFrame
    double gpuMs = 0;      // game queue busy
    double periodMs = 0;   // headset display period; 0 = unknown
};

// Fraction of the budget above which a side counts as the limit.
constexpr double kLimitShare = 0.85;
// Frames this close to the display period count as on target (timer jitter).
constexpr double kOnTargetSlack = 1.05;

inline const char* verdict(const Averages& a) {
    const double budget = a.periodMs > 0 ? a.periodMs : a.intervalMs;
    if (budget <= 0) return "unknown";

    if (a.periodMs > 0 && a.intervalMs <= a.periodMs * kOnTargetSlack) return "on target";

    const double cpuMs = a.intervalMs > a.waitMs ? a.intervalMs - a.waitMs : 0;
    if (a.gpuMs >= budget * kLimitShare) return "GPU-bound";
    if (cpuMs >= budget * kLimitShare) return "CPU-bound";
    return "runtime-bound";
}

} // namespace frame_pacing
