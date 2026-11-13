#include <cassert>
#include <cstdio>
#include <cstring>
#include "frame_pacing.hpp"

using frame_pacing::Averages;
using frame_pacing::verdict;

static bool is(const Averages& a, const char* expected) { return std::strcmp(verdict(a), expected) == 0; }

int main() {
    // 90 Hz headset (11.11 ms budget).
    // Meets the rate with room to spare.
    assert(is({11.1, 4.0, 5.0, 11.11}, "on target"));
    // Game busy the whole slower frame, with the frame-to-frame spread of real driving.
    assert(is({14.0, 0.5, 6.0, 11.11, 2.5}, "CPU-bound"));
    // Spread unknown: never called capped.
    assert(is({14.0, 0.5, 6.0, 11.11}, "CPU-bound"));
    // M8.53, the user report: FPS limit set to 90 but stuck at a steady 75 (13.33 ms),
    // GPU 5-8 ms. It was a frame limiter, not the CPU.
    assert(is({13.33, 0.5, 6.0, 11.11, 0.3}, "FPS-capped"));
    // Same, with the limiter's sleep landing in xrWaitFrame instead.
    assert(is({13.33, 7.0, 6.0, 11.11, 0.3}, "FPS-capped"));
    // A steady 60 fps cap on a 120 Hz headset is exactly half rate: left to the runtime.
    assert(is({16.67, 10.0, 6.0, 8.33, 0.2}, "runtime-bound"));
    // Steady half rate on 90 Hz (reprojection) stays runtime-bound.
    assert(is({22.2, 15.0, 5.0, 11.11, 0.3}, "runtime-bound"));
    // Steady but GPU-limited stays GPU-bound.
    assert(is({13.33, 0.5, 10.0, 11.11, 0.3}, "GPU-bound"));
    // Steady at the headset's rate is on target.
    assert(is({11.1, 4.0, 5.0, 11.11, 0.1}, "on target"));
    // Unknown display period: no cap verdict without a rate to compare with.
    assert(is({13.33, 0.5, 6.0, 0, 0.3}, "CPU-bound"));
    // GPU needs the whole budget; the runtime holds the game in xrWaitFrame.
    assert(is({22.2, 9.0, 10.5, 11.11}, "GPU-bound"));
    // Both sides fast, yet frames are slow: the runtime throttles (half-rate reprojection).
    assert(is({22.2, 15.0, 5.0, 11.11}, "runtime-bound"));
    // Unknown display period: judged against the frame interval.
    assert(is({14.0, 0.5, 6.0, 0}, "CPU-bound"));
    assert(is({0, 0, 0, 0}, "unknown"));
    std::puts("Frame pacing verdicts passed: on target, FPS-capped, CPU-, GPU- and runtime-bound, unknown period.");
}
