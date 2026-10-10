# Building (Windows, experimental)

Prerequisites:

1. Visual Studio 2022 or newer with the C++ desktop workload and CMake integration.
2. The [Vulkan SDK](https://vulkan.lunarg.com/) with headers for Vulkan fragment shading rate and layer negotiation.
3. An [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) with `openxr_loader_negotiation.h`. Set `OPENXR_INCLUDE_DIR` if CMake cannot find it.

From a **Visual Studio Developer PowerShell**:

```powershell
cmake -S . -B build -A x64 -DOPENXR_INCLUDE_DIR="C:/path/to/OpenXR-SDK/include"
cmake --build build --config Release
```

The expected outputs are `BeamNG_VRS.dll`, `BeamNG_EyeProbe.dll` (when OpenXR headers are provided), and the corresponding `.json` manifests, **copied beside each built DLL**. On multi-config generators the output is usually `build/Release/`.

## Build a portable beta ZIP

With Visual Studio 2022 C++ tools, Git, CMake 3.25+, and Python 3 installed on Windows, run:

```powershell
./scripts/Build-Release.ps1 -Version 0.1.0-beta.9
```

This script fetches pinned Vulkan and OpenXR header revisions from Khronos, builds both DLLs, runs the portable C++/Python tests plus Windows configuration and launcher integration tests, then packages `out/releases/BeamNG-VRS-0.1.0-beta.9-windows-x64.zip` and a SHA-256 file. It does not require the full graphical SDKs for this header-only build path. The Release DLLs use the static MSVC runtime. The script refuses to overwrite an existing ZIP or staging directory; use a new version or output directory for another build.

The package contains no EXE files. INI validation runs in the existing PowerShell launcher, with a test-only C++ fixture checking that it agrees with the DLL parser on valid and invalid settings. The packaging script rejects EXE files in the staged release.

The GitHub Actions workflow **Build Windows beta ZIP** performs these steps on Windows 2022 and uploads the ZIP as a workflow artifact. It runs on main-branch pushes, pull requests, or manual dispatch. Ordinary builds do not publish a release. For an intentional beta publication, dispatch it manually with the package version and **Publish the tested ZIP** enabled; a separate job verifies the archive checksum and publishes a prerelease using the workflow's GitHub token only after all build/tests pass. Repository visibility stays unchanged.

For artifact downloads, extract the artifact's outer ZIP, then extract the inner release ZIP into the game root. GitHub Release assets provide the release ZIP directly. Private repositories require a GitHub account with repository access for either download.

The ZIP contains:

```text
Launch-BeamNG-VRS.cmd
Diagnose-BeamNG-VRS.cmd
READ-ME-FIRST.txt
SHA256SUMS.txt
BeamNG-VRS/
  BeamNG_VRS.dll
  BeamNG_EyeProbe.dll
  VK_LAYER_BEAMNG_vrs.json
  XR_APILAYER_BEAMNG_eye_probe.json
  BeamNG-VRS.ini
  Launch-BeamNG-VRS.ps1
  LICENSE
  BUILD-INFO.txt
  VRS-SETTINGS.png
```

Before a public beta, run the packaged launcher on Windows with BeamNG and the target GPU/headset: check fixed-center fallback, eye tracking, both outlines, scissor top/bottom/left/right, resolution changes, and a normal game launch without layers. Review the ZIP contents, scan the archive, and verify checksums. Automated tests cannot establish VR compatibility or performance gains. Builds are unsigned; signing requires a separate signing identity. The MIT license is included in the package.

Linux cloud tasks can run `bash scripts/run-portable-tests.sh`. Native DLL building remains Windows-only.

## Layer loading

The Vulkan manifest defines `VK_LAYER_BEAMNG_vrs` and the OpenXR manifest defines `XR_APILAYER_BEAMNG_eye_probe`. Neither should be installed globally during early tests. Use a **process-local** PowerShell environment and set `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`, `XR_API_LAYER_PATH`, and `XR_ENABLE_API_LAYERS` to match the compiled output directory and layer names. Launch BeamNG in Vulkan mode from that same process environment. The exact executable path depends on the user's installation.

**Do not commit or paste your actual game installation path, Windows username, SDK paths, environment dump, or headset logs.**

If startup fails, close that shell or remove the four loader variables. Always keep a known-good DLL backup before testing changes.

## Test status

Portable C++ pattern/geometry tests are under `tests/`. Source contract checks are Python scripts in the same folder. These do not compile the Windows Vulkan/OpenXR DLLs or establish headset compatibility.

The source tree includes historical milestone identifiers because this is a research prototype. Simplification, error handling, and GPU timing A/B benchmarks remain to be done before a broader release.
