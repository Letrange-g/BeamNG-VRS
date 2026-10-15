# Earlier-pass VRS experiment

This test build includes the compatible-render-pass fix and an optional attempt
to apply VRS to an earlier colour pass feeding an eye output. No FPS improvement
has been established. It does not replace the public 0.1.0 release.

## Install and compare

Back up your existing DLLs and INI. Extract the test ZIP in the same location as
your current installation. GitHub Actions downloads have an outer artifact ZIP;
extract that first to find `BeamNG-VRS-0.1.1-early.3-windows-x64.zip`.
Use the matching test launcher because older launchers reject the new setting.
Keep your usual sizes, shape, resolution, Turbo and scissor settings fixed.

In the root `BeamNG-VRS.ini`, add or edit:

```ini
[Experimental]
EarlyPasses=true
```

Restart BeamNG after each change. Run the same warmed-up stationary scene for
at least 10 seconds, with debug outlines off. Compare these three modes in the
**same test build**:

| Value | Purpose |
| --- | --- |
| `false` (default) | Existing VRS scope, without the new tracking hooks. |
| `trace` | Discover candidate inputs, but keep the existing VRS scope. Helps isolate tracking overhead. |
| `true` | Track candidates and extend VRS to candidates with repeated, unambiguous evidence. |

From `early.3`, use `true`/`false` like the other INI switches. The old `on`/`off`
spellings remain accepted, as do `1`/`0`; values are case-insensitive. `trace` is
an advanced diagnostic mode. Logs retain the internal `mode=on`/`mode=trace` names.
Older DLLs and launchers reject `true`/`false` here: update both together.

Keep `[Foveation] Enabled=true`. The experiment does nothing when VRS is disabled.
Record GPU time and CPU/frame time, then turn your head and inspect each eye for
shimmer, lighting changes, misplaced sharp regions or flicker. An inference can
be wrong even when all Vulkan objects are valid. Restore `false` if appearance or
performance is worse. To return to 0.1.0, restore the old DLLs/launcher/INI together.

The `early.2` build also reuses idle VRS framebuffers in **all three modes**.
It only reuses an exact match after the original framebuffer is destroyed, while
its attachment views and render pass remain alive. Up to 64 idle variants are
retained; custom allocators and extended framebuffer descriptions bypass reuse.
This does not change the game's own framebuffer creation or the VRS pattern.
Compare `early.2 off` with `early.1 off` separately to assess that change.

The draw hooks now combine dispatch lookup and tracking into one lock, copy only
the required function pointer, and skip repeated descriptor scans until bindings
or descriptors change. The inference rules and ambiguity checks remain in place.

## Read the result without Nsight

Keep `[Logging] Level=info`. Every five seconds the Vulkan layer writes a brief
`M8.33 EARLY_VRS` line to `%TEMP%\BeamNG_VRS_layer.log`:

- `eyeDraws`: recorded draws in directly identified eye colour passes.
- `links`: observations of a unique earlier producer bound as a sampled input.
- `images`: supported colour images tracked, not all necessarily eye-sized.
- `learned` / `ready`: candidate image records / currently confirmed candidates.
- `selected`: earlier render passes actually redirected to a VRS copy.
- `binds`: successful VRS pipeline substitutions inside those earlier passes.
- `ambiguous`, `noInput`, `unsupported`: why discovery may not find candidates.
- `descriptorScans` / `scanSkips`: actual descriptor inspections / repeated draws
  that avoided them (also includes already-ambiguous draws).

If `selected` and `binds` remain zero in `true`, the experiment did not widen VRS
coverage. That result does **not** demonstrate that earlier VRS cannot help;
the discovery path may simply miss this renderer's command structure. Share the
Vulkan layer log, mode, per-eye resolution and corresponding frame times.
Counters measure command recording, not executed GPU work or per-pass duration.
The standard game's `beamng.log` does not contain these layer counters.

## CPU overhead measurements in early.2

Alongside periodic framebuffer statistics, `M8.34 FB_REUSE` reports cumulative
cache `hits`, `misses`, the number of idle entries, actual lazy VRS framebuffer
`driverCreates`, and time spent in those driver creation calls (`driverCreateMs`).
A hit avoids one VRS framebuffer creation. Zero hits can be legitimate when the
game changes attachment views or render-pass objects frequently. Counts exclude
BeamNG's original framebuffers and the legacy eager path.

`M8.34 CPU_SAMPLE` reports sampled average/maximum CPU wall time in microseconds
for draw/unsupported-command tracking, descriptor binding, descriptor updates,
and pass begin/end tracking. One in 1,024 calls per category per recording thread
is timed, including time waiting for the layer mutex, excluding downstream draw
calls and file logging. Update timing excludes downstream descriptor updates and
their dispatch lookup. Pass timing does not include the existing VRS selection
machinery. These are **partial, sampled costs**, not total CPU time per frame,
GPU timings, or proof of the cause of a frame-time spike. `maxUs` is the largest
sample, not necessarily the slowest actual call. Brief runs can have few samples.
Sample timing is disabled at logging levels below `info`.

For a useful comparison, record CPU and GPU frame times in the same scene with
`false`, `trace`, and `true`, and share the corresponding M8.33/M8.34 lines. Compare
counter differences over the same interval; logs append across launches. Check
that `selected`/`binds` still rise in `true`: CPU savings must not come merely from
losing VRS coverage. No FPS gain is claimed without that headset comparison.

## Scope and limitations

The layer follows one hop: a supported colour target produced earlier in the
same primary command-buffer recording and subsequently bound as a sampled image
in a directly identified eye pass. It waits for three distinct recordings,
retains the producer's render-pass identity, expires unused evidence after two
seconds, and rejects shared-eye or ambiguous evidence. It uses that information
on subsequent recordings; it cannot change commands already recorded.

Only full-eye-size, single-layer, single-sample legacy passes with one colour
output and a compatible VRS shadow qualify. Common RGB(A) colour formats are
allowed; depth-only targets, multiple colour outputs, storage images, other
resolutions, layered/dynamic rendering and secondary command buffers do not.
The classifier is separate from scissor and outline eye identification.

Descriptor tracking supports ordinary single-element sampled-image writes and
single-element copies. Array/spanning writes and template-updated sets are
unknown until reallocated. Push descriptors, compute, secondary work and image
transfers break the local producer chain. Descriptor sets bound before the render-pass
begin are intentionally not inferred. Unsupported/newer binding APIs and
cross-command-buffer producer chains are not reconstructed by this first test.

This is inference from recorded **bindings**, not shader reflection or a GPU
capture: a bound texture may be unused, descriptor contents may change later,
and matching dimensions do not prove matching UV coordinates. Repeated evidence
reduces accidental matches but does not prove a pass is scene colour rather
than an intermediate effect. That is why this remains opt-in and needs visual
testing. It deliberately does not recursively spread VRS through the whole graph.

The tracking adds CPU work in `trace` and `true`; compare both against `false`.
Automatic tests cover graph scope, ambiguity, resource reuse, actual Vulkan hook
forwarding and mode-dependent selection. They cannot establish driver/headset
compatibility or performance. A separate opt-in [GPU profiling mode](GPU-PROFILING.md) is available in the profiling test build; leave it disabled during FPS comparisons.
