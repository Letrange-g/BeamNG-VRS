#include "vr_far_scan.hpp"
#include <windows.h>
#include <cstdio>

// M8.50: applies vr_far_scan.hpp to BeamNG's executable in this process. Offsets
// are RVAs: the Image's bytes are the loaded module itself, limited to the
// committed, readable pages of its sections.
namespace beamng_far {
namespace {
bool readable(const MEMORY_BASIC_INFORMATION& m) {
 if(m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
 return (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
// Splits [begin,end) into its committed readable regions.
void addReadable(const uint8_t* base, size_t begin, size_t end, std::vector<Range>& out) {
 size_t at = begin;
 while(at < end) {
  MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery(base + at, &m, sizeof(m))) break;
  const size_t regionEnd = std::min(end, size_t(static_cast<const uint8_t*>(m.BaseAddress) + m.RegionSize - base));
  if(regionEnd <= at) break;
  if(readable(m)) {
   if(!out.empty() && out.back().end == at) out.back().end = regionEnd;
   else out.push_back({at, regionEnd});
  }
  at = regionEnd;
 }
}
// A page for our float and double within +-1 GB of the image, so a disp32 reaches it.
uint8_t* allocateNear(uintptr_t imageBegin, uintptr_t imageEnd) {
 SYSTEM_INFO info{};
 GetSystemInfo(&info);
 const uintptr_t granularity = info.dwAllocationGranularity, page = info.dwPageSize, reach = uintptr_t(1) << 30;
 const uintptr_t low = imageBegin > reach ? imageBegin - reach : granularity;
 const uintptr_t high = imageEnd + reach;
 for(uintptr_t at = imageEnd; at < high;) {
  MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery(reinterpret_cast<void*>(at), &m, sizeof(m))) break;
  const uintptr_t regionBegin = uintptr_t(m.BaseAddress), regionEnd = regionBegin + m.RegionSize;
  if(m.State == MEM_FREE) {
   const uintptr_t candidate = (std::max(regionBegin, at) + granularity - 1) / granularity * granularity;
   if(candidate + page <= regionEnd && candidate + page <= high)
    if(void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
     return static_cast<uint8_t*>(p);
  }
  if(regionEnd <= at) break;
  at = regionEnd;
 }
 for(uintptr_t at = imageBegin; at > low;) {
  MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery(reinterpret_cast<void*>(at - 1), &m, sizeof(m))) break;
  const uintptr_t regionBegin = uintptr_t(m.BaseAddress), regionEnd = std::min(uintptr_t(regionBegin + m.RegionSize), at);
  if(m.State == MEM_FREE && regionEnd >= page) {
   const uintptr_t candidate = (regionEnd - page) / granularity * granularity;
   if(candidate >= regionBegin && candidate >= low)
    if(void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
     return static_cast<uint8_t*>(p);
  }
  if(regionBegin >= at) break;
  at = regionBegin;
 }
 return nullptr;
}
bool write(uint8_t* at, const void* bytes, size_t size) {
 DWORD old = 0;
 if(!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
 std::memcpy(at, bytes, size);
 DWORD ignored = 0;
 VirtualProtect(at, size, old, &ignored);
 FlushInstructionCache(GetCurrentProcess(), at, size);
 return true;
}
} // namespace

void patchRunningGame(void (*log)(const char*), double distance) {
 char line[256]{};
 auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
 const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
 if(!base || dos->e_magic != IMAGE_DOS_SIGNATURE) { log("M8.50 VR far scan FAILED: no executable image"); return; }
 const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
 if(nt->Signature != IMAGE_NT_SIGNATURE) { log("M8.50 VR far scan FAILED: no PE header"); return; }
 LARGE_INTEGER frequency{}, start{}, stop{};
 QueryPerformanceFrequency(&frequency);
 QueryPerformanceCounter(&start);
 Image image;
 image.bytes = base;
 image.size = nt->OptionalHeader.SizeOfImage;
 const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
 for(WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
  const DWORD flags = section->Characteristics;
  const size_t begin = section->VirtualAddress;
  const size_t end = std::min(size_t(image.size), begin + (section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData));
  if(flags & IMAGE_SCN_MEM_EXECUTE) addReadable(base, begin, end, image.code);
  else if((flags & IMAGE_SCN_MEM_READ) && !(flags & IMAGE_SCN_MEM_DISCARDABLE)) addReadable(base, begin, end, image.data);
 }
 const FunctionAt functionAt = [base](size_t offset) -> Range {
  DWORD64 imageBase = 0;
  const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(DWORD64(base + offset), &imageBase, nullptr);
  if(!f || imageBase != DWORD64(base)) return {};
  return {f->BeginAddress, f->EndAddress};
 };
 const Report report = scan(image, functionAt);
 QueryPerformanceCounter(&stop);
 sprintf_s(line, "M8.50 VR far scan: exe stamp=%08lx anchors=%zu refs=%zu functions=%zu constants float=%zu double=%zu sites=%zu (%.0f ms)",
           static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), report.anchors, report.anchorRefs, report.functions,
           report.floatConstants, report.doubleConstants, report.sites.size(),
           double(stop.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart));
 log(line);
 for(size_t i = 0; i < report.sites.size() && i < 60; ++i) {
  const Site& s = report.sites[i];
  sprintf_s(line, "M8.50 site %s rva=0x%zx function=0x%zx depth=%d", name(s.kind), s.offset, s.function, s.depth);
  log(line);
 }
 if(distance <= 0 || report.sites.empty()) return;
 const float farValue = float(distance);
 uint8_t* slot = nullptr;
 bool slotTried = false;
 size_t patched = 0;
 for(const Site& s : report.sites) {
  uint8_t* at = base + s.offset;
  if(s.kind == Kind::FloatImmediate) { patched += write(at, &farValue, sizeof(farValue)); continue; }
  if(s.kind == Kind::DoubleImmediate) { patched += write(at, &distance, sizeof(distance)); continue; }
  if(!slot && !slotTried) {
   slotTried = true;
   slot = allocateNear(uintptr_t(base), uintptr_t(base) + image.size);
   if(!slot) log("M8.50 VR draw distance WARNING: no memory within reach of the game for its constants");
   else {
    std::memcpy(slot, &farValue, sizeof(farValue));
    std::memcpy(slot + 8, &distance, sizeof(distance));
    DWORD old = 0;
    VirtualProtect(slot, 16, PAGE_READONLY, &old);
   }
  }
  if(!slot) continue;
  const uint8_t* target = s.kind == Kind::FloatConstant ? slot : slot + 8;
  const int64_t delta = int64_t(target - (at + 4));
  if(delta < INT32_MIN || delta > INT32_MAX) continue;
  const int32_t disp = int32_t(delta);
  patched += write(at, &disp, sizeof(disp));
 }
 sprintf_s(line, "M8.50 VR draw distance %.0f m: patched %zu of %zu sites", distance, patched, report.sites.size());
 log(line);
}
} // namespace beamng_far
