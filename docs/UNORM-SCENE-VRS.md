# UNORM scene VRS experiment

`0.1.1-unorm.2` tests the performance ceiling of **uniform 4x4 shading** on
selected pipelines in an earlier scene-color pass. The new `UnormSceneVRS`
switch starts disabled and can run independently of `SharedSceneVRS`.

This is a temporary coarse-shading experiment, including the center of the
view. `Size1x1`, `Size2x2`, and `Shape` do not change this UNORM pattern.
Final-eye VRS continues to follow gaze, and per-eye scissor stays on the final
eye outputs, but neither can restore detail lost earlier. Expect visible
pixelation; check both headset views. This test does not promise a performance
gain or represent a finished foveated-quality option.

This revision fixes depth-copy tracking: writing a separate scratch depth image
no longer discards associations for unrelated scene targets. Writes to a depth
image participating in an association still invalidate that association. The
first capture of `unorm.1` produced zero UNORM VRS binds and zero GPU samples,
so it did not measure the performance of this earlier-stage experiment.

## Recommended: one focused capture

The comparison switches the selected stage's VRS off and on automatically, so
one stationary session provides both sets of measurements.

1. Back up your INI and install the complete test ZIP. Keep your usual
   resolution, game settings, VRS sizes, scissor, and Turbo settings.
2. Set these values in the existing sections of `BeamNG-VRS.ini`:

   ```ini
   [Foveation]
   Enabled=true
   [Debug]
   DrawOutline=false
   [Experimental]
   EarlyPasses=false
   SharedSceneVRS=false
   UnormSceneVRS=true
   CompareUnormVRS=true
   ProfileGPU=false
   [Logging]
   Level=info
   ```

3. Rename the old `%TEMP%\BeamNG_VRS_layer.log`. Start
   **Launch-BeamNG-VRS.cmd**, let the scene finish loading, then remain
   stationary for about **60 seconds**. Check both headset views while the
   shading changes.
4. Exit and share the complete log. Set `CompareUnormVRS=false` and
   `UnormSceneVRS=false`, then restart to return to your usual rendering.

Leave `ProfileGPU=false`: the comparison enables focused timing internally,
without the resource-graph capture. The 10-second phases start at the first
qualified scope; the first 2 seconds of each phase are excluded from
measurements. The phase clock may start during loading, which is why the
complete log matters. This mode adds measurement overhead and deliberately
changes shading during play.

At `[Logging] Level=info`, `M8.38 UNORM_SCENE` reports `learned`, `ready`,
`selected`, and `binds` counts. `depthWrites` counts submitted, live depth writes
handled by destination; `staleDepthWrites` counts rejected old generations.
`M8.38 GPU_UNORM` records focused timings for
`mode=baseline` and `mode=vrs`, together with draw coverage. Both modes need
measured coverage for a useful comparison. `coverage=full` means all recorded
draws in that measured scope used the replacement pipeline; the hardware may
still clamp the actual shading rate. These are **GPU scope timings, not whole
frame totals** or guaranteed savings.

## Optional: manual comparison after a useful result

Once the focused capture shows coverage and a timing difference worth checking,
compare the game's GPU frame time with `CompareUnormVRS=false` and
`ProfileGPU=false`. Keep `SharedSceneVRS=false` and all other settings unchanged.
Run the same stationary scene first with `UnormSceneVRS=false`, then restart
with only `UnormSceneVRS=true`. After loading settles, wait another 10 seconds
and observe each run for 60 seconds. Compare headset appearance as well as GPU
frame time; FPS can hide changes at the headset's frame-rate cap.

## How the pass is recognized

The learner looks for a UNORM pass followed by an SFLOAT scene-color producer,
each with at least 128 draws and sharing the same live depth target. Their
association must fit within a 256-event window. The SFLOAT output must then be
observed in validated descriptors of final-eye draws. Authorization requires
three independent submissions with distinct producer observations and recent
corroboration from both eyes; re-reading one old producer is insufficient.

The learned pipeline set, first pipeline, image and depth generations, and
live object lifetimes constrain where the replacement can be used. Unknown
pipelines keep their normal shading; stale objects or broken associations
invalidate the relevant evidence. This is a candidate association from
submission order and bound textures, not proof of shader reads, matching
texture coordinates, or GPU dependencies.

Learning follows one observed queue. Depth-only startup transfers do not choose
that queue. A later foreign-queue write to a participating depth disables
learning, but transfers before discovery are not used to prove cross-queue
synchronization.

## Settings and limits

`UnormSceneVRS` and `CompareUnormVRS` accept `true` or `false` (also `1` or `0`)
and are case insensitive. Both default to `false`. The launcher rejects
`UnormSceneVRS=true` unless `[Foveation] Enabled=true` and `EarlyPasses=false`;
it rejects `CompareUnormVRS=true` unless `UnormSceneVRS=true`.

Disabling `UnormSceneVRS` does not disable normal final-eye VRS, scissor, Turbo,
or a separately enabled `SharedSceneVRS`. Turn off `CompareUnormVRS` first when
leaving the diagnostic mode. Restore both new flags to `false` and restart if
either eye looks wrong.

Target selection is inference from observed image use, not shader analysis.
The pilot keeps the original compatible render pass and applies uniform VRS
only to selected pipelines, so an unexpected pipeline can retain its normal
shading. It can miss work and does not change geometry, draw distance, render
resolution, or compute shaders. A timing difference on the selected stage is
not the same as a guaranteed whole-frame saving. No improvement is implied by
the feature merely being enabled.

For direct developer loading, the corresponding environment variables are
`BEAMNG_VRS_UNORM_SCENE=1` and `BEAMNG_VRS_COMPARE_UNORM=1` (`0` disables each).

See [the earlier shared-scene experiment](SHARED-SCENE-VRS.md) for the separate
SFLOAT target test.
