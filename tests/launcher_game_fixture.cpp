#include <cstdlib>
#include <fstream>
#include <filesystem>
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
 for(const char* name : {"BEAMNG_VRS_CONFIG", "VK_LAYER_PATH", "XR_API_LAYER_PATH", "VK_INSTANCE_LAYERS", "XR_ENABLE_API_LAYERS", "BEAMNG_VRS_CENTER_RADIUS"}) {
  const char* value = std::getenv(name);
  file << name << '=' << (value ? value : "") << '\n';
 }
 return 0;
}
