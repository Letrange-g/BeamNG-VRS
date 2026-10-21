#pragma once
#include "ffr_pattern.hpp"
#include "scissor_crop.hpp"
#include <cstddef>
#include <cstdint>

// M8.41/M8.42: Direct3D 12 side of BeamNG-VRS. BeamNG 0.40 hands its D3D12
// device and queue to the OpenXR layer in xrCreateSession. From there this
// module hooks render-target binding to:
//  - M8.41 (diagnostics runs only): report VRS support and the render-target
//    sizes bound per frame;
//  - M8.42: apply a tile-based shading-rate image (VRS Tier 2) to eye-shaped
//    render targets, at the game's internal render size and at full size.
//  - M8.43: the [Scissor], [LensMask] and [Debug] options. Eye-shaped targets
//    get their scissor rectangles clipped to the crop; the lens corners get
//    the coarsest rate; and before each OpenXR eye image is released, its
//    cropped edges and lens corners are cleared to black and the debug
//    outline is drawn on it.
// Both eyes share the same intermediate targets, so each tile uses the finer
// rate of the two eyes (the M8.32 combined pattern), and the render-time crop
// keeps what either eye needs. Implemented in d3d12_probe.cpp with the C COM
// interface.
namespace beamng_d3d12 {
using LogFn = void (*)(const char*);
struct Foveation {
 bool enabled = false;
 bool gaze = false;          // Follow eye tracking; otherwise fixed center.
 float center = .25f, middle = .55f;
 uint8_t middleRate = beamng_ffr::rate(2, 2), outerRate = beamng_ffr::rate(4, 4);
 beamng_ffr::Shape shape = beamng_ffr::Shape::Circle;
 uint32_t graceMs = 30;
};
struct Effects {
 bool crop = false;
 beamng_scissor::Margins margins[2]{}; // Left eye, right eye.
 bool lensMask = false;
 double lensSize = 1.15;
 bool outline = false;                  // Drawn only while foveation is active.
 // M8.44: the OpenXR layer already removed the margins from the field of view,
 // so eye images are the kept part only; crop is then off and margins place the
 // lens ellipse, which stays relative to the full eye image.
 bool fovCrop = false;
 bool any() const { return crop || lensMask || outline; }
};
// Installs the hooks once per process and selects this device for VRS and
// the effects. With nothing requested it changes nothing. Returns false
// (rendering untouched) when the device or a hook cannot be used.
bool attach(void* device, void* queue, LogFn log, bool diagnostics, const Foveation& foveation,
            const Effects& effects = {});
// Called just before xrReleaseSwapchainImage for an eye image (ID3D12Resource*,
// in RENDER_TARGET state; eye 0 = left). The game's rendering to it has been
// submitted on the OpenXR queue, so work submitted here runs after it.
void finishEyeImage(uint64_t texture, int eye, uint32_t dxgiFormat);
// Known OpenXR eye swapchain textures (ID3D12Resource*), marked in reports.
void setEyeImages(const uint64_t* images, size_t count);
// OpenXR eye swapchain size; render targets with the same aspect ratio at
// 25-100% of this size receive the shading-rate image.
void setEyeSize(uint32_t width, uint32_t height);
// Projected gaze per eye in [0,1] image coordinates; bit i of mask = eye i valid.
void setGaze(const float u[2], const float v[2], uint32_t mask);
// Called once per xrEndFrame: updates shading-rate images, logs diagnostics.
void frame();
}
