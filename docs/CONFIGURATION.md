# Configuration (environment variables in current implementation)

Configuration is currently read from the **BeamNG process environment**; there is no INI parser yet. Restart the game for changes.

| Variable | Example | Meaning |
| --- | --- | --- |
| `BEAMNG_VRS_SHAPE` | `circle` | `circle`, `square`, or `ellipse`. |
| `BEAMNG_VRS_CENTER_RADIUS` | `0.25` | Legacy name: **diameter/side length**, from `0.0` to `1.0`. Circle and square are measured relative to eye render **height**. |
| `BEAMNG_VRS_MID_RADIUS` | `0.60` | Outer diameter/side of the medium-quality zone; same scale. |
| `BEAMNG_VRS_MID_RATE` | `2x2` | Medium-zone fragment shading rate; also accepts `1x1`, `4x4`. |
| `BEAMNG_VRS_OUTER_RATE` | `4x4` | Peripheral fragment shading rate. |
| `BEAMNG_VRS_FOVEATION_MODE` | `eye` | Use eye tracking when available (subject to OpenXR). Without valid tracking, fixed center applies. |
| `BEAMNG_VRS_GAZE_UPDATE_MS` | `8` | Minimum CPU gaze poll interval in milliseconds. |
| `BEAMNG_VRS_GAZE_GRACE_MS` | `0` | Hold the last valid gaze when tracking is temporarily unavailable (`0` avoids a perceived freeze). |
| `BEAMNG_VRS_M823_LAZY_FB` | `1` | Create extra eye framebuffer variants only when needed. |
| `BEAMNG_VRS_DEBUG_OUTLINE` | `0` | Red 1×1 edge; orange 2×2-to-4×4 edge (only with corresponding rates). |
| `BEAMNG_VRS_OUTLINE_RESIZE_PAUSE_MS` | `350` | Delay debug border after a resolution change to avoid ghosts. |
| `BEAMNG_VRS_SCISSOR_ENABLE` | `0` | Experimental cropping; keep off until GPU frame-time impact and visible edges are measured. |
| `BEAMNG_VRS_M818_STENCIL` | `0` | Experimental hidden-area stencil; not beneficial on tested runtime. |
| `BEAMNG_VRS_LOG_LEVEL` | `1` | Quiet logging. Level `2` is very verbose and can affect frame pacing. |

## Legacy feature gates

This repository retains low-level opt-in flags from earlier milestones, such as `BEAMNG_VRS_M8_EXPERIMENTAL`, `BEAMNG_VRS_M812_LIVE_GAZE`, `BEAMNG_VRS_M815_ASYNC`, and `BEAMNG_VRS_M89_STATIC_TEST`. They are **not yet replaced by a single user-facing enable switch**. Advanced testers should preserve the known-good settings from their own working build while validating each change. A starter/launcher with safe consolidated defaults is planned.

## Region size example

With `CENTER_RADIUS=0.25`, `MID_RADIUS=0.60`, and `SHAPE=circle`, the central 1×1 circle has a diameter equal to 25% of each eye render **height**. The 2×2 ring extends to a circle 60% of that height; outside the ring the setting `OUTER_RATE=4x4` applies. Both eye regions start at the center on headsets without eye tracking.

## What users should not expect

These settings alter **fragment shader invocation rates**; they do not resize render targets or automatically reduce CPU draw calls. GPU savings depend on the game shaders and GPU bottlenecks. No benchmark-derived FPS increase is claimed.
