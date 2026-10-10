# Configuration

The release ZIP puts `BeamNG-VRS.ini` beside `Launch-BeamNG-VRS.cmd` at the extraction root. Edit it in Notepad and restart BeamNG. The launcher validates it before starting the game. No INI editor application is required. When upgrading from beta.7 or earlier, restore your saved INI here; the old nested copy is not used by the new launcher.

| Section | Setting | Default | Meaning |
| --- | --- | --- | --- |
| Foveation | `Enabled` | `true` | Apply VRS. Set `false` for scissor-only or turbo-only use; those settings remain independent. Beta.10+. |
| Foveation | `Size1x1` | `0.25` | Inner full-quality region size (0 to 1); ellipse uses width/height-relative diameters, circle uses height, square uses height-relative side length. |
| Foveation | `Size2x2` | `0.60` | Outer size of the medium-quality region (0 to 1); must be at least `Size1x1`. Outside it, shading uses 4x4. |
| Foveation | `Shape` | `circle` | `circle`, `square`, or `ellipse`. Square uses side length; ellipse uses eye-width-relative horizontal diameter and eye-height-relative vertical diameter. |
| Debug | `DrawOutline` | `false` | Draw the red 1x1 boundary and orange 2x2-to-4x4 boundary. |
| Scissor | `Enabled` | `false` | Experimental four-edge crop. Test visible edges and frame times before enabling. |
| Scissor | `Top` | `0.30` | Fraction of eye-image height cropped from the top when scissor is enabled (0 to 0.49). |
| Scissor | `Bottom` | `0.30` | Fraction cropped from the bottom (0 to 0.49). |
| Scissor | `LeftEyeLeft` | `0.10` | Left eye's outer/left edge, fraction of that eye's image width (0 to 0.49). Beta.9+. |
| Scissor | `LeftEyeRight` | `0` | Left eye's nose-side/right edge (0 to 0.49). Beta.9+. |
| Scissor | `RightEyeLeft` | `0` | Right eye's nose-side/left edge (0 to 0.49). Beta.9+. |
| Scissor | `RightEyeRight` | `0.10` | Right eye's outer/right edge (0 to 0.49). Beta.9+. |
| Performance | `TurboMode` | `true` | Experimental one-frame OpenXR wait overlap. Enabled by default in 0.1.0. Restart to change. |
| Logging | `Level` | `info` | `off`, `errors`, `info`, or `debug`. Default is brief status/errors, not verbose diagnostics. |
| Tracking | `GracePeriodMs` | `30` | Hold the last valid gaze for this many milliseconds during temporary tracking loss (0 to 1000). |
| Tracking | `GazeRefreshMs` | `8` | Minimum CPU gaze polling interval (2 to 50 ms). This does not measure eye-to-display latency. |

Use a dot for decimals. Boolean values accept `true`/`false` or `1`/`0`; section/key names and shape values are case-insensitive. Put `;` or `#` comments on their own lines. Duplicate keys, unknown settings, empty files and invalid values are rejected. Omitted settings use the defaults above. Each layer loads the file once when first needed; editing a running game's file does not change its cached settings.

The launcher loads both layers; `[Foveation] Enabled=true` activates the per-eye VRS pipeline and eye sampling. It keeps lazy framebuffer creation and asynchronous updates enabled, stencil disabled, the selected logging level, and a 350 ms debug-outline pause after resolution changes. Legacy feature gates are internal details; users do not need to configure them. Eye-following foveation falls back to fixed center when valid gaze is unavailable after the grace period.

With the default `Shape=circle`, `Size1x1=0.25` makes the sharp circle's diameter 25% of eye-image height; `Size2x2=0.60` sets the medium-quality outer diameter to 60%. Shading uses 4x4 outside it. Ellipse instead scales each diameter to the corresponding eye-image width or height. Sizes do not resize the render targets. GPU savings depend on shaders and GPU bottlenecks; no measured FPS increase is claimed.

## Scissor crop

Since beta.11, the two outer edges default to `0.10` (10%) and the two nose-side
edges default to `0`. Scissor remains disabled by default. Left/right refer to
the image as seen by that eye; they are not mirrored. Top/bottom remain shared.

For **outside-only** cropping, start with:

```ini
[Scissor]
Enabled=true
Top=0.30
Bottom=0.30
LeftEyeLeft=0.05
LeftEyeRight=0
RightEyeLeft=0
RightEyeRight=0.05
```

This leaves the nose-side edges untouched. To crop only near the nose instead,
set `LeftEyeLeft=0` / `RightEyeRight=0` and adjust `LeftEyeRight` / `RightEyeLeft`.
Inspect each eye separately in the headset and restart after each adjustment.
Do not keep a crop whose black border is visible during normal use.

`0.05` means 5% of image width for a horizontal edge. Top/bottom use image height.
With top/bottom 0.30 and only one horizontal edge at 0.05, approximately 38% of
each eye image remains. Cropping both horizontal edges at 0.05 leaves about 36%.
This area reduction is not a predicted FPS gain. Cropping does not cull scene
objects or change the camera projection.

**Upgrading:** beta.8's shared `Left` and `Right` keys are still accepted. An
omitted per-eye key inherits its corresponding shared edge when that shared key
is supplied; otherwise it uses the default in the table above. An explicit
per-eye key wins regardless of file order.
For example, `Left=0.10` with `RightEyeLeft=0` crops the left eye's outside by
10% and leaves the right eye's nose-side edge untouched. These legacy keys
are intentionally omitted from the new template. Update both DLLs and the
launcher together; older versions do not understand the new per-eye names.

## Scissor without VRS

```ini
[Foveation]
Enabled=false

[Scissor]
Enabled=true
```

These `Enabled` keys belong to different sections and do not conflict. Disabling
VRS disables its pattern generation, gaze sampling and upload gates. The OpenXR
layer still identifies eye images so crop settings can apply to the correct eye.
Missing `[Foveation] Enabled` keeps VRS enabled, preserving older INIs.

## Optional turbo

```ini
[Performance]
TurboMode=true
```

Requires the complete beta.10 package. Turbo starts the next runtime frame wait
in the background and can let CPU/render work overlap that wait. It does not
reduce CPU simulation work or GPU shading cost. It is on by default in 0.1.0 and can be
used with or without VRS/scissor. Restart after changing it. Compare the same
scene with turbo off/on; check head movement, stutter and frame times, not just FPS.
It may interfere with motion reprojection. Set it back to `false` if pacing or
headset appearance worsens. No Turbo setting is added to the older beta packages.

## Logging

```ini
[Logging]
Level=off
```

- `off`: no Vulkan/OpenXR layer log output or layer log-file access.
- `errors`: errors and warnings only.
- `info`: brief status and errors (default, formerly internal level 1).
- `debug`: detailed diagnosis (formerly level 2); avoid it for performance comparisons.

Restart after changing the level. Existing files are not deleted. The logging
control applies to this project's two DLLs, not BeamNG, SteamVR or other layers.
`Diagnose-BeamNG-VRS.cmd` overrides the INI with debug logging to collect evidence.
Normal launch uses the INI. Disabling logs may reduce some overhead but no FPS
increase is established. Invalid INI errors still appear in the launcher.

## Existing developer launch scripts

Existing `BEAMNG_VRS_*` and `BEAMNG_EYE_*` process environment variables continue to override corresponding INI values. The public launcher clears these project-specific settings only in the child process, so its INI is authoritative and the parent shell remains unchanged. The old `BEAMNG_VRS_CENTER_RADIUS` and `BEAMNG_VRS_MID_RADIUS` variables map to `Size1x1` and `Size2x2`; their names remain for compatibility, while their values represent diameters.

For custom layer-loading setups, the DLLs prefer `BeamNG-VRS.ini` in the parent of their module folder, with a fallback to an older copy beside the modules if the root file is absent. `BEAMNG_VRS_CONFIG` selects a different absolute file path, including Unicode paths. The public launcher explicitly selects the root file. Without a file, the old environment-only behavior is retained. Invalid files disable the INI activation gates and emit a debugger diagnostic; existing environment overrides still take precedence. The packaged launcher instead rejects the file before launch and displays the parser error.
