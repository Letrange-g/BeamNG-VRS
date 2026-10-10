Experimental Windows x64 beta with portable installation and streamlined INI settings.

M8.27 adds faster CPU pattern generation, skips byte-identical gaze uploads, reuses completed upload resources, and removes heap allocations from command dispatch. Scissor clears are batched, and live uploads also cover Vulkan Submit2. Live VRS now validates projected-gaze freshness independently of raw-pose diagnostics. Update both DLLs together. See [performance findings](https://github.com/Letrange-g/BeamNG-VRS/blob/main/docs/PERFORMANCE.md) for CPU benchmark results and an in-game comparison procedure; no FPS improvement or render-distance cap is claimed.

The package contains no BeamNG-VRS-Check.exe. The existing PowerShell launcher validates settings before starting BeamNG; no additional EXE is included. Defaults are now Shape=ellipse and Scissor Top=0.30/Bottom=0.30, with scissor disabled. Back up your INI before updating; delete the old BeamNG-VRS-Check.exe from an earlier installation. Existing INI files keep their values when restored.

The package retains Windows relative paths in both manifests, resolving their DLLs beside the JSON files. Windows regression checks cover DLL loading from a different working directory, including spaces and Unicode in the installation path.

Extraction into the game root or directly inside `Bin64` is supported. The launcher uses Windows PowerShell 5.1, included by default in Windows 10 and 11; PowerShell 7 is not required.

The launcher preserves normal Vulkan layer discovery. For a rendering-device startup error, run `Diagnose-BeamNG-VRS.cmd` and close the game/error dialog to collect the loader log at the printed path. Run under your normal user account.

1. Download the **BeamNG-VRS Windows x64 `.zip`** under Assets below. The GitHub source-code archives do not contain DLLs.
2. Close BeamNG and extract the entire ZIP into its installation root, beside `Bin64`, or directly inside `Bin64`. Keep `Launch-BeamNG-VRS.cmd` and the `BeamNG-VRS` folder together.
3. Edit `BeamNG-VRS/BeamNG-VRS.ini` if desired, then run `Launch-BeamNG-VRS.cmd`.
4. Enable VR in BeamNG as usual. Restart the game after changing settings.

The package includes both DLLs, script-based INI validation, installation/uninstall instructions, an MIT license, a visual settings guide, and checksums. No administrator rights or global layer registration are required. Start Steam first if your game installation needs it.

Defaults: `Size1x1=0.25`, `Size2x2=0.60`, `Shape=ellipse`, `DrawOutline=false`, scissor disabled (top/bottom `0.30`), `GracePeriodMs=30`, and `GazeRefreshMs=8`.

Built on Windows with Visual Studio 2022. The portable tests, Windows configuration tests, DLL loading/export checks, and launcher integration test passed before packaging. The exact source commit is recorded in `BeamNG-VRS/BUILD-INFO.txt`.

This version still needs testing in BeamNG with the target GPU/headset. It is unsigned and experimental; no universal compatibility or FPS gain is claimed. Launch BeamNG normally to stop using the layers.
