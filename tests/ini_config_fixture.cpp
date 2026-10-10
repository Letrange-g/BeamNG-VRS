#include "ini_config.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int check(const std::filesystem::path& path) {
 try {
  std::ifstream input(path, std::ios::binary);
  if(!input) throw std::runtime_error("Cannot open BeamNG-VRS.ini");
  const auto values = beamng_config::parse(input);
  (void)values;
  std::cout << "BeamNG-VRS.ini is valid.\n";
  return 0;
 } catch(const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
 }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
 if(argc != 2) { std::cerr << "Usage: ini_config_fixture <INI path>\n"; return 2; }
 return check(std::filesystem::path(argv[1]));
}
