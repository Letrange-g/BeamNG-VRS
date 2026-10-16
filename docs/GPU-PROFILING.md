# Find expensive work that misses VRS

This diagnostic build samples GPU timestamps around legacy graphics render
passes and compute dispatches. It helps locate costly work before choosing
another optimisation. It does not change which passes get VRS, and is not a
normal-play performance upgrade. Public 0.1.0 is unchanged.

## Capture one stationary scene

Back up your current DLLs, launcher and INI together. Download the test artifact,
extract its outer ZIP, then install `BeamNG-VRS-0.1.1-profile.3-windows-x64.zip`.
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
settle, and stay in the same view for 60 seconds. Use `Launch-BeamNG-VRS.cmd`, not
`Diagnose-BeamNG-VRS.cmd`. Exit normally. Share
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

## Resource connections in profile.2

The first user capture identified two graphics groups at eye resolution with
roughly 1.2–1.3 ms sampled spans and no VRS draws. Their eye identity was unknown.
This build adds resource observations to investigate those intermediate targets;
it does **not** enable VRS on them or claim those spans are recoverable savings.

- `GPU_PASS` now includes `colorView`, `depthView` and `storageView`. Groups split
  by their actual target view identities. Compute groups split by the first
  observed storage view; that view is **not** assumed to be a write target.
- `M8.36 GPU_LINK` lists candidate sampled/input-attachment reads and storage
  views bound during sampled graphics/compute work. Up to 128 groups per window
  get link records, including groups outside the timing table's top 20.
- `GPU_VIEW` maps a view ID to an image ID, format and mip/layer/aspect range.
  `GPU_IMAGE` describes dimensions, format, usage and sample count. IDs are
  process-local lifetime IDs; reused handles get new IDs. Old sample snapshots
  retain their original identities after resources are destroyed.
- `eyeMask` records direct transfers to currently published OpenXR eye images:
  0 unknown, 1 left, 2 right, 3 both over the image's lifetime. Both endpoints
  receive the observed label; historical source handles do not seed new image
  generations. This diagnostic does not expand the existing VRS classifier. A shared depth image or a matching resolution
  is insufficient evidence for assigning an eye.
- `GPU_TRANSFER` records image copy/blit/resolve endpoints and region count.
  These are recorded operations, including potentially unsubmitted recordings;
  subresource regions, execution order and synchronization are not reconstructed.
- `partial=1` identifies missing/truncated/unsupported observations. Image arrays,
  descriptor update templates, push descriptors, layout changes and secondary
  execution can make the observations partial. Ordinary sampled material textures
  are excluded; render-target, depth, storage and unknown imported images remain.
  Buffers and shader access reflection are not captured.

Descriptor image definitions are cached at update time. Bound descriptors are
examined only inside sampled GPU scopes, when bindings or descriptor definitions
change. This supports chains through different command buffers and compute stages
as **candidate** connections, not verified dataflow. A bound resource need not be
read by the shader; storage access may be read, write, or both. Descriptors can be
updated after recording. Unions within a reporting group can combine different
uses of a shared target. `partial=0` does not prove complete shader coverage.

No per-draw logging, GPU waits, or new VRS selections are added. This extra
tracking still has diagnostic overhead. `ProfileGPU=false` disables it along with
GPU timing; leave `EarlyPasses=false` for this capture and normal play.

Resource storage is bounded: 32,768 live image definitions, 65,536 views, 16,384
sets/framebuffers, 128 image slots per set, 8 bound sets per bind point, 32 candidate
read and 32 storage views per sample/group, and 256 transfer groups per report.
`GPU_LINKS` reports omitted groups and cumulative resource-table drops. A capped
or unsupported path must not be interpreted as absence of an image dependency.

## Descriptor cache fix in profile.3

A profile.2 capture reached 551,892 resource-tracking drops while GPU timestamps
continued working. Most final-eye groups had missing descriptor inputs. The
aggregate counter did not identify which table was full, so this build adds
separate capacity and coverage counters in `M8.36 GPU_TRACKER`.

The previous 16,384-set table stopped accepting new allocations once full, then
ignored updates for those missing sets. The table now evicts the least recently
observed set and keeps accepting new ones at the same memory bound. New writes
to an evicted set recover the known slots, with `partial=1` for missing history.
Recovered sets have unknown pool ownership and are conservatively invalidated on
any descriptor-pool reset/destruction in that device. Allocation, free and handle
reuse never inherit a previous set's contents.

Ordinary material textures are filtered before consuming the 128 candidate-image
slots per set. Replacing a target descriptor with a material texture erases the
old candidate edge. Unsupported array/template writes still remain partial.

`GPU_TRACKER` reports current set count/capacity; cumulative set evictions,
recoveries and sampled set misses; separate image/view/pass/framebuffer/slot/
transfer capacity drops; and unknown updates/missing view definitions. Evictions
count in `resourceDrops` but are **not dropped game frames or GPU commands**.
Low current misses and actual recorded final-eye input links matter more than
requiring the cumulative eviction count to stay zero.

Tests cover 20,000 descriptor allocations with the production 16,384-set bound,
retention of recently sampled sets, recovery and pool invalidation, single-entry
cache copies, handle reuse, device cleanup, and material textures that previously
could consume candidate slots. No additional rendering pass receives VRS.

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
