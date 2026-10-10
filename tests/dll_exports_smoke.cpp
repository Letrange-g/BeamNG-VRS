#include <windows.h>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
 assert(argc == 2);
 const std::filesystem::path root(argv[1]);
 const auto vulkanPath = root / L"BeamNG_VRS.dll";
 const auto xrPath = root / L"BeamNG_EyeProbe.dll";
 const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
 HMODULE vulkan = LoadLibraryExW(vulkanPath.c_str(), nullptr, flags);
 HMODULE xr = LoadLibraryExW(xrPath.c_str(), nullptr, flags);
 assert(vulkan && xr);
 assert(GetProcAddress(vulkan, "vkNegotiateLoaderLayerInterfaceVersion"));
 assert(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
 assert(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
 assert(GetProcAddress(xr, "xrNegotiateLoaderApiLayerInterface"));
 assert(FreeLibrary(xr));
 assert(FreeLibrary(vulkan));
 std::cout << "Both DLLs loaded and all loader entry points were found\n";
}
