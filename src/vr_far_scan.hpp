#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

// M8.50: VR draw distance. BeamNG 0.40 builds each OpenXR eye view's frustum
// every frame with a fixed 10000 m far distance and ignores the level's visible
// distance, the Lua frustum and every setting. This scanner looks for that
// constant in the game's own code, near the code that names the OpenXR views:
//  1. anchor strings ("oxrView", "OpenXrHelper", ...) in the image;
//  2. functions with a RIP-relative reference to one of them;
//  3. those functions and the ones they call (two levels);
//  4. in them, 10000 as a float/double immediate, or a RIP-relative reference to
//     a 10000 constant in the image's data.
// Each hit is a place that can be patched to the configured distance. The
// scanner is portable (offsets into a copy of the image) so it is unit-tested
// on Linux; vr_far_patch.cpp applies it to the running game on Windows.
namespace beamng_far {
struct Range { size_t begin = 0, end = 0; };
struct Image {
 const uint8_t* bytes = nullptr;
 size_t size = 0;
 std::vector<Range> code;  // Executable sections.
 std::vector<Range> data;  // Readable, non-executable sections.
};
// Returns the function that contains an offset ({0,0} if none), from .pdata on Windows.
using FunctionAt = std::function<Range(size_t)>;

enum class Kind : uint8_t { FloatImmediate, DoubleImmediate, FloatConstant, DoubleConstant };
struct Site {
 size_t offset = 0;   // Immediate bytes, or the disp32 of a RIP-relative operand.
 size_t function = 0; // Start of the containing function.
 Kind kind = Kind::FloatImmediate;
 int depth = 0;       // 0 = references an anchor string, 1-2 = called from there.
};
struct Report {
 size_t anchors = 0, anchorRefs = 0, functions = 0;
 size_t floatConstants = 0, doubleConstants = 0;
 std::vector<Site> sites;
};

constexpr float farFloat = 10000.f;
constexpr double farDouble = 10000.0;
inline const std::vector<std::string>& anchorStrings() {
 static const std::vector<std::string> anchors{"oxrView", "OpenXrHelper", "recentering event honored"};
 return anchors;
}

inline bool inside(const std::vector<Range>& ranges, size_t offset) {
 for(const auto& r : ranges) if(offset >= r.begin && offset < r.end) return true;
 return false;
}
inline int32_t disp32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }

// Offsets of every occurrence of a byte pattern in the ranges, at the given alignment.
inline std::vector<size_t> find(const Image& image, const std::vector<Range>& ranges,
                                const void* pattern, size_t length, size_t alignment = 1) {
 std::vector<size_t> hits;
 for(const auto& r : ranges) {
  for(size_t at = r.begin; at + length <= r.end && at + length <= image.size; ++at) {
   if(at % alignment) continue;
   if(std::memcmp(image.bytes + at, pattern, length) == 0) hits.push_back(at);
  }
 }
 return hits;
}

// Start of the C string containing offset (walks back over printable bytes).
inline size_t stringStart(const Image& image, size_t offset) {
 while(offset > 0) {
  const uint8_t c = image.bytes[offset - 1];
  if(c < 0x20 || c > 0x7e) break;
  --offset;
 }
 return offset;
}

// Positions in [begin,end) whose 4 bytes, read as a RIP-relative disp32 ending
// the instruction, point at one of the sorted targets.
inline std::vector<size_t> ripRefs(const Image& image, Range range, const std::vector<size_t>& sortedTargets) {
 std::vector<size_t> refs;
 if(sortedTargets.empty()) return refs;
 for(size_t at = range.begin; at + 4 <= range.end && at + 4 <= image.size; ++at) {
  const int64_t target = int64_t(at) + 4 + disp32(image.bytes + at);
  if(target < 0) continue;
  if(std::binary_search(sortedTargets.begin(), sortedTargets.end(), size_t(target))) refs.push_back(at);
 }
 return refs;
}

// Direct call targets (E8 rel32) in a function that start another function.
inline std::vector<Range> callees(const Image& image, Range function, const FunctionAt& functionAt) {
 std::vector<Range> out;
 for(size_t at = function.begin; at + 5 <= function.end && at + 5 <= image.size; ++at) {
  if(image.bytes[at] != 0xE8) continue;
  const int64_t target = int64_t(at) + 5 + disp32(image.bytes + at + 1);
  if(target < 0 || !inside(image.code, size_t(target))) continue;
  const Range f = functionAt(size_t(target));
  if(f.end > f.begin && f.begin == size_t(target)) out.push_back(f);
 }
 return out;
}

inline Report scan(const Image& image, const FunctionAt& functionAt, size_t maxFunctions = 4000) {
 Report report;
 // 1. Anchor strings in data (whole C strings, so a log prefix also counts).
 std::vector<size_t> strings;
 for(const auto& text : anchorStrings()) {
  for(size_t hit : find(image, image.data, text.data(), text.size())) {
   strings.push_back(stringStart(image, hit));
   ++report.anchors;
  }
 }
 std::sort(strings.begin(), strings.end());
 strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
 // 2. Functions that reference them.
 std::vector<std::pair<Range, int>> functions;
 std::set<size_t> seen;
 for(const auto& code : image.code) {
  for(size_t ref : ripRefs(image, code, strings)) {
   ++report.anchorRefs;
   const Range f = functionAt(ref);
   if(f.end > f.begin && seen.insert(f.begin).second) functions.push_back({f, 0});
  }
 }
 // 3. Two levels of direct callees.
 for(size_t i = 0; i < functions.size() && functions.size() < maxFunctions; ++i) {
  if(functions[i].second >= 2) continue;
  for(const Range& f : callees(image, functions[i].first, functionAt)) {
   if(functions.size() >= maxFunctions) break;
   if(seen.insert(f.begin).second) functions.push_back({f, functions[i].second + 1});
  }
 }
 report.functions = functions.size();
 // 4. 10000 in those functions: immediates, or references to 10000 constants in data.
 uint8_t fBytes[4], dBytes[8];
 std::memcpy(fBytes, &farFloat, 4); std::memcpy(dBytes, &farDouble, 8);
 auto floats = find(image, image.data, fBytes, 4, 4);
 auto doubles = find(image, image.data, dBytes, 8, 8);
 report.floatConstants = floats.size(); report.doubleConstants = doubles.size();
 for(const auto& [f, depth] : functions) {
  const std::vector<Range> one{f};
  for(size_t at : find(image, one, fBytes, 4)) report.sites.push_back({at, f.begin, Kind::FloatImmediate, depth});
  for(size_t at : find(image, one, dBytes, 8)) report.sites.push_back({at, f.begin, Kind::DoubleImmediate, depth});
  for(size_t at : ripRefs(image, f, floats)) report.sites.push_back({at, f.begin, Kind::FloatConstant, depth});
  for(size_t at : ripRefs(image, f, doubles)) report.sites.push_back({at, f.begin, Kind::DoubleConstant, depth});
 }
 std::sort(report.sites.begin(), report.sites.end(), [](const Site& a, const Site& b) { return a.offset < b.offset; });
 return report;
}

#ifdef _WIN32
// Scans BeamNG's executable in memory, logs M8.50 lines and, with distance > 0,
// patches every site to that distance. Implemented in vr_far_patch.cpp.
void patchRunningGame(void (*log)(const char*), double distance);
#endif

inline const char* name(Kind kind) {
 switch(kind) {
 case Kind::FloatImmediate: return "float-imm";
 case Kind::DoubleImmediate: return "double-imm";
 case Kind::FloatConstant: return "float-const";
 default: return "double-const";
 }
}
} // namespace beamng_far
