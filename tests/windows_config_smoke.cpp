#include "windows_config.hpp"
#include <cassert>
#include <fstream>

int main(int argc, char** argv) {
 assert(argc == 3);
 const std::string mode(argv[1]);
 // GetTempPathW preserves Unicode usernames. Each CTest case gets a new process.
 wchar_t temporary[32768]{};
 assert(GetTempPathW(32768, temporary));
 const auto directory = std::filesystem::path(temporary) / (L"BeamNG-VRS-config-\u00e9-" + std::to_wstring(GetCurrentProcessId()));
 std::filesystem::create_directory(directory);
 const auto path = directory / L"BeamNG-VRS.ini";
 if(mode == "valid") std::filesystem::copy_file(std::filesystem::path(argv[2]) / "packaging/BeamNG-VRS.ini", path);
 else if(mode == "invalid") { std::ofstream file(path); file << "[Debug]\nDrawOutline=typo\n"; }
 assert(SetEnvironmentVariableW(L"BEAMNG_VRS_CONFIG", path.c_str()));
 assert(SetEnvironmentVariableA("BEAMNG_VRS_SHAPE", nullptr));
 assert(SetEnvironmentVariableA("BEAMNG_VRS_ENABLE_FEATURES", nullptr));
 char buffer[32]{};
 if(mode == "valid") {
  const auto layers = directory / L"BeamNG-VRS";
  std::filesystem::create_directory(layers);
  const auto legacyIni = layers / L"BeamNG-VRS.ini";
  { std::ofstream file(legacyIni); file << "[Debug]\nDrawOutline=true\n"; }
  assert(beamng_config::moduleIniPath(layers) == path); // Root wins over an old copy.
  std::filesystem::remove(path);
  assert(beamng_config::moduleIniPath(layers) == legacyIni);
  std::filesystem::copy_file(std::filesystem::path(argv[2]) / "packaging/BeamNG-VRS.ini", path);
  assert(beamng_config::read("BEAMNG_VRS_SHAPE", buffer, sizeof(buffer)) == 7);
  assert(std::strcmp(buffer, "ellipse") == 0);
  char small[2]{'x','x'};
  assert(beamng_config::read("BEAMNG_VRS_SHAPE", small, sizeof(small)) == 8);
  assert(small[0] == '\0');
  assert(beamng_config::read("BEAMNG_VRS_SHAPE", nullptr, 0) == 8);
 } else if(mode == "missing") assert(beamng_config::read("BEAMNG_VRS_SHAPE", buffer, sizeof(buffer)) == 0);
 else {
  assert(beamng_config::read("BEAMNG_VRS_ENABLE_FEATURES", buffer, sizeof(buffer)) == 1);
  assert(std::strcmp(buffer, "0") == 0);
 }
 assert(SetEnvironmentVariableA("BEAMNG_VRS_SHAPE", "square"));
 assert(beamng_config::read("BEAMNG_VRS_SHAPE", buffer, sizeof(buffer)) == 6);
 assert(std::strcmp(buffer, "square") == 0);
 assert(SetEnvironmentVariableA("BEAMNG_CONFIG_TEST_UNRELATED", "unchanged"));
 assert(beamng_config::read("BEAMNG_CONFIG_TEST_UNRELATED", buffer, sizeof(buffer)) == 9);
 assert(std::strcmp(buffer, "unchanged") == 0);
 std::filesystem::remove_all(directory);
}
