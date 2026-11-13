#pragma once
#include "triple_view.hpp"
#include <cstdint>

// M8.54 triple view, Direct3D 12 side (see triple_view.hpp). Runs on the game's
// OpenXR queue, after the game's own work for the frame:
//  - captureEye copies each sharp eye image just before the game releases it
//    (afterwards the runtime owns it);
//  - compose draws each full-field-of-view output image from the middle view
//    and the copied sharp window, into this layer's own swapchain images.
namespace beamng_triple_d3d12 {
using LogFn = void (*)(const char*);
// Compiles the shader and creates the device objects. False: triple view stays off.
bool init(void* device, void* queue, LogFn log);
bool ready();
// texture: the game's eye image (ID3D12Resource*, RENDER_TARGET state), eye 0 = left.
void captureEye(uint64_t texture, int eye);
struct EyeInput {
 beamng_triple::Constants constants{}; // out, win, middle (half extents, feather), rotation rows
 int32_t rectX = 0, rectY = 0, rectWidth = 0, rectHeight = 0; // sharp window in the game's image
};
// outputs: this layer's swapchain images (ID3D12Resource*, RENDER_TARGET state).
// middle: compose the wide views (false: black around the sharp window).
// shared: triple view, both eyes use wide view 0 (M8.54); otherwise one per eye (M8.55).
bool compose(const EyeInput eyes[2], const uint64_t outputs[2], bool middle, bool shared);
}
