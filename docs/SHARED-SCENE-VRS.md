# Shared scene VRS experiment

`0.1.1-shared.1` tests whether VRS on earlier scene-color passes reduces GPU
frame time. It is separate from the public release and starts disabled.

The resource traces show intermediate targets in both eye paths. This experiment
uses one fixed, centered VRS pattern on eligible learned targets, without assigning
them a permanent left or right eye. It uses your existing `Size1x1`, `Size2x2`, and
`Shape` settings. Final-eye VRS continues to follow gaze; the existing scissor behavior is unchanged.

The earlier pattern does **not** follow your eyes. Looking away from the center
can expose coarse shading that final-eye VRS cannot undo. Check both eyes in the
headset. There is no measured or promised performance gain for this build yet.

## Compare off and on

1. Back up your INI and install the complete test ZIP. Keep your normal VRS,
   scissor, Turbo, resolution, anti-aliasing, and game settings for both runs.
2. Set these values in the existing sections of `BeamNG-VRS.ini`:

   ```ini
   [Foveation]
   Enabled=true
   [Debug]
   DrawOutline=false
   [Experimental]
   EarlyPasses=false
   SharedSceneVRS=false
   ProfileGPU=false
   [Logging]
   Level=info
   ```

3. Rename the old `%TEMP%\BeamNG_VRS_layer.log`. Start
   **Launch-BeamNG-VRS.cmd**, load your usual scene and stay in one view. After
   loading settles, wait another 10 seconds, then observe GPU frame time for
   60 seconds. Exit and save this log as the off run.
4. Change only `SharedSceneVRS=true`, restart, and repeat the same scene and
   timing. Check both headset views, including looking away from the center.
5. Compare GPU frame time and headset appearance. FPS alone can hide a change
   when the headset caps frame rate. A second off run helps reveal loading or
   background work that affected the comparison.

Keep `ProfileGPU=false` during these comparisons. Use the normal launcher;
the Diagnose launcher enables extra logging intended for launch problems.

If the headset flickers, shows incorrect shading, or loses too much detail,
restore `SharedSceneVRS=false` and restart. Disabling the experiment restores
the usual final-eye VRS behavior. It does not require changing your scissor or
Turbo settings.

## Check coverage

`M8.37` entries in `%TEMP%\BeamNG_VRS_layer.log` report selected targets and
binds. A nonzero bind count means the experiment reached an earlier pass.
Zero binds means the test did not apply there; it does not establish that earlier
VRS has no benefit. Share the complete off/on logs along with GPU frame times
and any headset artifacts.

The selection is inference from observed image use, not shader analysis. It is
limited to eligible scene-color targets and may miss work. It does not change
the game's geometry, draw distance, render resolution, or compute shaders.
Sampled pass timings from the earlier diagnostic are not guaranteed savings.

`SharedSceneVRS` accepts `true` or `false` (also `1` or `0`), is case insensitive,
and defaults to `false`. It requires `[Foveation] Enabled=true` and
`EarlyPasses=false`; the launcher rejects conflicting settings. For direct
developer loading, the corresponding environment variable is
`BEAMNG_VRS_SHARED_SCENE=1` or `0`.

## Implementation limits

The experiment records bounded observations of submitted primary command buffers.
It requires a full-eye-size, single-sample SFLOAT color target with one color
attachment and depth, at least 128 draw API calls in its learned producer pass,
and candidate input connections to both eyes across three submissions. Learning
expires after two seconds without recent qualifying activity. Image/pass
lifetimes and resolution changes invalidate the corresponding history.

Lightweight writes and copies can occur between the heavy producer and the final
eye pass. The learner records repeated participation of a target in those stages;
it does not reconstruct the last writer, prove shader reads, or verify matching
texture coordinates. The UNORM pass from the timing capture remains excluded.
A second queue with relevant work disables learning. Unsupported recordings and
capacity limits can prevent selection; coverage counters make this visible.

The common framebuffer uses the launcher's default lazy framebuffer path. A
legacy override disabling that path, or unsupported framebuffer extensions, can
leave shared coverage at zero. No GPU timestamps, new GPU waits, or per-draw log
messages are required. CPU tracking still has a cost, so compare CPU time too.
