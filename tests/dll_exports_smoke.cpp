#include <windows.h>
#include <cassert>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
 assert(argc == 3);
 const std::filesystem::path vulkanPath(argv[1]);
 const std::filesystem::path xrPath(argv[2]);
 assert(vulkanPath.is_absolute() && xrPath.is_absolute());
 const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
 // Reproduce the error from the old manifest's unresolved relative path.
 HMODULE relative = LoadLibraryExW(L"./BeamNG_VRS.dll", nullptr, flags);
 const DWORD relativeError = GetLastError();
 assert(!relative && relativeError == ERROR_INVALID_PARAMETER);
 HMODULE vulkan = LoadLibraryExW(vulkanPath.c_str(), nullptr, flags);
 HMODULE xr = LoadLibraryExW(xrPath.c_str(), nullptr, flags);
 assert(vulkan && xr);
 assert(GetProcAddress(vulkan, "vkNegotiateLoaderLayerInterfaceVersion"));
 assert(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
 assert(GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
 assert(GetProcAddress(xr, "xrNegotiateLoaderApiLayerInterface"));
 assert(FreeLibrary(xr));
 assert(FreeLibrary(vulkan));
 std::cout << "Old relative path reproduced error 87; manifest-resolved DLLs loaded and exported all loader entry points\n";
 return 0;
}
