#include "vr_far_scan.hpp"
#include <cassert>
#include <cstdio>

// M8.50: the VR far scan finds 10000 near code that references the OpenXR view
// anchors (directly or through calls), and nowhere else.
namespace {
std::vector<uint8_t> bytes(0x10000, 0xCC);
void put(size_t at, std::initializer_list<uint8_t> b) { for(uint8_t x : b) bytes[at++] = x; }
void put32(size_t at, int32_t v) { std::memcpy(&bytes[at], &v, 4); }
void putFloat(size_t at, float v) { std::memcpy(&bytes[at], &v, 4); }
// RIP-relative operand whose disp32 sits at `at` and points at `target`.
void rip(size_t at, size_t target) { put32(at, int32_t(int64_t(target) - int64_t(at + 4))); }
}

int main() {
 const char anchor[] = "engine::OpenXrHelper::onRenderUpdate";
 std::memcpy(&bytes[0x8100], anchor, sizeof(anchor));
 bytes[0x80ff] = 0;
 putFloat(0x9000, 10000.f);          // __real@461c4000
 const double d = 10000.0;
 std::memcpy(&bytes[0x9008], &d, 8); // __real@40c3880000000000

 // A: lea rcx, [anchor] ; call B
 put(0x1010, {0x48, 0x8D, 0x0D}); rip(0x1013, 0x8100);  // The whole log prefix string, which contains "OpenXrHelper".
 put(0x1020, {0xE8}); put32(0x1021, int32_t(0x2000 - (0x1021 + 4)));
 // B: movss xmm0, [10000.f] ; mov dword [rsp+28h], 10000.f ; call C2
 put(0x2000, {0xF3, 0x0F, 0x10, 0x05}); rip(0x2004, 0x9000);
 put(0x2040, {0xC7, 0x44, 0x24, 0x28}); putFloat(0x2044, 10000.f);
 put(0x2050, {0xE8}); put32(0x2051, int32_t(0x4000 - (0x2051 + 4)));
 // C2 (depth 2): movsd xmm1, [10000.0]
 put(0x4000, {0xF2, 0x0F, 0x10, 0x0D}); rip(0x4004, 0x9008);
 // D: unrelated function using 10000.f: never reached from the anchors.
 put(0x3000, {0xF3, 0x0F, 0x10, 0x05}); rip(0x3004, 0x9000);

 const std::vector<beamng_far::Range> functions{{0x1000, 0x1100}, {0x2000, 0x2100}, {0x3000, 0x3100}, {0x4000, 0x4100}};
 const beamng_far::FunctionAt functionAt = [&](size_t offset) -> beamng_far::Range {
  for(const auto& f : functions) if(offset >= f.begin && offset < f.end) return f;
  return {};
 };
 beamng_far::Image image;
 image.bytes = bytes.data(); image.size = bytes.size();
 image.code = {{0x1000, 0x8000}};
 image.data = {{0x8000, 0x10000}};

 const auto report = beamng_far::scan(image, functionAt);
 assert(report.anchors == 1 && report.anchorRefs == 1);
 assert(report.functions == 3);
 assert(report.floatConstants == 1 && report.doubleConstants == 1);
 assert(report.sites.size() == 3);
 assert(report.sites[0].offset == 0x2004 && report.sites[0].kind == beamng_far::Kind::FloatConstant && report.sites[0].depth == 1);
 assert(report.sites[1].offset == 0x2044 && report.sites[1].kind == beamng_far::Kind::FloatImmediate && report.sites[1].function == 0x2000);
 assert(report.sites[2].offset == 0x4004 && report.sites[2].kind == beamng_far::Kind::DoubleConstant && report.sites[2].depth == 2);
 for(const auto& s : report.sites) assert(s.function != 0x3000);

 // Without anchors nothing is reported, so a game that changed beyond recognition is left alone.
 bytes[0x8100] = 'X'; bytes[0x8108] = 'X';
 assert(beamng_far::scan(image, functionAt).sites.empty());
 std::puts("M8.50 VR far scan: anchors, call depth and 10000 sites passed");
}
