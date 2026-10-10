#pragma once
#include "ini_config.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace beamng_config {
inline std::filesystem::path moduleIniPath(const std::filesystem::path& layerDirectory) {
 const auto rootIni = layerDirectory.parent_path() / L"BeamNG-VRS.ini";
 if(std::filesystem::exists(rootIni)) return rootIni;
 return layerDirectory / L"BeamNG-VRS.ini"; // Older direct-loading layouts.
}
inline std::filesystem::path configPath() {
 std::wstring path(32768, L'\0');
 const DWORD length = GetEnvironmentVariableW(L"BEAMNG_VRS_CONFIG", path.data(), static_cast<DWORD>(path.size()));
 if(length) {
  if(length >= path.size()) throw std::runtime_error("BEAMNG_VRS_CONFIG is too long");
  path.resize(length);
  const std::filesystem::path overridePath(path);
  if(!overridePath.is_absolute()) throw std::runtime_error("BEAMNG_VRS_CONFIG must be an absolute path");
  return overridePath;
 }
 HMODULE module = nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
     reinterpret_cast<LPCWSTR>(&configPath), &module)) throw std::runtime_error("could not locate layer module");
 const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
 if(!size || size >= path.size()) throw std::runtime_error("could not locate layer directory");
 path.resize(size);
 return moduleIniPath(std::filesystem::path(path).parent_path());
}
inline const Values& fileValues() {
 // Lazy initialization: no file I/O in DllMain or a static initializer.
 static const Values values = [] {
  try {
   const auto path = configPath();
   if(!std::filesystem::exists(path)) return Values{}; // Preserve legacy environment-only use.
   std::ifstream input(path, std::ios::binary);
   if(!input) throw std::runtime_error("could not open INI file");
   return parse(input);
  } catch(const std::exception& error) {
   const std::string message = std::string("BeamNG-VRS configuration rejected: ") + error.what() + "\n";
   OutputDebugStringA(message.c_str());
   // An invalid file never partly enables the experimental rendering path.
   Values disabled;
   activation(disabled, false);
   disabled["BEAMNG_VRS_TURBO_MODE"]="0";
   return disabled;
  }
 }();
 return values;
}
inline DWORD read(const char* name, char* buffer, DWORD capacity) {
 // Environment overrides preserve existing developer launch scripts. TEMP and
 // every other non-project variable keep their original Win32 semantics.
 const DWORD length = GetEnvironmentVariableA(name, buffer, capacity);
 if(length || (std::strncmp(name, "BEAMNG_VRS_", 11) && std::strncmp(name, "BEAMNG_EYE_", 11))) return length;
 const auto& values = fileValues();
 const auto it = values.find(name);
 if(it == values.end()) return 0;
 const auto& value = it->second;
 if(value.size() >= capacity) {
  if(buffer && capacity) buffer[0] = '\0';
  return static_cast<DWORD>(value.size() + 1);
 }
 if(buffer) std::memcpy(buffer, value.c_str(), value.size() + 1);
 return static_cast<DWORD>(value.size());
}
} // namespace beamng_config
