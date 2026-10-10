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

- **Size1x1:** size of the sharp, full-quality center. Default `0.25`.
- **Size2x2:** size of the medium-quality region around it. Default `0.60`.
- **Shape:** `ellipse` (default), `circle`, or `square`.
- **DrawOutline:** shows the quality-region borders for checking settings. Leave `false` for gameplay.
- **Scissor:** crops image edges to save rendering work. Enable only after checking the edges in your headset.

![VRS quality regions and independent crop controls for each eye](docs/images/vrs-settings.png)

Scissor top/bottom apply to both eyes. **Beta.9 adds independent horizontal edges:**

| INI setting | Edge | Default |
| --- | --- | --- |
| `Top` / `Bottom` | Top / bottom of both eyes | `0.30` each |
| `LeftEyeLeft` | Left eye, outside | `0` |
| `LeftEyeRight` | Left eye, near the nose | `0` |
| `RightEyeLeft` | Right eye, near the nose | `0` |
| `RightEyeRight` | Right eye, outside | `0` |

`0.05` crops 5% from that edge. Start small; keep the nose-side values at `0`
if you can see those borders. Scissor is **disabled by default**.
[All settings and examples →](docs/CONFIGURATION.md)

## See it in action

[Demo](https://gofile.io/d/0497e367-c9e8-4831-8618-0a52bc37e277) ·
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
[Build from source](docs/BUILDING.md) · [Report an issue safely](docs/SECURITY.md)

Unofficial project. [MIT license](LICENSE).
