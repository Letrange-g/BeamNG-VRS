# Configuration

The release ZIP puts `BeamNG-VRS.ini` beside `Launch-BeamNG-VRS.cmd` at the extraction root. Edit it in Notepad and restart BeamNG. The launcher validates it before starting the game. No INI editor application is required. When upgrading from beta.7 or earlier, restore your saved INI here; the old nested copy is not used by the new launcher.

| Section | Setting | Default | Meaning |
| --- | --- | --- | --- |
| Foveation | `Size1x1` | `0.25` | Inner full-quality region size (0 to 1); ellipse uses width/height-relative diameters, circle uses height, square uses height-relative side length. |
| Foveation | `Size2x2` | `0.60` | Outer size of the medium-quality region (0 to 1); must be at least `Size1x1`. Outside it, shading uses 4x4. |
| Foveation | `Shape` | `ellipse` | `circle`, `square`, or `ellipse`. Square uses side length; ellipse uses eye-width-relative horizontal diameter and eye-height-relative vertical diameter. |
| Debug | `DrawOutline` | `false` | Draw the red 1x1 boundary and orange 2x2-to-4x4 boundary. |
| Scissor | `Enabled` | `false` | Experimental four-edge crop. Test visible edges and frame times before enabling. |
| Scissor | `Top` | `0.30` | Fraction of eye-image height cropped from the top when scissor is enabled (0 to 0.49). |
| Scissor | `Bottom` | `0.30` | Fraction cropped from the bottom (0 to 0.49). |
| Scissor | `Left` | `0` | Fraction of eye-image width cropped from the left (0 to 0.49). Requires beta.8 or newer. |
| Scissor | `Right` | `0` | Fraction of eye-image width cropped from the right (0 to 0.49). Requires beta.8 or newer. |
| Tracking | `GracePeriodMs` | `30` | Hold the last valid gaze for this many milliseconds during temporary tracking loss (0 to 1000). |
| Tracking | `GazeRefreshMs` | `8` | Minimum CPU gaze polling interval (2 to 50 ms). This does not measure eye-to-display latency. |

Use a dot for decimals. Boolean values accept `true`/`false` or `1`/`0`; section/key names and shape values are case-insensitive. Put `;` or `#` comments on their own lines. Duplicate keys, unknown settings, empty files and invalid values are rejected. Omitted settings use the defaults above. Each layer loads the file once when first needed; editing a running game's file does not change its cached settings.

The launcher activates the existing per-eye VRS pipeline and eye sampling automatically. It keeps lazy framebuffer creation and asynchronous updates enabled, stencil disabled, quiet logging, and a 350 ms debug-outline pause after resolution changes. Legacy feature gates are internal details; users do not need to configure them. Eye-following foveation falls back to fixed center when valid gaze is unavailable after the grace period.

With `Size1x1=0.25`, `Size2x2=0.60`, and `Shape=ellipse`, the full-quality ellipse spans 25% of each eye image's width and height. The 2x2 region extends to 60%; 4x4 applies outside it. Sizes are not radii and do not resize the eye render targets. GPU savings depend on shaders and GPU bottlenecks; no measured FPS increase is claimed.

## Scissor crop

When scissor is enabled, start horizontal cropping at `Left=0.05` and
`Right=0.05` and inspect both eyes for visible black borders. Cropping reduces
pixel coverage in matching eye passes; it does not cull scene objects or change
the camera projection. With top/bottom 0.30 and left/right 0.05, approximately
36% of the eye-image area remains, versus 40% with the horizontal defaults.
This area reduction is not a predicted FPS gain. Restart after each adjustment.
Old INIs that omit `Left` and `Right` retain zero horizontal crop.

## Existing developer launch scripts

Existing `BEAMNG_VRS_*` and `BEAMNG_EYE_*` process environment variables continue to override corresponding INI values. The public launcher clears these project-specific settings only in the child process, so its INI is authoritative and the parent shell remains unchanged. The old `BEAMNG_VRS_CENTER_RADIUS` and `BEAMNG_VRS_MID_RADIUS` variables map to `Size1x1` and `Size2x2`; their names remain for compatibility, while their values represent diameters.

For custom layer-loading setups, the DLLs prefer `BeamNG-VRS.ini` in the parent of their module folder, with a fallback to an older copy beside the modules if the root file is absent. `BEAMNG_VRS_CONFIG` selects a different absolute file path, including Unicode paths. The public launcher explicitly selects the root file. Without a file, the old environment-only behavior is retained. Invalid files disable the INI activation gates and emit a debugger diagnostic; existing environment overrides still take precedence. The packaged launcher instead rejects the file before launch and displays the parser error.
