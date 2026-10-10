cmake_minimum_required(VERSION 3.25)
project(BeamNG_VRS VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The Vulkan/OpenXR API layer DLLs currently require Windows.")
endif()

option(BEAMNG_FETCH_HEADERS "Download pinned Khronos headers when SDKs are unavailable" OFF)
if(BEAMNG_FETCH_HEADERS)
  include(FetchContent)
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG 952f776f6573aafbb62ea717d871cd1d6816c387
    SOURCE_SUBDIR beamng-headers-only)
  FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG 7f9285bce1ce8b69bb75554bf788666579d0c35e
    SOURCE_SUBDIR beamng-headers-only)
  # Only populate header trees; do not build or distribute the SDK loaders.
  FetchContent_MakeAvailable(vulkan_headers openxr_sdk)
endif()
find_path(Vulkan_INCLUDE_DIR vulkan/vk_layer.h
  HINTS "${vulkan_headers_SOURCE_DIR}/include" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include"
  REQUIRED)

add_library(BeamNG_VRS SHARED src/layer.cpp src/layer.def)
target_compile_features(BeamNG_VRS PRIVATE cxx_std_20)
target_include_directories(BeamNG_VRS PRIVATE ${Vulkan_INCLUDE_DIR})
target_compile_definitions(BeamNG_VRS PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(BeamNG_VRS PROPERTIES OUTPUT_NAME "BeamNG_VRS" PREFIX "")

find_path(OPENXR_INCLUDE_DIR openxr/openxr_loader_negotiation.h
  HINTS "${openxr_sdk_SOURCE_DIR}/include" "$ENV{OPENXR_SDK}/include" "$ENV{OPENXR_SDK_PATH}/include"
  DOC "OpenXR SDK include directory (containing openxr/openxr.h)")
if(OPENXR_INCLUDE_DIR)
  add_library(BeamNG_EyeProbe SHARED src/openxr_probe.cpp src/openxr_probe.def)
  target_compile_features(BeamNG_EyeProbe PRIVATE cxx_std_20)
  target_include_directories(BeamNG_EyeProbe PRIVATE "${OPENXR_INCLUDE_DIR}" ${Vulkan_INCLUDE_DIR})
  target_compile_definitions(BeamNG_EyeProbe PRIVATE XR_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(BeamNG_EyeProbe PROPERTIES OUTPUT_NAME "BeamNG_EyeProbe" PREFIX "")
else()
  message(WARNING "OpenXR headers not found: BeamNG_EyeProbe target will not build. Set OPENXR_INCLUDE_DIR.")
endif()

add_executable(BeamNG-VRS-Check src/config_check.cpp)
target_compile_features(BeamNG-VRS-Check PRIVATE cxx_std_20)
target_compile_definitions(BeamNG-VRS-Check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
  target_link_options(BeamNG-VRS-Check PRIVATE -municode)
endif()
if(MSVC)
  # Make the ZIP self-contained: users do not need a separate VC runtime install.
  set_property(TARGET BeamNG_VRS BeamNG-VRS-Check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(TARGET BeamNG_EyeProbe)
    set_property(TARGET BeamNG_EyeProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  file(GLOB contract_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_*.py")
  foreach(test IN LISTS contract_tests)
    get_filename_component(name "${test}" NAME_WE)
    add_test(NAME "${name}" COMMAND Python3::Interpreter "${test}")
    set_tests_properties("${name}" PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  file(GLOB cpp_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*test.cpp")
  foreach(test IN LISTS cpp_tests)
    get_filename_component(name "${test}" NAME_WE)
    add_executable("${name}" "${test}")
    target_compile_features("${name}" PRIVATE cxx_std_20)
    target_include_directories("${name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # These existing tests use assert; keep assertions active in Release builds.
    if(MSVC)
      target_compile_options("${name}" PRIVATE /UNDEBUG)
    else()
      target_compile_options("${name}" PRIVATE -UNDEBUG)
    endif()
    add_test(NAME "${name}" COMMAND "${name}")
    set_tests_properties("${name}" PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  add_executable(windows_config_test tests/windows_config_smoke.cpp)
  target_compile_features(windows_config_test PRIVATE cxx_std_20)
  target_compile_definitions(windows_config_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(windows_config_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(MSVC)
    target_compile_options(windows_config_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(windows_config_test PRIVATE -UNDEBUG)
  endif()
  foreach(mode IN ITEMS valid missing invalid)
    add_test(NAME "windows_config_${mode}" COMMAND windows_config_test "${mode}" "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
  if(TARGET BeamNG_EyeProbe)
  add_executable(launcher_game_fixture tests/launcher_game_fixture.cpp)
  target_compile_features(launcher_game_fixture PRIVATE cxx_std_20)
  add_executable(dll_exports_test tests/dll_exports_smoke.cpp)
  target_compile_features(dll_exports_test PRIVATE cxx_std_20)
  target_compile_definitions(dll_exports_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MSVC)
    target_compile_options(dll_exports_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(dll_exports_test PRIVATE -UNDEBUG)
    target_link_options(dll_exports_test PRIVATE -municode)
  endif()
  add_test(NAME dll_exports COMMAND dll_exports_test "$<TARGET_FILE_DIR:BeamNG_VRS>")
  add_test(NAME launcher_integration
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/Test-Launcher.ps1"
    -BuildDirectory "$<TARGET_FILE_DIR:BeamNG_VRS>" -GameFixture "$<TARGET_FILE:launcher_game_fixture>")
  endif()
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
