Experimental Windows x64 beta with portable installation and streamlined INI settings.

This update fixes game discovery when the ZIP is extracted directly inside `Bin64`. Both installation locations are now supported. The launcher uses Windows PowerShell 5.1, included by default in Windows 10 and 11; PowerShell 7 is not required.

The launcher now preserves normal Vulkan layer discovery. For a rendering-device startup error, run `Diagnose-BeamNG-VRS.cmd` and close the game/error dialog to collect the loader log at the printed path. Run under your normal user account. This adds troubleshooting information; it does not establish the cause of every rendering-device error.

1. Download the **BeamNG-VRS Windows x64 `.zip`** under Assets below. The GitHub source-code archives do not contain DLLs.
2. Close BeamNG and extract the entire ZIP into its installation root, beside `Bin64`, or directly inside `Bin64`. Keep `Launch-BeamNG-VRS.cmd` and the `BeamNG-VRS` folder together.
3. Edit `BeamNG-VRS/BeamNG-VRS.ini` if desired, then run `Launch-BeamNG-VRS.cmd`.
4. Enable VR in BeamNG as usual. Restart the game after changing settings.

The package includes both DLLs, the INI validator, installation/uninstall instructions, an MIT license, a visual settings guide, and checksums. No administrator rights or global layer registration are required. Start Steam first if your game installation needs it.

Defaults: `Size1x1=0.25`, `Size2x2=0.60`, `Shape=circle`, `DrawOutline=false`, scissor disabled (top/bottom `0.20`), `GracePeriodMs=30`, and `GazeRefreshMs=8`.

Built on Windows with Visual Studio 2022. The portable tests, Windows configuration tests, DLL loading/export checks, and launcher integration test passed before packaging. The exact source commit is recorded in `BeamNG-VRS/BUILD-INFO.txt`.

This version still needs testing in BeamNG with the target GPU/headset. It is unsigned and experimental; no universal compatibility or FPS gain is claimed. Launch BeamNG normally to stop using the layers.
