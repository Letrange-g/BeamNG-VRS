#pragma once
#include <windows.h>
#include <cstdint>
// Same layout in both DLLs. Diagnostic-only, process-local mapping.
namespace beamng_gaze {
inline constexpr const char* mappingName = "Local\\BeamNG_VRS_Gaze_M73b_v2";
inline constexpr uint32_t magicValue = 0x47373342;
struct alignas(8) Data {
 volatile LONG sequence;
 uint32_t magic;
 uint32_t processId;
 uint32_t reserved;
 uint64_t tickMs;
 int64_t predictedTime;
 float qx,qy,qz,qw;
 float px,py,pz;
 uint32_t flags;
 float eyeU[2],eyeV[2];
 uint32_t eyeValidMask;
 uint32_t padding;
};
static_assert(sizeof(Data)==88, "Unexpected gaze shared-memory layout");
}

// M7.5a: separate process-local mapping for OpenXR projection-eye VkImages.
// Never modify the legacy gaze Data ABI.
namespace beamng_eye_images {
inline constexpr const char* mappingName = "Local\\BeamNG_VRS_EyeImages_M75a1";
inline constexpr uint32_t magicValue = 0x75354145;
struct alignas(8) Data {
 volatile LONG sequence;
 uint32_t magic;
 uint32_t processId;
 uint32_t reserved;
 uint64_t image[2][3];
 uint64_t updatedTickMs;
};
static_assert(sizeof(Data)==72);
}

// M8.14: independent resolution ABI, published by the OpenXR probe only
// after a matching pair of eye-sized swapchains is observed.
namespace beamng_eye_resolution {
inline constexpr const char* mappingName = "Local\\BeamNG_VRS_EyeResolution_M814";
inline constexpr uint32_t magicValue = 0x38313452;
struct alignas(8) Data {
 volatile LONG sequence;
 uint32_t magic;
 uint32_t processId;
 uint32_t width;
 uint32_t height;
 uint32_t reserved;
 uint64_t changedTickMs;
};
static_assert(sizeof(Data)==32, "Unexpected resolution shared-memory layout");
}

// M8.18: runtime-provided per-eye visibility LINE_LOOP and projection tangents.
// Coordinates are at view-space z=-1, NOT normalized [0,1] texture UV.
// Updated only by OpenXR; read with the seqlock from Vulkan.
namespace beamng_visibility_mesh {
inline constexpr const char* mappingName = "Local\\BeamNG_VRS_VisibilityMesh_M818";
inline constexpr uint32_t magicValue = 0x38313856;
inline constexpr uint32_t maxVertices = 512;
struct Point { float x,y; };
struct Eye { uint32_t count=0; float tanLeft=0,tanRight=0,tanDown=0,tanUp=0;
             Point vertices[maxVertices]{}; };
struct alignas(8) Data {
 volatile LONG sequence=0;
 uint32_t magic=0, processId=0, validMask=0;
 Eye eyes[2]{};
};
}
