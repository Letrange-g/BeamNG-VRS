#pragma once

#include <algorithm>
#include <cmath>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

// Portable parser, shared by the two layers. No process environment is modified.
namespace beamng_config {
using Values = std::map<std::string, std::string>;

inline std::string trim(std::string text) {
 const auto first = text.find_first_not_of(" \t\r\n");
 if(first == std::string::npos) return {};
 return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
inline std::string lower(std::string text) {
 for(char& c : text) if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
 return text;
}
inline void number(const std::string& value, double low, double high, bool integer = false) {
 std::istringstream input(value);
 input.imbue(std::locale::classic());
 double parsed = 0;
 if(!(input >> parsed) || input.peek() != std::char_traits<char>::eof() ||
    !std::isfinite(parsed) || parsed < low || parsed > high ||
    (integer && value.find_first_not_of("0123456789") != std::string::npos))
  throw std::runtime_error("value is outside the allowed range");
}
inline std::string boolean(std::string value) {
 value = lower(value);
 if(value == "1" || value == "true") return "1";
 if(value == "0" || value == "false") return "0";
 throw std::runtime_error("expected 0, 1, false or true");
}
inline std::string choice(std::string value, std::initializer_list<std::string_view> choices) {
 value = lower(value);
 if(std::find(choices.begin(), choices.end(), value) == choices.end())
  throw std::runtime_error("unsupported setting value");
 return value;
}

// Launcher-only: the PowerShell launcher disables these implicit OpenXR layers
// for the game process. The DLLs only validate the value.
inline void openxrLayerList(const std::string& value) {
 if(value.empty() || value == "*") return;
 std::istringstream names(value);
 std::string name;
 while(std::getline(names, name, ',')) {
  name = trim(name);
  if(name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos)
   throw std::runtime_error("expected OpenXR layer names separated by commas, or *");
 }
 if(value.back() == ',') throw std::runtime_error("expected OpenXR layer names separated by commas, or *");
}

// These are rendering/tracking gates, not DLL loading switches. Scissor and
// turbo stay independent; the OpenXR layer still identifies eye images.
inline void activation(Values& out, bool enabled) {
 const std::string value = enabled ? "1" : "0";
  // These gates together select the existing per-eye, compatible-pipeline path.
  for(const char* name : {"BEAMNG_VRS_ENABLE_FEATURES", "BEAMNG_VRS_ATTACHMENT_PIPELINES",
       "BEAMNG_VRS_UPLOAD_TEST", "BEAMNG_VRS_M8_EXPERIMENTAL", "BEAMNG_VRS_M89_STATIC_TEST",
       "BEAMNG_VRS_M812_LIVE_GAZE", "BEAMNG_VRS_M84_CPU_GAZE", "BEAMNG_VRS_M815_ASYNC", "BEAMNG_EYE_SAMPLE",
       "BEAMNG_EYE_FORCE_EXTENSION"}) out[name] = value;
}
inline Values defaults() {
 Values out;
 activation(out, true);
 out["BEAMNG_VRS_TURBO_MODE"] = "0";
 out["BEAMNG_VRS_ALL_PASSES"] = "1";
 out["BEAMNG_VRS_SHAPE"] = "circle";
 out["BEAMNG_VRS_FOVEATION_MODE"] = "eye";
 out["BEAMNG_VRS_CENTER_RADIUS"] = "0.25";
 out["BEAMNG_VRS_MID_RADIUS"] = "0.55";
 out["BEAMNG_VRS_MID_RATE"] = "2x2";
 out["BEAMNG_VRS_OUTER_RATE"] = "4x4";
 out["BEAMNG_VRS_GAZE_UPDATE_MS"] = "8";
 out["BEAMNG_VRS_GAZE_GRACE_MS"] = "30";
 out["BEAMNG_VRS_M823_LAZY_FB"] = "1";
 out["BEAMNG_VRS_DEBUG_OUTLINE"] = "0";
 out["BEAMNG_VRS_OUTLINE_RESIZE_PAUSE_MS"] = "350";
 out["BEAMNG_VRS_SCISSOR_ENABLE"] = "1";
 out["BEAMNG_VRS_SCISSOR_TOP"] = "0.30";
 out["BEAMNG_VRS_SCISSOR_BOTTOM"] = "0.30";
 out["BEAMNG_VRS_SCISSOR_LEFT"] = "0";
 out["BEAMNG_VRS_SCISSOR_RIGHT"] = "0";
 for(const auto* edge : {"LEFT_EYE_LEFT", "LEFT_EYE_RIGHT", "RIGHT_EYE_LEFT", "RIGHT_EYE_RIGHT"})
  out[std::string("BEAMNG_VRS_SCISSOR_") + edge] = "0";
 out["BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT"] = "0.10";
 out["BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT"] = "0.10";
 out["BEAMNG_VRS_M818_STENCIL"] = "0";
 out["BEAMNG_VRS_LENS_MASK"] = "1";
 out["BEAMNG_VRS_LENS_MASK_SIZE"] = "1.15";
 out["BEAMNG_VRS_LOG_LEVEL"] = "1";
 out["BEAMNG_VRS_RENDERER"] = "d3d12";
 out["BEAMNG_VRS_VR_DRAW_DISTANCE"] = "4000";
 return out;
}
inline void setting(Values& out, const std::string& section, const std::string& key, std::string value) {
 const std::string id = section + "." + key;
 if(id == "foveation.enabled") activation(out,boolean(value)=="1");
 else if(id == "logging.level") {
  const auto level=choice(value,{"off","errors","info","debug"});
  out["BEAMNG_VRS_LOG_LEVEL"]=level=="off"?"-1":level=="errors"?"0":level=="debug"?"2":"1";
 }
 else if(id == "performance.turbomode") out["BEAMNG_VRS_TURBO_MODE"]=boolean(value);
 else if(id == "foveation.allpasses") out["BEAMNG_VRS_ALL_PASSES"]=boolean(value);
 else if(id == "lensmask.enabled") out["BEAMNG_VRS_LENS_MASK"]=boolean(value);
 else if(id == "lensmask.size") { number(value, 0.5, 1.5); out["BEAMNG_VRS_LENS_MASK_SIZE"] = value; }
 else if(id == "foveation.shape") out["BEAMNG_VRS_SHAPE"] = choice(value, {"circle", "square", "ellipse"});
 else if(id == "foveation.size1x1" || id == "foveation.size2x2") {
  number(value, 0, 1);
  out[id == "foveation.size1x1" ? "BEAMNG_VRS_CENTER_RADIUS" : "BEAMNG_VRS_MID_RADIUS"] = value;
 } else if(id == "tracking.gazerefreshms" || id == "tracking.graceperiodms") {
  number(value, id == "tracking.gazerefreshms" ? 2 : 0, id == "tracking.gazerefreshms" ? 50 : 1000, true);
  out[id == "tracking.gazerefreshms" ? "BEAMNG_VRS_GAZE_UPDATE_MS" : "BEAMNG_VRS_GAZE_GRACE_MS"] = value;
 } else if(id == "debug.drawoutline" || id == "scissor.enabled")
  out[id == "debug.drawoutline" ? "BEAMNG_VRS_DEBUG_OUTLINE" : "BEAMNG_VRS_SCISSOR_ENABLE"] = boolean(value);
 else if(id == "scissor.top" || id == "scissor.bottom" || id == "scissor.left" || id == "scissor.right" ||
         id == "scissor.lefteyeleft" || id == "scissor.lefteyeright" ||
         id == "scissor.righteyeleft" || id == "scissor.righteyeright") {
  number(value, 0, 0.49);
  const Values names{{"top", "TOP"}, {"bottom", "BOTTOM"}, {"left", "LEFT"}, {"right", "RIGHT"},
    {"lefteyeleft", "LEFT_EYE_LEFT"}, {"lefteyeright", "LEFT_EYE_RIGHT"},
    {"righteyeleft", "RIGHT_EYE_LEFT"}, {"righteyeright", "RIGHT_EYE_RIGHT"}};
  out["BEAMNG_VRS_SCISSOR_" + names.at(key)] = value;
 } else if(id == "openxr.disablelayers") openxrLayerList(value);
 // Launcher-only: selects BeamNG's -gfx argument (default d3d12, BeamNG 0.40's VR
 // default). d3d12 features come from the OpenXR layer, vulkan ones from the Vulkan layer.
 else if(id == "renderer.api") out["BEAMNG_VRS_RENDERER"] = choice(value, {"vulkan", "d3d12"});
 // Launcher-only (M8.48): BeamNG's VR upscale render scale, applied by a small Lua mod.
 else if(id == "renderer.renderscale") { number(value, 0.25, 1.0); out["BEAMNG_VRS_RENDER_SCALE"] = value; }
 // M8.50: VR far distance in metres, patched into the game's OpenXR view code.
 // "off" leaves the game's code untouched (nothing is scanned or patched).
 else if(id == "renderer.vrdrawdistance") {
  if(lower(value) == "off") out.erase("BEAMNG_VRS_VR_DRAW_DISTANCE");
  else { number(value, 100, 10000); out["BEAMNG_VRS_VR_DRAW_DISTANCE"] = value; }
 }
 // Removed (M8.49 had no effect in VR): accepted and ignored so older INIs still launch.
 else if(id == "renderer.farclip") {}
 else throw std::runtime_error("unknown section or setting");
}

inline Values parse(std::istream& input) {
 Values out = defaults(), seen;
 std::string line, section;
 size_t lineNumber = 0;
 while(std::getline(input, line)) {
  ++lineNumber;
  if(lineNumber == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
  line = trim(line);
  if(line.empty() || line[0] == ';' || line[0] == '#') continue;
  try {
   if(line[0] == '[') {
    if(line.back() != ']') throw std::runtime_error("invalid section header");
    section = lower(trim(line.substr(1, line.size() - 2)));
    if(section != "foveation" && section != "debug" && section != "scissor" && section != "tracking" && section != "performance" && section != "logging" && section != "lensmask" && section != "openxr" && section != "renderer")
     throw std::runtime_error("unknown section");
    continue;
   }
   const auto equals = line.find('=');
   if(equals == std::string::npos) throw std::runtime_error("expected key=value");
   const auto key = lower(trim(line.substr(0, equals)));
   const auto value = trim(line.substr(equals + 1));
   if(!seen.emplace(section + "." + key, value).second) throw std::runtime_error("duplicate setting");
   setting(out, section, key, value);
  } catch(const std::runtime_error& error) {
   throw std::runtime_error("INI line " + std::to_string(lineNumber) + ": " + error.what());
  }
 }
 // Explicit per-eye edges win independently of file order. Old shared edges
 // remain accepted for upgrades, but are omitted from the public template.
 for(const auto& edge : {std::pair{"lefteyeleft", "LEFT_EYE_LEFT"},
                         std::pair{"lefteyeright", "LEFT_EYE_RIGHT"},
                         std::pair{"righteyeleft", "RIGHT_EYE_LEFT"},
                         std::pair{"righteyeright", "RIGHT_EYE_RIGHT"}}) {
  if(!seen.count(std::string("scissor.") + edge.first)) {
   const bool left = std::string_view(edge.first).ends_with("left");
   // Explicit legacy edges retain their old meaning; absent legacy settings
   // leave the public per-eye defaults in place.
   if(seen.count(left ? "scissor.left" : "scissor.right"))
    out[std::string("BEAMNG_VRS_SCISSOR_") + edge.second] =
      out.at(left ? "BEAMNG_VRS_SCISSOR_LEFT" : "BEAMNG_VRS_SCISSOR_RIGHT");
  }
 }
 if(input.bad()) throw std::runtime_error("could not read INI file");
 if(seen.empty()) throw std::runtime_error("INI contains no settings");
 const auto center = out.find("BEAMNG_VRS_CENTER_RADIUS"), middle = out.find("BEAMNG_VRS_MID_RADIUS");
 if(center != out.end() && middle != out.end()) {
  std::istringstream c(center->second), m(middle->second);
  c.imbue(std::locale::classic()); m.imbue(std::locale::classic());
  double cv = 0, mv = 0; c >> cv; m >> mv;
  if(cv > mv) throw std::runtime_error("Size1x1 must not exceed Size2x2");
 }
 return out;
}
} // namespace beamng_config
