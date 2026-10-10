# BeamNG-VRS (experimental source)

An **unofficial, experimental** pair of Windows API layers for **BeamNG.drive** Vulkan VR rendering:

- **`BeamNG_VRS.dll`** – Vulkan layer applying per-eye fragment shading-rate patterns.
- **`BeamNG_EyeProbe.dll`** – OpenXR API layer collecting gaze/eye-swapchain information when supported.

The project supports fixed center foveation without eye tracking, and eye-following foveation when an OpenXR eye-tracking source is available. Center and middle regions can be **circle**, **square**, or **ellipse**; sizes are **diameters (or square side lengths) relative to eye-image height** rather than radii. The debug view can show a red 1×1 boundary and an orange 2×2-to-4×4 boundary.

## Status

**Experimental beta.** The latest source snapshot contains M8.26 dual outlines on top of M8.25 gaze grace, M8.24 region shapes, M8.23 lazy framebuffer creation, and M8.21 faster gaze polling. Earlier rendering tests primarily covered a Pimax Dream Air and NVIDIA RTX 5090. The Windows/MSVC build and automated configuration, DLL loading, and launcher checks pass in GitHub Actions. The new INI/launcher package still needs testing in BeamNG with the target GPU/headset. Do not assume support for arbitrary games, headsets, Vulkan drivers, or VR runtimes.

The layers read **`BeamNG-VRS.ini`** beside the DLLs. A portable ZIP launcher and a Windows build/package workflow are included. **No compiled DLLs are committed** in this repository; binary beta packages are distributed through GitHub Releases.

## Portable installation

1. Download the Windows x64 ZIP from [Releases](https://github.com/Letrange-g/BeamNG-VRS/releases) when a beta is available. Use the binary ZIP, not GitHub's source-code ZIP.
2. Close BeamNG and extract the entire ZIP into its installation root, beside `Bin64`. Extraction directly inside `Bin64` also works; keep `Launch-BeamNG-VRS.cmd` and the `BeamNG-VRS` folder together.
3. Edit `BeamNG-VRS/BeamNG-VRS.ini` in Notepad if desired.
4. Start Steam if your installation requires it, then double-click `Launch-BeamNG-VRS.cmd`.
5. Enable VR in the game as usual. Restart after changing settings.

The launcher validates settings, starts the game in Vulkan mode, and activates both layers for that process. The ZIP is for Windows x64 and remains an experimental beta; see the requirements below before using it.

The launcher uses Windows PowerShell 5.1, included by default in Windows 10 and 11. PowerShell 7 is not required. `BeamNG-VRS-Check.exe` validates your INI automatically before launch; keep it with the other package files.

The ZIP keeps its DLLs and settings inside `BeamNG-VRS/`; it does not replace original game files or register layers globally. Launch BeamNG normally to stop using the layers. To uninstall, close the game and remove that folder and the launch shortcut. Back up the INI before extracting an update.

## Build

See [docs/BUILDING.md](docs/BUILDING.md) for required SDKs, CMake configuration and loader manifests. There is no globally installed layer and no game-file patching needed for the source workflow.

## Configuration

See [docs/CONFIGURATION.md](docs/CONFIGURATION.md). The user-facing controls are:

![Illustration of VRS region sizes, debug outlines, scissor cropping and gaze timing](docs/images/vrs-settings.png)

```ini
[Foveation]
Size1x1=0.25
Size2x2=0.60
Shape=circle

[Debug]
DrawOutline=false

[Scissor]
Enabled=false
Top=0.20
Bottom=0.20

[Tracking]
GracePeriodMs=30
GazeRefreshMs=8
```

Restart BeamNG after editing. Sizes are diameters or square side lengths relative to eye-image height: **0.5 means half the eye-image height** for circle/square. The public launcher uses the INI settings; existing environment overrides remain supported when loading the layers through developer scripts.

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

**License:** [MIT](LICENSE).
