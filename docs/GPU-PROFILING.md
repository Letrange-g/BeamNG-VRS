# Find expensive work that misses VRS

This diagnostic build samples GPU timestamps around legacy graphics render
passes and compute dispatches. It helps locate costly work before choosing
another optimisation. It does not change which passes get VRS, and is not a
normal-play performance upgrade. Public 0.1.0 is unchanged.

## Capture one stationary scene

Back up your current DLLs, launcher and INI together. Download the test artifact,
extract its outer ZIP, then install `BeamNG-VRS-0.1.1-profile.1-windows-x64.zip`.
Use the matching launcher; older launchers reject the new setting.

```ini
[Experimental]
EarlyPasses=false
ProfileGPU=true

[Logging]
Level=info
```

Keep normal `[Foveation] Enabled=true`, debug outlines off, and your usual
resolution/scissor/Turbo settings. Restart BeamNG, load the scene, let loading
settle, and stay in the same view for 45–60 seconds. Exit normally. Share
`%TEMP%\BeamNG_VRS_layer.log`, compressed if large. It appends across launches;
rename the existing log before launching if you want a separate capture.

Set `ProfileGPU=false` and restart for normal play. Profiling adds CPU and GPU
overhead; do not use its FPS as the benchmark for a performance release.

## Read the results

`M8.35 GPU_PROFILE` reports cumulative recording, sampling, unavailable-result,
unsupported-path, error and capacity-drop counters. `M8.35 GPU_PASS` ranks up to
20 groups by sampled GPU elapsed time **since the previous report**, roughly
every five seconds. Later scene windows replace loading-screen rankings.

| Field | Meaning |
| --- | --- |
| `kind=graphics` / `compute` | Whole legacy render pass / one compute dispatch. VRS does not accelerate compute shaders. |
| `pass`, `firstPipeline` | Process-local diagnostic IDs. They are not engine names, shader hashes, or stable IDs across runs. A graphics pass can contain many pipelines. |
| `size`, `colors`, `firstColorFormat`, `depth`, `subpasses` | Render area and attachment description. Format is a Vulkan enum number; compute dimensions are unknown and shown as zero. |
| `samplesMask` | Bitwise union of colour attachment sample counts; zero means absent/unknown, not a zero-sample image. |
| `eye` | Original copy-based eye identity: 0 left, 1 right, -1 unknown. Unknown does not mean non-VR. |
| `shadowReject` | Existing layer's VRS render-pass rejection mask; `0xFFFFFFFF` means unknown. Zero does not prove eye identity or compatible pipelines. |
| `count`, `avgMs`, `maxMs`, `sampledTotalMs` | Available timestamp samples and their GPU spans for this reporting window. |
| `rateAttachment` | The layer redirected this pass to its VRS render-pass copy. |
| `drawCalls`, `vrsDrawCalls` | Intercepted primary draw commands / those with both a substituted VRS pipeline and a VRS attachment. These are API-call counts, not triangles, shader invocations, or a GPU-time percentage. |
| `secondary=1` | Draws inside secondary command buffers are not counted. Treat coverage as unknown. |

A repeatedly expensive graphics group with intercepted draws but zero VRS draws
is a candidate to investigate. Partial coverage needs closer inspection. Zero
draws can indicate untracked APIs or secondary work; it is not proof of no VRS.
Expensive compute groups point to work requiring a different optimisation.
Attachment formats and sizes can suggest a role, but cannot prove that a group
is lighting, SSAO, scene colour, normals or a final composite.

## Measurement and lifetime limits

- Approximately one in 16 eligible primary command-buffer recordings is sampled,
  and one in four scopes within those recordings. Pseudorandom selection avoids
  always selecting the same position in a repeating frame sequence.
- At most 16 query pools per device, 128 timed scopes per sampled recording,
  and 512 aggregate groups per reporting window are retained. Capacity drops
  are reported. Tracking tables also have explicit limits.
- Timestamps use the command pool's actual queue-family valid-bit count and the
  device's timestamp period, including counter wrapping. Both boundaries use
  `BOTTOM_OF_PIPE`. Pipeline overlap and timestamp overhead affect the spans:
  **they are not exclusive shader costs, and must not be summed into frame time**.
- Each query pool belongs to one command-buffer recording. Results are read only
  after a successful submission and when BeamNG legally resets/frees that
  recording, resets/destroys its pool, or destroys its device. No query WAIT flag,
  new fences, queue-idle wait or device-idle wait is introduced by this profiler.
- Replayed recordings are excluded from results. Simultaneous-use/protected
  recordings, unsupported queue families, begin-info extension chains and
  multi-device groups are excluded. Results can be delayed until reset/free;
  unavailable or incomplete samples are discarded.
- A dynamic-rendering begin stops further sampling in that recording, avoiding
  injected commands between suspended/resumed rendering segments. Transfer work,
  ray tracing and graphics APIs such as mesh-task draws are not fully profiled.
  Legacy passes can include secondary rendering in their elapsed span, but its
  individual draw/VRS coverage remains unknown.
- Typical draw/indirect/count commands and direct/base/indirect compute dispatch
  commands are intercepted. Empty commands may still count as recorded API calls.

Automated tests use a fake driver to check timestamp math, query availability,
submission/reset lifetimes, bounded storage, coverage and hook forwarding. They
cannot validate real NVIDIA timing accuracy, BeamNG stability, or headset FPS.
