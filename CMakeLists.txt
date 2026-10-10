cmake_minimum_required(VERSION 3.25)
project(BeamNG_VRS LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The Vulkan/OpenXR API layer DLLs currently require Windows.")
endif()

find_package(Vulkan REQUIRED)

add_library(BeamNG_VRS SHARED src/layer.cpp)
target_compile_features(BeamNG_VRS PRIVATE cxx_std_20)
target_include_directories(BeamNG_VRS PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(BeamNG_VRS PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(BeamNG_VRS PROPERTIES OUTPUT_NAME "BeamNG_VRS")

find_path(OPENXR_INCLUDE_DIR openxr/openxr_loader_negotiation.h
  HINTS "$ENV{OPENXR_SDK}/include" "$ENV{OPENXR_SDK_PATH}/include"
  DOC "OpenXR SDK include directory (containing openxr/openxr.h)")
if(OPENXR_INCLUDE_DIR)
  add_library(BeamNG_EyeProbe SHARED src/openxr_probe.cpp src/openxr_probe.def)
  target_compile_features(BeamNG_EyeProbe PRIVATE cxx_std_20)
  target_include_directories(BeamNG_EyeProbe PRIVATE "${OPENXR_INCLUDE_DIR}" ${Vulkan_INCLUDE_DIRS})
  target_compile_definitions(BeamNG_EyeProbe PRIVATE XR_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(BeamNG_EyeProbe PROPERTIES OUTPUT_NAME "BeamNG_EyeProbe")
else()
  message(WARNING "OpenXR headers not found: BeamNG_EyeProbe target will not build. Set OPENXR_INCLUDE_DIR.")
endif()

# Both manifests resolve their DLL by a relative path; copy next to build output.
add_custom_command(TARGET BeamNG_VRS POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/manifests/VK_LAYER_BEAMNG_vrs.json"
    "$<TARGET_FILE_DIR:BeamNG_VRS>/VK_LAYER_BEAMNG_vrs.json")
if(TARGET BeamNG_EyeProbe)
  add_custom_command(TARGET BeamNG_EyeProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/manifests/XR_APILAYER_BEAMNG_eye_probe.json"
      "$<TARGET_FILE_DIR:BeamNG_EyeProbe>/XR_APILAYER_BEAMNG_eye_probe.json")
endif()
