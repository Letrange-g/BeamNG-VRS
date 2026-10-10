Experimental Windows x64 beta with independent horizontal cropping for each eye.

Beta.9 adds four `[Scissor]` settings, all defaulting to `0`:

- `LeftEyeLeft`: left eye, outside.
- `LeftEyeRight`: left eye, near the nose.
- `RightEyeLeft`: right eye, near the nose.
- `RightEyeRight`: right eye, outside.

For outside-only cropping, try `LeftEyeLeft=0.05` and `RightEyeRight=0.05`,
leaving both nose-side settings at `0`. Set `Enabled=true`, restart and check
both eyes in your headset. Each edge accepts 0 to 0.49; top/bottom stay shared
at 0.30 by default. Scissor remains disabled by default.

Old shared `Left`/`Right` keys still work. Explicit per-eye settings override
them independently. Update the launcher and both DLLs together. The new crop
uses classified eye images for dynamic scissors, matching static pipeline
copies and border clears. Unknown eye targets keep the area either eye needs.

1. Download the **Windows x64 ZIP** under Assets, not the source-code ZIP.
2. Close BeamNG and extract the entire ZIP beside `Bin64`, or inside `Bin64`.
3. Edit `BeamNG-VRS.ini` beside the launch CMD, then run `Launch-BeamNG-VRS.cmd`.
4. Enable VR normally. Restart after every settings change.

Back up your INI before updating. Restore it beside the launch CMD; older nested
copies are ignored. The ZIP contains no EXEs and needs no administrator rights.
Windows PowerShell 5.1 is included in Windows 10/11. Delete an old
`BeamNG-VRS-Check.exe` if present from a previous installation.

The GitHub front page and packaged diagram now focus on installation and settings.
[Settings guide](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/CONFIGURATION.md) ·
[Help and requirements](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/TECHNICAL.md) ·
[Performance details](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/PERFORMANCE.md)

Built and tested on Windows with Visual Studio 2022. The exact source commit is
in `BeamNG-VRS/BUILD-INFO.txt`. This beta still needs in-game headset testing;
automated checks do not establish appearance or FPS gains.
