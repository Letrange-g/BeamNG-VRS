# Performance work: M8.27–M8.31

This beta reduces layer overhead while retaining the configured foveation sizes,
shape, 30 ms gaze grace and 8 ms refresh interval. GPU/FPS improvements must be
measured in BeamNG; there is no game, headset or rendering GPU in the cloud test
environment.

## Implemented changes

- **Pattern generation:** each row intersects a circle, square or ellipse in a
  contiguous span. Binary searches use the original float predicate to find its
  boundaries, then fill the spans. No square-root approximation, gaze smoothing,
  or change to boundary tiles is introduced. The original per-tile loop remains
  a test/benchmark oracle; 1,872 deterministic cases compare every byte across
  shapes, sizes, gaze edges, partial/unequal tiles and randomized inputs.
- **Identical payloads:** a new gaze sample still updates tracking, but a
  byte-for-byte identical shading-rate image needs no new staging write,
  transfer barriers or queue submission. The comparison only reads coherent
  staging memory and does not modify a pinned slot. A changed payload is
  prepared at the existing refresh interval. Resolution changes invalidate the
  sequence shortcut, including center fallback during tracking loss.
- **Upload resources:** completed upload command pools, buffers and fences enter
  a device-local cache of at most six idle sets. Reuse requires a signaled fence
  and matching queue family. Command pools and fences are reset before recording;
  pending uploads remain fence-owned. Missing reset entrypoints retain the
  create/destroy path. The single-graphics-queue asynchronous barriers and
  conservative multiple-queue fallback are retained.
- **Reusable recordings (M8.28):** if a completed command buffer references the
  same source slots, eye images, extents and queue family, only its fence is reset.
  It can be submitted again without recording its barriers/copies anew. Source
  bytes are read at execution time, so new gaze payloads use the same recording.
  Buffers are recorded without `ONE_TIME_SUBMIT` or `SIMULTANEOUS_USE`; an in-flight
  buffer never enters the cache. Different slots/resolutions/eye selections need
  a matching recording or a reset/re-record. All GPU barriers still execute.
- **Command hooks:** dispatch snapshots contain only trivially copyable data;
  queue-family vectors remain in device state rather than being allocated on
  each command. Quiet logging bypasses diagnostic pipeline-bind bookkeeping.
  Feature flags are cached on first use, consistently with the documented
  restart requirement. Disabled scissor and outlines skip their state lookups.
  Up to 16 dynamic scissors use stack storage, and black borders are cleared
  in one call.
- **Four-edge scissor (beta.8 / M8.29):** `Left` and `Right` crop fractions of
  eye-image width and default to zero. Top/bottom retain their height-relative
  defaults. The same rectangle clips matching static and dynamic scissors,
  including static scissors in compatible VRS pipeline copies. Earlier copies
  could retain the application's uncropped static scissor.
  Up to four non-overlapping border rectangles are cleared together, restricted
  to the original render area. This avoids clearing retained corner pixels.
  Cropping reduces rasterization/pixel coverage; it does not cull objects or
  avoid vertex processing. Explicit legacy `Left`/`Right` values retain their meaning.
- **Per-eye horizontal scissor (beta.9 / M8.30):** beta.11 defaults the outer
  edges to 0.20 and nose-side edges to zero, with scissor still off. Four width-relative controls
  separate nose-side and outside edges. Eye selection follows framebuffer image
  classification learned from OpenXR transfers, independently of debug outlines
  and VRS substitution. Matching static pipelines with asymmetric settings get
  two cropped copies at creation, including compatible VRS/stencil copies;
  binds select the correct eye copy without creating pipelines or allocating.
  Dynamic scissors and final border clears use the same eye selection. Unknown
  or mixed-eye targets keep the common rectangle (minimum horizontal crop per
  side), avoiding premature clipping of pixels the other eye needs. Copy creation
  failure cleans up partial copies and falls back to the common static crop with
  exact per-eye border clears. This fallback does not save the extra per-eye
  rasterization work. Secondary-only, layered and dynamic-rendering crop paths
  are not added. Old shared INI edges remain supported.
- **Submission APIs:** live uploads now precede `vkQueueSubmit2` and
  `vkQueueSubmit2KHR` as well as the original `vkQueueSubmit` path.
- **Gaze freshness:** live VRS reads a separate projection mapping. Raw-pose
  diagnostic updates cannot make old projected UV coordinates appear fresh.
  A completed projection publishes its own timestamp and sequence; actual
  projection failures publish invalidity while count-only queries preserve the
  prior sample. The existing grace and centered fallback handle invalid data.
  These timestamps measure API retrieval/publication, not hardware sample time
  or eye-to-display latency. Update **both DLLs** together.
  Source-age telemetry now starts at projection publication and is not directly
  comparable with beta.5's raw-pose timestamp.

Native mock tests exercise the real Vulkan hooks and OpenXR interceptor. They
check allocation-free common command paths, unchanged pipeline substitution,
batched scissor clears, identical uploads, pinned-slot immutability, retry,
resolution fallback, independent projection freshness, and failure/recovery.
Upload-cache tests cover completion gating, queue-family isolation, bounded
ownership and allocation/reset failures. They do not validate a GPU driver.

## CPU benchmark

`benchmarks/pattern_benchmark.cpp` constructs one eye's CPU shading-rate payload.
It compares the original and optimized kernels with the same compiler/flags,
128 seeded gaze positions and nine alternating batches, and reports the median
microseconds per pattern. Payload equality is checked before timing. Results
include output allocation; they exclude shared-memory reads, staging copies,
Vulkan calls and GPU execution. No fast-math option is used.

Cloud Linux, GCC 14.2, `-std=c++20 -O2`, default sizes 0.25/0.60:

| Eye resolution | Shape | Original µs | Spans µs | CPU speedup |
| --- | --- | ---: | ---: | ---: |
| 2595×2084 | ellipse | 76.78 | 8.82 | 8.7× |
| 3892×3126 | ellipse | 168.84 | 13.67 | 12.4× |
| 5190×4168 | ellipse | 311.09 | 20.93 | 14.9× |

These are host/compiler-specific microbenchmarks, **not FPS gains**. The Windows
release workflow also runs the benchmark and saves a separate
`CPU-pattern-benchmark` artifact and attaches its CSV to published releases;
benchmark/test EXEs are excluded from the ZIP.

Windows Server 2022, Visual Studio 2022 Release, beta.6 build
[`891d8da`](https://github.com/Letrange-g/BeamNG-VRS/commit/891d8da5f73298633e3aba6d703c63f276be50f8):

| Eye resolution | Shape | Original µs | Spans µs | CPU speedup |
| --- | --- | ---: | ---: | ---: |
| 2595×2084 | ellipse | 77.71 | 13.62 | 5.7× |
| 3892×3126 | ellipse | 158.51 | 17.41 | 9.1× |
| 5190×4168 | ellipse | 279.19 | 23.62 | 11.8× |

The complete [Windows CSV](https://github.com/Letrange-g/BeamNG-VRS/releases/download/v0.1.0-beta.6/pattern-benchmark-windows.csv)
also includes circle and square. Mock transfer tests submit 100 changing payloads
using one recording; three alternating staging pairs retain three recordings.
These establish fewer CPU recording calls and resource ownership, not driver
timings or GPU speedup. The changed bytes reach the mock target on each execution.


Linux reproduction:

```sh
g++ -std=c++20 -O2 -I . -I src benchmarks/pattern_benchmark.cpp -o /tmp/vrs-pattern-benchmark
/tmp/vrs-pattern-benchmark
```

Windows reproduction: configure with `-DBEAMNG_BUILD_BENCHMARKS=ON`, build
Release, then run `build/Release/pattern_benchmark.exe`.

## Findings from earlier development

The provided successful Vulkan log ends at 5,632 uploads, all asynchronous,
averaging 37 µs CPU upload time with a cumulative maximum of 301 µs. It reports
no staging-slot exhaustion, an 8 ms preparation interval, and about 12 ms
average source-to-submit age. It contains 192,272 lines of verbose trace.
Those upload measurements exclude CPU pattern generation and do not measure
GPU frame time. The shared development discussion also describes quieter runs
and successful lazy framebuffer creation, but different scenes/settings prevent
a controlled FPS comparison.


Earlier runtime tests returned no usable Pimax visibility-mask geometry, and
the tested eye passes lacked a suitable stencil path. The experimental stencil
gate stays disabled. Existing lazy framebuffer creation is retained; framebuffer
handles are not recycled across application lifetimes.

## Compare in BeamNG

Initial beta.7 Italy-map tests supplied by the user reported roughly 70 FPS with
default VRS on/off, 78–80 FPS with near-uniform 4×4 shading, and 90 FPS with
top/bottom scissor. The original VRS videos included debug outlines; later
screenshots used an open menu, and the zero-size diagnostic run enabled verbose
logging. These are useful observations, not a controlled benchmark. CPU and GPU
timing order varied across screenshots, so they do not establish a fixed CPU
bottleneck. The visible coarse image confirms an effect on rendering; identifying
the remaining GPU costs needs the layer log and a GPU timeline capture.

1. Back up beta.5 and its INI. Keep the same map, vehicle, camera, weather,
   graphics settings, eye resolution and runtime/reprojection settings.
2. Use normal logging, outlines disabled, and scissor disabled for both builds.
   Warm up the scene before recording. Repeat runs in alternating order.
3. Record GPU and CPU frame times and BeamNG's unmanaged FPS, plus compositor
   behavior. A runtime FPS cap/reprojection threshold can mask an improvement.
4. Compare beta.5 with M8.28 using **both matching DLLs**, then test scissor off
   versus on in M8.28 separately. At top/bottom 0.30, only the middle 40% of the
   image remains; verify the crop does not hide visible content in the headset.
5. Check fixed-center fallback, eye movement, tracking dropout/recovery, both
   debug outlines, and 0.5/0.75/1.0 resolution transitions. Restore outlines off
   before performance measurements.
6. Inspect periodic `M8.17 STATS`, `M8.21 GAZE_STATS`, `M8.27 UPLOAD_CACHE` and
   `M8.27 PATTERN_STATS` reports (`M8.28 UPLOAD_CACHE` in beta.7). Reuse and identical-payload counters should
   rise where appropriate. A static gaze may produce too few uploads for another
   256-upload report. Use verbose diagnostics only when investigating a problem.

## Can VR be capped at 1 km?

There is no Vulkan command that sets world render distance in metres. The layer
sees buffers, shaders and draw commands, not BeamNG objects or their distances.
The user's earlier tests found that normal Lua visibility-distance settings and
fog did not affect the VR rendering path. No 1 km cap is implemented in this beta.

The strongest option is finding BeamNG's actual VR camera/culling distance. An
engine-level limit could avoid distant draw submissions and geometry work as
well as shading. That needs the current game version's renderer interfaces or
an identified runtime variable; the cloud checkout contains neither the game
binary nor its renderer data. Previous chat suggestions about internal variables
or removed sliders are leads, not verified APIs or offsets.

An API-layer experiment could add a far clip plane to the VR pipeline's final
vertex-processing stage. This would require captured shaders and confirmation
of their coordinate conventions/units, clipping feature support, and which
pipelines represent world geometry. It could reject distant rasterization and
fragment work but would still pay draw submission and vertex-processing costs.
UI, sky, transparency, depth conventions, tessellation and geometry stages must
be accounted for. Changing an unknown projection matrix or skipping draws by
vertex count cannot reliably enforce a world-distance limit.

Depth bounds and fog are also insufficient substitutes: normalized depth is not
world distance, and obscuring pixels does not imply object culling. A Vulkan
distance experiment should be compared against engine culling using actual CPU
and GPU timings before adding a user-facing distance setting.

## OpenXR Toolkit turbo mode: feasibility

The upstream [frame hooks](https://github.com/mbucchia/OpenXR-Toolkit/blob/6b9ecb69a4b2dc714b14a86407868af315d02531/XR_APILAYER_MBUCCHIA_toolkit/layer.cpp#L2104)
start a background `xrWaitFrame` after `xrEndFrame`. The next app-facing wait can
return an extrapolated predicted display time before that wait completes; the
real begin is deferred until submission. This overlaps app work with runtime
waiting, rather than reducing shading or draw cost. A second poll waits, limiting
pipelining to one frame. Transitions and swapchain destruction also drain waits.

Toolkit's graphics integration uses D3D11/D3D12. The frame scheduling idea itself
uses OpenXR, so a Vulkan application could use an implementation in our OpenXR
layer; it is not a Vulkan VRS setting or a drop-in Toolkit switch. It needs session
lifecycle, thread safety, frame call order, prediction, swapchain lifetime and
runtime failure handling. Upstream explicitly warns about motion reprojection,
ASW and motion smoothing, and documents a possible one-second transition freeze.

**Implemented as an opt-in experiment in beta.10 / M8.31.** The independent
scheduler uses a persistent session worker, one outstanding runtime wait and
short extrapolation only while the previous state was renderable. Ready states
retain runtime `shouldRender`. Hitches, extension chains and an already-pipelined
app use real/synchronous timing. The real wait and begin complete before end is
forwarded. Swapchain drain and session/instance shutdown join or resolve the wait
before downstream objects are destroyed. OpenXR has no cancellation for a blocked
runtime wait; a broken runtime can still stall teardown. Tests use a blocking
mock callback to verify overlap, order, error propagation, one-frame bounds and
lifetime; these are not runtime/headset validation.

`[Foveation] Enabled=false` permits scissor/turbo without VRS pattern preparation,
uploads or gaze sampling. Eye-image discovery remains active. `[Logging] Level=off`
skips logger message classification/file access; normal defaults retain brief
status/error logging, and diagnostics override the level. No new mirror-render
pass, scene draw or image copy is introduced by the crop controls. A reported
monitor-only image strip remains unconfirmed: a screenshot does not distinguish
mirror composition/stale pixels from extra GPU work. Headset output was reported
unaffected, so the crop scope is retained for this timing experiment.

For an in-game comparison, measure normal `xrWaitFrame`, begin/end and
swapchain wait durations, alongside GPU time, with the runtime and reprojection
mode identified. Benefits depend on avoidable wait bubbles; it cannot reduce GPU
rendering cost or the game's non-VR CPU variability. The supplied loader logs do
not contain those timings. Turbo tests should be opt-in and separate
from crop comparisons, with frame pacing and head-motion appearance checked in
the headset as well as FPS.
