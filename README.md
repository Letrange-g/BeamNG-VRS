# BeamNG-VRS (experimental source)

An **unofficial, experimental** pair of Windows API layers for **BeamNG.drive** Vulkan VR rendering:

- **`BeamNG_VRS.dll`** – Vulkan layer applying per-eye fragment shading-rate patterns.
- **`BeamNG_EyeProbe.dll`** – OpenXR API layer collecting gaze/eye-swapchain information when supported.

The project supports fixed center foveation without eye tracking, and eye-following foveation when an OpenXR eye-tracking source is available. Center and middle regions can be **circle**, **square**, or **ellipse**; sizes are **diameters (or square side lengths) relative to eye-image height** rather than radii. The debug view can show a red 1×1 boundary and an orange 2×2-to-4×4 boundary.

## Status

**Research beta source, not a plug-and-play release.** The latest source snapshot contains M8.26 dual outlines on top of M8.25 gaze grace, M8.24 region shapes, M8.23 lazy framebuffer creation, and M8.21 faster gaze polling. Testing has primarily covered a Pimax Dream Air and NVIDIA RTX 5090. Windows/MSVC builds and compatibility with other headsets/GPUs **are not yet validated from this repository**. Do not assume support for arbitrary games, headsets, Vulkan drivers, or VR runtimes.

The project still uses **environment variables**, not an INI loader. The example launcher and `.ini` editor discussed in development are future work. **No compiled DLLs are included** in this repository.

## Build

See [docs/BUILDING.md](docs/BUILDING.md) for required SDKs, CMake configuration and loader manifests. There is no globally installed layer and no game-file patching needed for the source workflow.

## Configuration

See [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for key environment variables and safer defaults. Common controls include:

```powershell
$env:BEAMNG_VRS_SHAPE = "circle"
$env:BEAMNG_VRS_CENTER_RADIUS = "0.25"  # diameter, despite legacy name
$env:BEAMNG_VRS_MID_RADIUS = "0.60"     # diameter, despite legacy name
$env:BEAMNG_VRS_MID_RATE = "2x2"
$env:BEAMNG_VRS_OUTER_RATE = "4x4"
$env:BEAMNG_VRS_GAZE_UPDATE_MS = "8"
$env:BEAMNG_VRS_GAZE_GRACE_MS = "0"
$env:BEAMNG_VRS_M823_LAZY_FB = "1"
$env:BEAMNG_VRS_DEBUG_OUTLINE = "0"
$env:BEAMNG_VRS_M818_STENCIL = "0"
$env:BEAMNG_VRS_LOG_LEVEL = "1"
```

Restart BeamNG when changing environment settings. Variable names like `CENTER_RADIUS` are retained for compatibility but their behavior changed in M8.24: **0.5 means a diameter equal to half the eye-image height** for circle/square.

## Notes and limitations

- Both layers are experimental and can cause application or headset-runtime instability.
- A Vulkan driver must support the required attachment shading-rate features. Unsupported configurations need testing and should fail safely.
- OpenXR visibility masks were unavailable in tested sessions; the experimental stencil implementation is **disabled** in the recommended configuration. Scissor crop is also experimental.
- Faster gaze polling and CPU source-to-upload measurements do **not** measure eye-to-display latency.
- The debug outline is injected into game rendering, and may appear in temporal history; disable it for normal gameplay.
- Do not distribute log files, dumps, private headset identifiers, or graphics captures without reviewing them.
- The code is not an official BeamNG or headset-vendor project. No affiliation or endorsement is implied.

## Privacy and contributions

Only curated source, manifests, and portable tests are included. No user logs, executables, graphics captures, API keys or personal settings are committed. See [docs/SECURITY.md](docs/SECURITY.md) before reporting an issue.

**License:** not selected yet. Until a license is chosen, the repository is source-available to its owner but no public reuse license is granted. Decide on a license before making a public release.
