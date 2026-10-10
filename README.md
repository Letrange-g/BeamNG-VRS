# BeamNG-VRS

Foveated rendering and optional edge cropping for **BeamNG.drive in Vulkan VR**.
Keep more detail where you look and reduce rendering work around the edges.
Works with a fixed center or supported eye tracking.

**Experimental Windows beta.** Performance varies by headset, GPU and scene.

## Install

1. [Download the Windows ZIP](https://github.com/Letrange-g/BeamNG-VRS/releases) from **Assets** (not the source-code ZIP).
2. Close BeamNG and extract the whole ZIP into the game folder, beside `Bin64`.
3. Double-click `Launch-BeamNG-VRS.cmd`, then enable VR in the game.

Edit **`BeamNG-VRS.ini` beside the launch CMD** in Notepad to change settings.
Restart the game after every change. Extraction inside `Bin64` also works.
Windows PowerShell is included in Windows 10/11; the package adds no EXE.

## Settings

- **Foveation Enabled:** set `false` to use scissor or turbo without VRS. Default `true`.
- **Size1x1:** size of the sharp, full-quality center. Default `0.25`.
- **Size2x2:** size of the medium-quality region around it. Default `0.60`.
- **Shape:** `circle` (default), `ellipse`, or `square`.
- **DrawOutline:** shows the quality-region borders for checking settings. Leave `false` for gameplay.
- **Scissor:** crops image edges to save rendering work. Enable only after checking the edges in your headset.

![VRS quality regions and independent crop controls for each eye](docs/images/vrs-settings.png)

Scissor top/bottom apply to both eyes. Horizontal edges are independent:

| INI setting | Edge | Default |
| --- | --- | --- |
| `Top` / `Bottom` | Top / bottom of both eyes | `0.30` each |
| `LeftEyeLeft` | Left eye, outside | `0.20` |
| `LeftEyeRight` | Left eye, near the nose | `0` |
| `RightEyeLeft` | Right eye, near the nose | `0` |
| `RightEyeRight` | Right eye, outside | `0.20` |

`0.05` crops 5% from that edge. Start small; keep the nose-side values at `0`
if you can see those borders. Scissor is **disabled by default**.
[All settings and examples →](docs/CONFIGURATION.md)

**Optional turbo (beta.10):** `[Performance] TurboMode=true` can overlap CPU work
with the headset runtime wait. Default `false`; check smoothness as well as FPS.
It may interfere with motion reprojection. `[Logging] Level=off` disables layer
logs; other choices are `errors`, `info` (default), and `debug`.

## See it in action

![BeamNG VR with VRS quality outlines and black scissor borders](docs/images/vrs-scissor-showcase.png)

**VRS + scissor in game:** red outlines the full-quality region; orange marks
the outer 2x2 boundary. Black borders show the scissor crop. Turn debug outlines
off for gameplay.

[VRS off](https://youtu.be/dfVtQ83cNbM) ·
[VRS on](https://youtu.be/psoPj8rjUV8) ·
[VRS + scissor](https://youtu.be/jXkz3hwZiLg)

The comparison videos have debug outlines enabled. Turn them off when comparing performance.

## Update or remove

Back up your INI, extract the new ZIP, then restore your settings in the INI beside
the launch CMD. Update the launcher and both DLLs together. Older `Left`/`Right`
settings still work; the four new controls let you adjust each eye separately.

Launch BeamNG normally to stop using VRS. To uninstall, remove the `BeamNG-VRS`
folder, `BeamNG-VRS.ini`, both CMD files and `READ-ME-FIRST.txt`.

[Help and requirements](docs/TECHNICAL.md) · [Performance notes](docs/PERFORMANCE.md) ·
[Source guide](docs/SOURCE-GUIDE.md) · [Build from source](docs/BUILDING.md) · [Report an issue safely](docs/SECURITY.md)

Unofficial project. [MIT license](LICENSE).
