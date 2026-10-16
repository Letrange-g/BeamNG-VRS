BeamNG-VRS 0.2 — portable Windows x64.

## What's new

- **VRS now covers the whole scene.** 0.1 only foveated each eye's final
  image. 0.2 also foveates the game's internal eye-sized passes (scene,
  lighting, post effects), which is where most GPU time goes. Those passes use
  the finer rate of both eyes per tile. `[Foveation] AllPasses=false` restores
  the old behaviour for comparison.
- **Lens-corner mask.** `[LensMask]` skips rendering the corners outside an
  ellipse that the headset lenses hide (`Size=1.15` by default; increase it if
  you see black corners, lower it toward 1.05 to skip more).
- **New defaults:** `Size2x2=0.55`, scissor enabled, lens mask enabled, Turbo
  off (it showed no gain when the GPU is the limit).
- **Diagnostics:** the info log now reports which passes receive VRS
  (`M8.32`, `M8.33`) and lens-mask activity (`M8.34`).

In testing on a Pimax Dream Air and RTX 5090, BeamNG at VR resolution 0.75
reached 90 FPS with VRS alone, without scissor. Results depend on headset,
GPU, scene and settings.

## Install

1. Download **BeamNG-VRS-0.2.0-windows-x64.zip** below.
2. Close BeamNG and extract the whole ZIP into the game folder, beside `Bin64`.
   Extraction directly inside `Bin64` also works.
3. Edit **BeamNG-VRS.ini** beside the CMD, then run **Launch-BeamNG-VRS.cmd**.
4. Enable VR normally. Restart after changing settings.

Updating from 0.1: back up your INI, extract the new ZIP, then copy your
values into the new INI so you get the new `AllPasses` and `[LensMask]` keys.
Update the launcher and both DLLs together.

Requires Windows 10/11 x64, BeamNG Vulkan VR, an OpenXR runtime, and suitable
GPU/driver VRS support for foveation. Windows PowerShell 5.1 is included in
Windows 10/11. The package contains no EXEs and needs no administrator rights
or global layer registration.

## Defaults and options

- VRS enabled on all eye-sized passes: `Size1x1=0.25`, `Size2x2=0.55`, `Shape=circle`.
- Scissor enabled: top/bottom `0.30`, outer edges `0.10`, nose-side edges `0`.
  Check the edges in your headset and reduce the values if content is cut off.
- Lens mask enabled, `Size=1.15`.
- Turbo off. `TurboMode=true` may help when the CPU is the limit; it can
  interfere with motion reprojection.
- Debug outlines disabled, brief `info` logging, 30 ms gaze grace and 8 ms polling.
- If VRS shimmers on fine detail, try `AllPasses=false` or BeamNG's MSAA.

[Settings](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/CONFIGURATION.md) ·
[Help](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/TECHNICAL.md) ·
[Source guide](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/SOURCE-GUIDE.md)

The source commit and build details are included in `BUILD-INFO.txt`; archive
and individual file SHA-256 checksums are provided. Both DLLs are built with
Visual Studio 2022 and tested on Windows before packaging. Automated tests do
not establish FPS gains or compatibility with every headset/runtime. Earlier
in-game testing used a Pimax Dream Air and RTX 5090. These renderer modifications remain experimental.
