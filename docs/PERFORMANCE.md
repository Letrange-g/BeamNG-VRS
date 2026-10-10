# Performance work: M8.27

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
- **Command hooks:** dispatch snapshots contain only trivially copyable data;
  queue-family vectors remain in device state rather than being allocated on
  each command. Quiet logging bypasses diagnostic pipeline-bind bookkeeping.
  Feature flags are cached on first use, consistently with the documented
  restart requirement. Disabled scissor and outlines skip their state lookups.
  Up to 16 dynamic scissors use stack storage, and the two black bars are cleared
  in one call.
- **Submission APIs:** live uploads now precede `vkQueueSubmit2` and
  `vkQueueSubmit2KHR` as well as the original `vkQueueSubmit` path.
- **Gaze freshness:** live VRS reads a separate projection mapping. Raw-pose
  diagnostic updates cannot make old projected UV coordinates appear fresh.
  A completed projection publishes its own timestamp and sequence; actual
  projection failures publish invalidity while count-only queries preserve the
  prior sample. The existing grace and centered fallback handle invalid data.
  These timestamps measure API retrieval/publication, not hardware sample time
  or eye-to-display latency. Update **both DLLs** together.

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
`CPU-pattern-benchmark` artifact; benchmark/test EXEs are excluded from the ZIP.

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

1. Back up beta.5 and its INI. Keep the same map, vehicle, camera, weather,
   graphics settings, eye resolution and runtime/reprojection settings.
2. Use normal logging, outlines disabled, and scissor disabled for both builds.
   Warm up the scene before recording. Repeat runs in alternating order.
3. Record GPU and CPU frame times and BeamNG's unmanaged FPS, plus compositor
   behavior. A runtime FPS cap/reprojection threshold can mask an improvement.
4. Compare beta.5 with M8.27 using **both matching DLLs**, then test scissor off
   versus on in M8.27 separately. At top/bottom 0.30, only the middle 40% of the
   image remains; verify the crop does not hide visible content in the headset.
5. Check fixed-center fallback, eye movement, tracking dropout/recovery, both
   debug outlines, and 0.5/0.75/1.0 resolution transitions. Restore outlines off
   before performance measurements.
6. Inspect periodic `M8.17 STATS`, `M8.21 GAZE_STATS`, `M8.27 UPLOAD_CACHE` and
   `M8.27 PATTERN_STATS` reports. Reuse and identical-payload counters should
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
