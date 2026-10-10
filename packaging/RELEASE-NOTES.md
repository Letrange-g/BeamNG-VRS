BeamNG-VRS 0.1 — first public Windows x64 release.

Foveated VRS keeps detail around the fixed center or supported eye tracking.
Optional scissor cropping reduces rendering around each eye's edges. VRS can
be disabled to use cropping alone; optional Turbo overlaps OpenXR frame waiting.

## Install

1. Download **BeamNG-VRS-0.1.0-windows-x64.zip** below.
2. Close BeamNG and extract the whole ZIP into the game folder, beside `Bin64`.
   Extraction directly inside `Bin64` also works.
3. Edit **BeamNG-VRS.ini** beside the CMD, then run **Launch-BeamNG-VRS.cmd**.
4. Enable VR normally. Restart after changing settings.

Requires Windows 10/11 x64, BeamNG Vulkan VR, an OpenXR runtime, and suitable
GPU/driver VRS support for foveation. Windows PowerShell 5.1 is included in
Windows 10/11. The package contains no EXEs and needs no administrator rights
or global layer registration. Back up your INI before updating; update the
launcher and both DLLs together.

## Defaults and options

- VRS enabled: `Size1x1=0.25`, `Size2x2=0.60`, `Shape=circle`.
- Scissor disabled: top/bottom `0.30`, outer edges `0.10`, nose-side edges `0`.
- Turbo enabled. It may interfere with motion reprojection; compare smoothness
  with it off/on using the same settings. Set `TurboMode=false` for normal timing.
- Debug outlines disabled, brief `info` logging, 30 ms gaze grace and 8 ms polling.
- Use `[Foveation] Enabled=false` for scissor-only. Try BeamNG's MSAA if VRS
  shimmers; its effect and performance cost depend on your setup.

[Settings](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/CONFIGURATION.md) ·
[Help](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/TECHNICAL.md) ·
[Source guide](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/SOURCE-GUIDE.md)

The source commit and build details are included in `BUILD-INFO.txt`; archive
and individual file SHA-256 checksums are provided. Both DLLs are built with
Visual Studio 2022 and tested on Windows before packaging. Automated tests do
not establish FPS gains or compatibility with every headset/runtime. Earlier
in-game testing used a Pimax Dream Air and RTX 5090; Turbo was reported smooth
with reprojection disabled. These renderer modifications remain experimental.
