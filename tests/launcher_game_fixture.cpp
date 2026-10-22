#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
 const char* output = std::getenv("BEAMNG_LAUNCHER_TEST_OUTPUT");
 if(!output) return 1;
 std::ofstream file(output);
 if(!file) return 1;
 file << "arguments=";
 for(int i = 1; i < argc; ++i) file << (i == 1 ? "" : " ") << argv[i];
 file << '\n';
 file << "working_directory=" << std::filesystem::current_path().string() << '\n';
 for(const char* name : {"BEAMNG_VRS_CONFIG", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "XR_API_LAYER_PATH", "VK_INSTANCE_LAYERS", "XR_ENABLE_API_LAYERS", "BEAMNG_VRS_CENTER_RADIUS", "VK_LOADER_DEBUG", "BEAMNG_VRS_LOG_LEVEL",
   "XR_LOADER_DEBUG", "DISABLE_XR_APILAYER_TEST_overlay", "DISABLE_XR_APILAYER_TEST_off",
   "BEAMNG_VRS_VR_DRAW_DISTANCE", "BEAMNG_VRS_VR_DRAW_DISTANCE_FILE"}) {
  const char* value = std::getenv(name);
  file << name << '=' << (value ? value : "") << '\n';
 }
 std::cerr << "Vulkan loader diagnostic fixture\n";
 return 0;
}
