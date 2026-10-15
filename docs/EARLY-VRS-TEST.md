# Earlier-pass VRS experiment

This test build includes the compatible-render-pass fix and an optional attempt
to apply VRS to an earlier colour pass feeding an eye output. No FPS improvement
has been established. It does not replace the public 0.1.0 release.

## Install and compare

Back up your existing DLLs and INI. Extract the test ZIP in the same location as
your current installation. GitHub Actions downloads have an outer artifact ZIP;
extract that first to find `BeamNG-VRS-0.1.1-early.1-windows-x64.zip`.
Use the matching test launcher because older launchers reject the new setting.
Keep your usual sizes, shape, resolution, Turbo and scissor settings fixed.

In the root `BeamNG-VRS.ini`, add or edit:

```ini
[Experimental]
EarlyPasses=on
```

Restart BeamNG after each change. Run the same warmed-up stationary scene for
at least 10 seconds, with debug outlines off. Compare these three modes in the
**same test build**:

| Value | Purpose |
| --- | --- |
| `off` (default) | Existing VRS scope, without the new tracking hooks. |
| `trace` | Discover candidate inputs, but keep the existing VRS scope. Measures tracking overhead. |
| `on` | Track candidates and extend VRS to candidates with repeated, unambiguous evidence. |

Keep `[Foveation] Enabled=true`. The experiment does nothing when VRS is disabled.
Record GPU time and CPU/frame time, then turn your head and inspect each eye for
shimmer, lighting changes, misplaced sharp regions or flicker. An inference can
be wrong even when all Vulkan objects are valid. Restore `off` if appearance or
performance is worse. To return to 0.1.0, restore the old DLLs/launcher/INI together.

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

If `selected` and `binds` remain zero in `on`, the experiment did not widen VRS
coverage. That result does **not** demonstrate that earlier VRS cannot help;
the discovery path may simply miss this renderer's command structure. Share the
Vulkan layer log, mode, per-eye resolution and corresponding frame times.
Counters measure command recording, not executed GPU work or per-pass duration.
The standard game's `beamng.log` does not contain these layer counters.

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

The tracking adds CPU work in `trace` and `on`; compare both against `off`.
Automatic tests cover graph scope, ambiguity, resource reuse, actual Vulkan hook
forwarding and mode-dependent selection. They cannot establish driver/headset
compatibility or performance. No GPU-timestamp instrumentation is included.
