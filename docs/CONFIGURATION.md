# Configuration

The release ZIP puts `BeamNG-VRS.ini` beside `Launch-BeamNG-VRS.cmd` at the extraction root. Edit it in Notepad and restart BeamNG. The launcher validates it before starting the game. No INI editor application is required. When upgrading from beta.7 or earlier, restore your saved INI here; the old nested copy is not used by the new launcher.

| Section | Setting | Default | Meaning |
| --- | --- | --- | --- |
| Foveation | `Enabled` | `true` | Apply VRS. Set `false` for scissor-only or turbo-only use; those settings remain independent. Beta.10+. |
| Foveation | `AllPasses` | `true` | Also apply VRS to the game's internal eye-sized passes (scene, lighting, post effects) that are not directly copied to the headset. Those passes use the finer rate of both eyes per tile. Set `false` to foveate only the final eye image, as in 0.1.0. |
| Foveation | `Size1x1` | `0.25` | Inner full-quality region size (0 to 1); ellipse uses width/height-relative diameters, circle uses height, square uses height-relative side length. |
| Foveation | `Size2x2` | `0.55` | Outer size of the medium-quality region (0 to 1); must be at least `Size1x1`. Outside it, shading uses 4x4. |
| Foveation | `Shape` | `circle` | `circle`, `square`, or `ellipse`. Square uses side length; ellipse uses eye-width-relative horizontal diameter and eye-height-relative vertical diameter. |
| Debug | `DrawOutline` | `false` | Draw the red 1x1 boundary and orange 2x2-to-4x4 boundary. |
| Scissor | `Enabled` | `true` | Four-edge crop. Check the visible edges in the headset and reduce the edge values if content is cut off. |
| Scissor | `Top` | `0.30` | Fraction of eye-image height cropped from the top when scissor is enabled (0 to 0.49). |
| Scissor | `Bottom` | `0.30` | Fraction cropped from the bottom (0 to 0.49). |
| Scissor | `LeftEyeLeft` | `0.10` | Left eye's outer/left edge, fraction of that eye's image width (0 to 0.49). Beta.9+. |
| Scissor | `LeftEyeRight` | `0` | Left eye's nose-side/right edge (0 to 0.49). Beta.9+. |
| Scissor | `RightEyeLeft` | `0` | Right eye's nose-side/left edge (0 to 0.49). Beta.9+. |
| Scissor | `RightEyeRight` | `0.10` | Right eye's outer/right edge (0 to 0.49). Beta.9+. |
| LensMask | `Enabled` | `true` | Skip rendering the image corners outside an ellipse, which the headset lenses hide. Experimental; check the edges in the headset. |
| LensMask | `Size` | `1.15` | Kept ellipse diameter relative to eye-image width/height (0.5 to 1.5). `1.0` touches all four edges and skips about 21% of pixels; `1.41` skips none. Increase it if black corners are visible. |
| Performance | `TurboMode` | `false` | Experimental one-frame OpenXR wait overlap. Enabled by default in 0.1.0; off by default since it showed no gain in GPU-limited tests. Restart to change. |
| OpenXR | `DisableLayers` | `*` | Implicit OpenXR layers to turn off for BeamNG only: `*` (all, default), comma-separated names, or empty to keep every layer. See [Other OpenXR layers](#other-openxr-layers). |
| Renderer | `API` | `d3d12` | `d3d12` starts BeamNG with `-gfx d3d12`, the game's VR default since 0.40. Foveation (fixed or eye-tracked) uses D3D12 VRS Tier 2 there; scissor, lens mask and outline also work, and `AllPasses` does not apply. `vulkan` starts `-gfx vk` with the original Vulkan layer. See [Direct3D 12](#direct3d-12). |
| Logging | `Level` | `info` | `off`, `errors`, `info`, or `debug`. Default is brief status/errors, not verbose diagnostics. |
| Tracking | `GracePeriodMs` | `30` | Hold the last valid gaze for this many milliseconds during temporary tracking loss (0 to 1000). |
| Tracking | `GazeRefreshMs` | `8` | Minimum CPU gaze polling interval (2 to 50 ms). This does not measure eye-to-display latency. |

Use a dot for decimals. Boolean values accept `true`/`false` or `1`/`0`; section/key names and shape values are case-insensitive. Put `;` or `#` comments on their own lines. Duplicate keys, unknown settings, empty files and invalid values are rejected. Omitted settings use the defaults above. Each layer loads the file once when first needed; editing a running game's file does not change its cached settings.

The launcher loads both layers; `[Foveation] Enabled=true` activates the per-eye VRS pipeline and eye sampling. It keeps lazy framebuffer creation and asynchronous updates enabled, stencil disabled, the selected logging level, and a 350 ms debug-outline pause after resolution changes. Legacy feature gates are internal details; users do not need to configure them. Eye-following foveation falls back to fixed center when valid gaze is unavailable after the grace period.

With the default `Shape=circle`, `Size1x1=0.25` makes the sharp circle's diameter 25% of eye-image height; `Size2x2=0.55` sets the medium-quality outer diameter to 55%. Shading uses 4x4 outside it. Ellipse instead scales each diameter to the corresponding eye-image width or height. Sizes do not resize the render targets. GPU savings depend on shaders and GPU bottlenecks; no measured FPS increase is claimed.

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
reduce CPU simulation work or GPU shading cost. It was on by default in 0.1.0 and is off by default since 0.2.0. It can be
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

## Direct3D 12

BeamNG 0.40 renders VR with Direct3D 12 and, by default, draws most of each eye's
scene at half the headset resolution before upscaling it. With `[Renderer]
API=d3d12` (default) the OpenXR layer receives the game's D3D12 device and applies
a shading-rate image (VRS Tier 2, 16-pixel tiles on tested GPUs) to every render
target with the eye image's shape at 25-100% of its size, so both the internal
render size and the full-size passes are covered. The same `[Foveation]` sizes,
shape and rates apply, and `[Tracking]` gaze settings move the sharp region with
eye tracking. Both eyes share the game's intermediate targets, so each tile uses
the finer rate of the two eyes; the sharp region is the union of both eyes' regions.

`[Scissor]`, `[LensMask]` and `[Debug]` also work with Direct3D 12 (`M8.43` lines):

- **Scissor** (`M8.44` FOV crop): the margins are removed from the field of view
  that `xrLocateViews` reports and from the recommended eye-image size, so BeamNG
  renders only the kept part of each eye in every pass, compute and FSR
  included. The headset compositor places the smaller views by their reported
  field of view, so nothing moves; the removed area is simply not shown.
  `[Foveation]` sizes stay relative to the full eye (`M8.45`), so the sharp
  area covers the same part of the lens with or without the crop. Each
  eye uses its own margins (`LeftEyeLeft` and so on). Clipping the game's
  scissors saved nothing on D3D12, because BeamNG 0.40 sets its scissors through
  a path the layer does not see. Because the targets serve both eyes,
  rendering keeps what either eye needs (with the defaults, only `Top` and
  `Bottom` save work there); each eye's own edges, `LeftEyeLeft` included, are
  then cleared to black on its final image.
- **LensMask**: tiles entirely outside the lens ellipse get the coarsest
  shading rate, and the corners are cleared to black on each final eye image.
  The ellipse stays relative to the full eye image when Scissor crops the view.
  Unlike Vulkan, there is no depth stamp, so geometry there is still drawn at
  the coarse rate.
- **DrawOutline**: the red 1x1 and orange 2x2 boundaries are drawn on each final
  eye image at that eye's gaze position, on the tile grid of the game's internal
  render size. It needs foveation to be enabled.

A GPU without VRS Tier 2 keeps normal rendering and logs
`M8.42 D3D12 foveation unavailable`; the FOV crop and the black lens corners still work. `Diagnose-BeamNG-VRS.cmd` also logs the VRS
tier and the render-target sizes the game binds per frame (`M8.41` lines).

## Other OpenXR layers

Overlays, toolkits and streaming add-ons often install *implicit* OpenXR API
layers, which the OpenXR loader puts into every VR game. They sit above this
project's layer, closest to the game, so BeamNG-VRS cannot filter what they
receive. If one of them fails, for example because it does not support the
game's graphics API, BeamNG cannot create its OpenXR instance or session and
stays on the desktop.

By default every implicit layer is turned off for BeamNG (`DisableLayers=*`,
also used when the setting is omitted). To keep one you rely on, list only the
layers to turn off, or leave the value empty to keep all of them:

```ini
[OpenXR]
DisableLayers=XR_APILAYER_EXAMPLE_overlay, XR_APILAYER_EXAMPLE_toolkit
```

The launcher reads the installed implicit layers from
`HKLM` and `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` and sets each
listed layer's own `disable_environment` variable in BeamNG's process only.
Nothing in the registry changes and other games keep the layers. `*` turns off
every implicit layer; use it once to check whether a layer is the cause, then
name only the one you need off. `Diagnose-BeamNG-VRS.cmd` and
`Launch-BeamNG-VRS.ps1 -CheckOnly` print each layer's name and whether it is
active, off in the registry, or disabled for BeamNG. Diagnostics also set
`XR_LOADER_DEBUG=info`, so the saved log shows the layer chain and the
`xrCreateInstance` result. The setting applies only when BeamNG is started by
this launcher.

## Existing developer launch scripts

Existing `BEAMNG_VRS_*` and `BEAMNG_EYE_*` process environment variables continue to override corresponding INI values. The public launcher clears these project-specific settings only in the child process, so its INI is authoritative and the parent shell remains unchanged. The old `BEAMNG_VRS_CENTER_RADIUS` and `BEAMNG_VRS_MID_RADIUS` variables map to `Size1x1` and `Size2x2`; their names remain for compatibility, while their values represent diameters.

For custom layer-loading setups, the DLLs prefer `BeamNG-VRS.ini` in the parent of their module folder, with a fallback to an older copy beside the modules if the root file is absent. `BEAMNG_VRS_CONFIG` selects a different absolute file path, including Unicode paths. The public launcher explicitly selects the root file. Without a file, the old environment-only behavior is retained. Invalid files disable the INI activation gates and emit a debugger diagnostic; existing environment overrides still take precedence. The packaged launcher instead rejects the file before launch and displays the parser error.
