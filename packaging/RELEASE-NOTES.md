BeamNG-VRS 0.4.2 — portable Windows x64, for BeamNG.drive 0.40.

## What's new in 0.4.2

- **Multiplayer (BeamMP) support.** Double-click the new
  **Launch-BeamMP-VRS.cmd** instead of `BeamMP-Launcher.exe`. It sets up
  BeamNG-VRS as usual and starts BeamMP, which then starts BeamNG with
  foveation, the crop, the lens mask and the VR draw distance from your INI.
  Starting BeamMP the usual way runs the game without BeamNG-VRS.
  BeamMP is looked for in `%APPDATA%\BeamMP-Launcher`; if you installed it
  elsewhere, edit the CMD and add `-BeamMPLauncher "D:\path\BeamMP-Launcher.exe"`.
- **On a BeamMP server**, BeamMP turns off every mod the server does not list,
  including BeamNG-VRS's small in-game mod. The radial menu entry and the
  "VR draw distance" app are then gone until you restart; the INI distance (or
  your last in-game pick) still applies. The launcher turns the mod back on at
  every start, so single player is not left without it.

## In 0.4.1

- **Right-eye ghosting fix.** BeamNG 0.40 builds local exposure and bloom from
  one eye and reuses them for the other, so dark or bright "ghost" shapes show
  in the right eye. BeamNG-VRS now patches the game's combine shader so each eye
  reads them at its own matching position. It is **off by default**: set
  `[Renderer] GhostingFix=true` in the INI to try it. The launcher then writes
  the patched shader into your BeamNG user folder and clears the shader cache,
  so the next start takes longer. Set it back to `false` to remove it. Very near
  objects (the cockpit glass) can still be slightly off.
- **The log now says what limits your frame rate.** The `M8.47` GPU line adds
  the frame time, the headset budget, the CPU and wait times and a verdict:
  on target, GPU-bound, CPU-bound or runtime-bound (compositor or streaming,
  such as Virtual Desktop). See "What limits the frame rate" in
  docs/CONFIGURATION.md.

## In 0.4.0

- **VR draw distance.** BeamNG 0.40 draws VR out to 10000 m, further than the
  desktop's 8000 m, and no game setting changes it. BeamNG-VRS now sets it to
  **4000 m by default**. Change it with `[Renderer] VRDrawDistance` (100 to
  10000, or `off` to keep the game's value) in the INI, or live in game from the
  radial menu: **Freeroam Actions > VR draw distance** (500 / 1000 / 2000 /
  4000 / 6000 / Full). A "VR draw distance" UI app with a slider is also added.
  Your in-game pick is kept for the next launch unless you change the INI value.
- **Sharper picture, your choice.** BeamNG 0.40 renders VR at 50% and upscales
  it, which looks soft. Uncomment `RenderScale=1.0` under `[Renderer]` in the INI
  for full resolution (more GPU; 0.67 to 0.8 is a middle ground). With the 4000 m
  draw distance, 1.0 stayed within the 90 Hz budget on an RTX 5090.
- In testing on an RTX 5090 at render scale 1.0, 4000 m looked almost the same as
  the full distance, with GPU time around 7 ms on West Coast and Italy. Short
  distances help most in open maps: 100 m on Utah cut GPU time from 9.1 to 3.8 ms.
- The distance is found in the game's code at every start. After a BeamNG update,
  the log line `M8.50 VR draw distance … patched N of M sites` should show N above 0;
  set `VRDrawDistance=off` if the game ever misbehaves.

## Since 0.2

- **Moved to Direct3D 12 VR.** BeamNG 0.40 renders VR with Direct3D 12, and the
  launcher now starts it that way. Foveation (fixed or eye-tracked) needs a GPU
  with D3D12 VRS Tier 2. Vulkan still works with `[Renderer] API=vulkan`.
- **New option to force the VR render scale.** BeamNG 0.40 renders VR at 50%;
  `[Renderer] RenderScale` (0.25 to 1.0) sets your own. 1.0 is much sharper but
  costs more GPU. The launcher applies it with a small mod in your BeamNG user
  folder and removes the mod when the line is commented out.

## Install

1. Download **BeamNG-VRS-0.4.2-windows-x64.zip** below.
2. Close BeamNG and extract the whole ZIP into the game folder, beside `Bin64`.
   Extraction directly inside `Bin64` also works.
3. Edit **BeamNG-VRS.ini** beside the CMD, then run **Launch-BeamNG-VRS.cmd**
   (or **Launch-BeamMP-VRS.cmd** for BeamMP multiplayer).
4. Enable VR normally. Restart after changing settings.

Updating: back up your INI, extract the new ZIP, then copy your values into
the new INI. An older INI without `VRDrawDistance` also gets the 4000 m default.
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
  game's own VR render scale, VR draw distance 4000 m. On Direct3D 12, scissor
  crops the field of view.
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
