Experimental Windows x64 beta with optional turbo, VRS on/off and logging controls.

New in beta.11:

- Default shape is now `circle`.
- Outer horizontal scissor edges default to `0.10`; nose-side edges stay `0`.
  Scissor remains disabled by default. Explicit saved settings are retained.

Included from beta.10:

- `[Performance] TurboMode=true` overlaps CPU work with the next OpenXR runtime
  wait. Default `false`. Compare smoothness and frame times as well as FPS;
  motion reprojection may be incompatible. Restart to change it.
- `[Foveation] Enabled=false` disables VRS while keeping scissor and turbo
  available. Default `true`. The two sections' Enabled keys are independent.
- `[Logging] Level=off` disables this project's layer logs. Other choices are
  `errors`, `info` (default) and `debug`. The diagnostic launcher overrides this
  setting with detailed logging.
- A [source guide](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/SOURCE-GUIDE.md)
  and comments explain the main rendering, crop, gaze, upload and turbo paths.

The four independent horizontal crop controls from beta.9 remain available,
with the defaults above. Top/bottom remain 0.30 and scissor remains off by default.
Quality-region sizes and tracking defaults are retained.

1. Download the **Windows x64 ZIP** under Assets, not a source-code archive.
2. Back up your INI, close BeamNG and extract the whole ZIP beside or inside `Bin64`.
3. Restore/edit `BeamNG-VRS.ini` beside the CMD. Update both DLLs and the launcher together.
4. Run `Launch-BeamNG-VRS.cmd` and enable VR normally. Restart after changes.

Windows PowerShell 5.1 is included in Windows 10/11. No EXEs, administrator
rights or global layer registration are needed. Delete an old checker EXE if
left over from earlier packages.

[Settings](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/CONFIGURATION.md) ·
[Help](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/TECHNICAL.md) ·
[Performance details](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/PERFORMANCE.md)

Built and tested on Windows with Visual Studio 2022. Tests cover normal timing,
one-frame overlap, real frame ordering, failure fallback, extension chains,
existing app pipelining, swapchain/session teardown, configuration and launcher
behavior. The source commit is in BUILD-INFO.txt. Turbo still needs in-game
headset testing; no FPS or latency improvement is claimed from automated tests.
