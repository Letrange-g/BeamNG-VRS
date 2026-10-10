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

## Layer loading

The Vulkan manifest defines `VK_LAYER_BEAMNG_vrs` and the OpenXR manifest defines `XR_APILAYER_BEAMNG_eye_probe`. Neither should be installed globally during early tests. Use a **process-local** PowerShell environment and set `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`, `XR_API_LAYER_PATH`, and `XR_ENABLE_API_LAYERS` to match the compiled output directory and layer names. Launch BeamNG in Vulkan mode from that same process environment. The exact executable path depends on the user's installation.

**Do not commit or paste your actual game installation path, Windows username, SDK paths, environment dump, or headset logs.**

If startup fails, close that shell or remove the four loader variables. Always keep a known-good DLL backup before testing changes.

## Test status

Portable C++ pattern/geometry tests are under `tests/`. Source contract checks are Python scripts in the same folder. These do not compile the Windows Vulkan/OpenXR DLLs or establish headset compatibility.

The source tree includes historical milestone identifiers because this is a research prototype. Simplification, error handling, and GPU timing A/B benchmarks remain to be done before a broader release.
