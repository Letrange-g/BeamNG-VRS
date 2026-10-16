# Requirements and troubleshooting

The portable package targets Windows x64, BeamNG's Vulkan VR mode, an OpenXR
runtime, and a GPU/driver supporting attachment fragment shading rate.
Earlier in-game tests primarily used a Pimax Dream Air and NVIDIA RTX 5090.
Other hardware needs testing. Experimental builds can crash or show artifacts.

## What the package loads

- `BeamNG_VRS.dll`: Vulkan layer applying per-eye shading-rate patterns and optional scissor crops.
- `BeamNG_EyeProbe.dll`: OpenXR layer collecting supported eye-tracking and eye-image information.

The launcher validates the INI and activates both layers only for the game it
starts. It preserves normal Vulkan layer discovery, needs no administrator
rights, and does not replace original game files or register layers globally.
Windows PowerShell 5.1 is included by default in Windows 10/11; PowerShell 7
is not required. The ZIP contains no EXEs. Source builds and test executables
are described in [BUILDING.md](BUILDING.md).

## The game does not start

First confirm BeamNG works in Vulkan mode through its normal launcher. Start
Steam first if your installation needs it, and use your normal Windows account:
elevated applications ignore Vulkan layer search paths.

Run `Diagnose-BeamNG-VRS.cmd`, then close the game and any error dialog. The
window prints the Vulkan loader log path. Use the normal launcher for performance
comparisons; diagnostics enable extra logging.

Extract into the game installation folder, not its user-data folder. For an
unusual layout, run `BeamNG-VRS/Launch-BeamNG-VRS.ps1` with
`-GameExecutable "C:\path\to\Bin64\BeamNG.drive.x64.exe"`.

The INI must sit beside the launch CMD. Since beta.8 the public launcher ignores
an older copy inside `BeamNG-VRS/`. Back up and move your saved settings to the
root when updating. Delete `BeamNG-VRS/BeamNG-VRS-Check.exe` if left over from
beta.4 or earlier; extracting a newer ZIP does not remove old files.

If an old beta logs `Failed to open dynamic library "./BeamNG_VRS.dll" with
error 87, update the complete package. Beta.4 corrected the Windows relative
paths in both layer manifests.

## Rendering limits

Foveation reduces eligible fragment shading; it does not lower render-target
resolution. Scissor reduces pixel coverage in matching legacy eye render passes;
it does not cull scene objects, avoid vertex processing, or cap render distance.
Dynamic rendering, layered/multiview targets and secondary-only subpasses are
outside the supported crop path. VRS substitution also targets eligible single-sample
passes; enabling MSAA can change which passes receive VRS and its benefit.
With `AllPasses=true` (default) eye-sized passes that are not directly copied to
the headset use a combined pattern (finer rate of both eyes per tile). VRS does
not apply to compute-shader passes.
Unknown eye targets retain the horizontal
area either eye needs, rather than guessing which eye is being rendered.

The debug outline is injected into rendering and can appear in temporal history.
Disable it for gameplay and benchmarks. OpenXR visibility masks were unavailable
in tested sessions; the experimental stencil path stays disabled by default.
CPU gaze polling and source-to-upload timing do not measure eye-to-display latency.
Turbo uses the OpenXR layer, not Vulkan pipeline flags. It overlaps a runtime
wait with app work, can interfere with reprojection and cannot reduce shader or
physics cost. A stalled runtime wait can also stall draining during teardown.
Use the normal launcher and switch `[Performance] TurboMode=false/true` for a
controlled test. `[Logging] Level=off` disables layer logs; diagnostics overrides it.

See [configuration details](CONFIGURATION.md) and [performance findings](PERFORMANCE.md)
for the exact controls and a comparison procedure. Automated tests use mock API
callbacks and do not validate headset appearance or predict FPS.

## Reporting a problem

Include your package version, GPU, headset, settings and whether normal Vulkan
works. Review logs before sharing: they can contain local paths and device
identifiers. Follow [SECURITY.md](SECURITY.md). This is an unofficial project,
with no BeamNG or headset-vendor affiliation or endorsement.
