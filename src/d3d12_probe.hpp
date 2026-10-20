#pragma once
#include <cstddef>
#include <cstdint>

// M8.41: diagnostics-only Direct3D 12 probe. BeamNG 0.40 hands its D3D12
// device to the OpenXR layer in xrCreateSession; from there this probe reports
// VRS support and which render-target sizes the game binds per frame, so the
// later D3D12 foveation can target the real (pre-upscale) eye passes.
// It never changes rendering. Implemented in d3d12_probe.cpp (C COM interface).
namespace beamng_d3d12 {
using LogFn = void (*)(const char*);
// Logs VRS tier and installs render-target hooks once per process. Returns
// false (and changes nothing) when the device or a hook cannot be used.
bool attach(void* device, LogFn log);
// Known OpenXR eye swapchain textures (ID3D12Resource*), marked in reports.
void setEyeImages(const uint64_t* images, size_t count);
// Called once per xrEndFrame; logs a render-target histogram periodically.
void frame();
}
