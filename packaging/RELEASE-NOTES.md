BeamNG-VRS 0.3 — portable Windows x64, for BeamNG.drive 0.40.

## What's new

- **Moved to Direct3D 12 VR.** BeamNG 0.40 renders VR with Direct3D 12, and the
  launcher now starts it that way. Foveation (fixed or eye-tracked) needs a GPU
  with D3D12 VRS Tier 2. Vulkan still works with `[Renderer] API=vulkan`.
- **New option to force the VR render scale.** BeamNG 0.40 renders VR at 50%;
  `[Renderer] RenderScale` (0.25 to 1.0) sets your own. 1.0 is much sharper but
  costs more GPU. The launcher applies it with a small mod in your BeamNG user
  folder and removes the mod when the line is commented out.

## Install

1. Download **BeamNG-VRS-0.3.0-windows-x64.zip** below.
2. Close BeamNG and extract the whole ZIP into the game folder, beside `Bin64`.
   Extraction directly inside `Bin64` also works.
3. Edit **BeamNG-VRS.ini** beside the CMD, then run **Launch-BeamNG-VRS.cmd**.
4. Enable VR normally. Restart after changing settings.

Updating from 0.2: back up your INI, extract the new ZIP, then copy your
values into the new INI so you get the new `[OpenXR]` and `[Renderer]` sections.
Update the launcher and both DLLs together.

Requires Windows 10/11 x64, BeamNG.drive 0.40 VR (Direct3D 12, or Vulkan), an
OpenXR runtime, and GPU/driver VRS support for foveation. Windows PowerShell 5.1
is included in Windows 10/11. The package contains no EXEs and needs no
administrator rights or global layer registration.

## Defaults and options

- VRS enabled on all eye-sized passes: `Size1x1=0.25`, `Size2x2=0.55`, `Shape=circle`.
- Scissor enabled: top/bottom `0.30`, outer edges `0.10`, nose-side edges `0`.
  Check the edges in your headset and reduce the values if content is cut off.
- Lens mask enabled, `Size=1.15`.
- Turbo off. `TurboMode=true` may help when the CPU is the limit; it can
  interfere with motion reprojection.
- Direct3D 12 renderer, other OpenXR layers off for BeamNG only (`DisableLayers=*`),
  game's own VR render scale. On Direct3D 12, scissor crops the field of view.
- Debug outlines disabled, brief `info` logging, 30 ms gaze grace and 8 ms polling.
- If VRS shimmers on fine detail, try BeamNG's MSAA (`AllPasses=false` on Vulkan).
- **Ctrl+Shift+V** pauses VRS in game; the log shows GPU time per frame for comparison.

[Settings](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/CONFIGURATION.md) ·
[Help](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/TECHNICAL.md) ·
[Source guide](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/SOURCE-GUIDE.md)

The source commit and build details are included in `BUILD-INFO.txt`; archive
and individual file SHA-256 checksums are provided. Both DLLs are built with
Visual Studio 2022 and tested on Windows before packaging. Automated tests do
not establish FPS gains or compatibility with every headset/runtime. Earlier
in-game testing used a Pimax Dream Air and RTX 5090. These renderer modifications remain experimental.
