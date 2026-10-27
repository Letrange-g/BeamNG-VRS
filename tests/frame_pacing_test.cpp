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
    // The user report: GPU 5-8 ms, game busy the whole slower frame.
    assert(is({14.0, 0.5, 6.0, 11.11}, "CPU-bound"));
    // GPU needs the whole budget; the runtime holds the game in xrWaitFrame.
    assert(is({22.2, 9.0, 10.5, 11.11}, "GPU-bound"));
    // Both sides fast, yet frames are slow: the runtime throttles (half-rate reprojection).
    assert(is({22.2, 15.0, 5.0, 11.11}, "runtime-bound"));
    // Unknown display period: judged against the frame interval.
    assert(is({14.0, 0.5, 6.0, 0}, "CPU-bound"));
    assert(is({0, 0, 0, 0}, "unknown"));
    std::puts("Frame pacing verdicts passed: on target, CPU-, GPU- and runtime-bound, unknown period.");
}
