Experimental Windows x64 beta with portable installation and streamlined INI settings.

1. Download the **BeamNG-VRS Windows x64 `.zip`** under Assets below. The GitHub source-code archives do not contain DLLs.
2. Close BeamNG and extract the entire ZIP into its installation root, beside `Bin64`.
3. Edit `BeamNG-VRS/BeamNG-VRS.ini` if desired, then run `Launch-BeamNG-VRS.cmd`.
4. Enable VR in BeamNG as usual. Restart the game after changing settings.

The package includes both DLLs, the INI validator, installation/uninstall instructions, an MIT license, a visual settings guide, and checksums. No administrator rights or global layer registration are required. Start Steam first if your game installation needs it.

Defaults: `Size1x1=0.25`, `Size2x2=0.60`, `Shape=circle`, `DrawOutline=false`, scissor disabled (top/bottom `0.20`), `GracePeriodMs=30`, and `GazeRefreshMs=8`.

Built on Windows with Visual Studio 2022. The portable tests, Windows configuration tests, DLL loading/export checks, and launcher integration test passed before packaging. The exact source commit is recorded in `BeamNG-VRS/BUILD-INFO.txt`.

This version still needs testing in BeamNG with the target GPU/headset. It is unsigned and experimental; no universal compatibility or FPS gain is claimed. Launch BeamNG normally to stop using the layers.
