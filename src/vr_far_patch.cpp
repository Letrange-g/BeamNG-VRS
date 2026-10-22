#include "vr_far_scan.hpp"
#include <windows.h>
#include <cstdio>
#include <mutex>

// M8.50/M8.51: VR draw distance on the running game (Windows side).
//
// BeamNG 0.40 builds each OpenXR eye frustum every frame with a hard-coded far
// distance of 10000 m. vr_far_scan.hpp finds the places in the game's code that
// load that value; this file points them at a value we own:
//
//  1. patchRunningGame (once, right after the game creates its OpenXR instance)
//     maps the executable's sections, runs the portable scan and logs what it
//     found (exe stamp, site count, one line per site).
//  2. Each site is redirected to the configured distance:
//     - an instruction that reads a shared 10000 constant through a RIP-relative
//       disp32 gets a new disp32 pointing at our own 16-byte "slot" (float at
//       +0, double at +8), allocated within +-1 GB of the image so a disp32 can
//       reach it. The game's shared constant is never modified, so unrelated
//       code that also uses 10000 keeps working;
//     - a 10000 immediate operand is overwritten in place.
//  3. setDistance (M8.51, from the in-game slider or radial menu) only rewrites
//     the slot and the immediates recorded in step 2, so a live change never
//     scans or redirects anything new.
//
// Offsets are RVAs: the Image's bytes are the loaded module itself, limited to
// the committed, readable pages of its sections.
namespace beamng_far {
namespace {
// M8.51: what the startup patch changed, so setDistance can rewrite it live.
// The startup patch runs inside xrCreateApiLayerInstance and setDistance inside
// xrWaitFrame, possibly on different threads; the mutex keeps them apart.
std::mutex patchMutex;
uint8_t* patchedSlot = nullptr; // float at 0, double at 8; read-only between writes.
std::vector<std::pair<uint8_t*, Kind>> patchedImmediates;
// True for committed pages we may read (no guard or no-access pages).
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
// Free regions are searched upward from the image end first, then downward from
// its start, at the allocation granularity (64 KB) VirtualAlloc requires.
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
// Writes into the game's code: unprotect, copy, restore the old protection, and
// flush the instruction cache so the CPU sees the new bytes.
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
 // The process's main module is the game executable; check it is a PE image.
 auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
 const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
 if(!base || dos->e_magic != IMAGE_DOS_SIGNATURE) { log("M8.50 VR far scan FAILED: no executable image"); return; }
 const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
 if(nt->Signature != IMAGE_NT_SIGNATURE) { log("M8.50 VR far scan FAILED: no PE header"); return; }
 LARGE_INTEGER frequency{}, start{}, stop{};
 QueryPerformanceFrequency(&frequency);
 QueryPerformanceCounter(&start);
 // Describe the loaded image to the portable scanner: executable sections are
 // searched for code, readable non-discardable sections for strings/constants.
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
 // Function bounds come from the x64 unwind table (.pdata), which every
 // non-leaf function in an MSVC binary has.
 const FunctionAt functionAt = [base](size_t offset) -> Range {
  DWORD64 imageBase = 0;
  const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(DWORD64(base + offset), &imageBase, nullptr);
  if(!f || imageBase != DWORD64(base)) return {};
  return {f->BeginAddress, f->EndAddress};
 };
 const Report report = scan(image, functionAt);
 // The stamp identifies the game build: compare site counts after an update.
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
 // Scan only (VRDrawDistance unset) or nothing found: leave the game untouched.
 if(distance <= 0 || report.sites.empty()) return;
 const float farValue = float(distance);
 uint8_t* slot = nullptr;
 bool slotTried = false;
 size_t patched = 0;
 for(const Site& s : report.sites) {
  uint8_t* at = base + s.offset;
  // Immediate operand: overwrite the 4/8 value bytes in the instruction itself.
  if(s.kind == Kind::FloatImmediate || s.kind == Kind::DoubleImmediate) {
   const bool ok = s.kind == Kind::FloatImmediate ? write(at, &farValue, sizeof(farValue)) : write(at, &distance, sizeof(distance));
   if(ok) { std::lock_guard<std::mutex> lock(patchMutex); patchedImmediates.push_back({at, s.kind}); }
   patched += ok;
   continue;
  }
  // Constant reference: allocate our slot once, on the first site that needs it.
  if(!slot && !slotTried) {
   slotTried = true;
   slot = allocateNear(uintptr_t(base), uintptr_t(base) + image.size);
   if(!slot) log("M8.50 VR draw distance WARNING: no memory within reach of the game for its constants");
   else {
    std::memcpy(slot, &farValue, sizeof(farValue));
    std::memcpy(slot + 8, &distance, sizeof(distance));
    DWORD old = 0;
    VirtualProtect(slot, 16, PAGE_READONLY, &old);
    std::lock_guard<std::mutex> lock(patchMutex);
    patchedSlot = slot;
   }
  }
  if(!slot) continue;
  // New disp32 = target - end of the disp32. The scanner matched this site with
  // the same formula (disp32 last in its instruction, so RIP = its end); a
  // disp32 followed by an immediate computes a wrong target and never matches.
  const uint8_t* target = s.kind == Kind::FloatConstant ? slot : slot + 8;
  const int64_t delta = int64_t(target - (at + 4));
  if(delta < INT32_MIN || delta > INT32_MAX) continue;
  const int32_t disp = int32_t(delta);
  patched += write(at, &disp, sizeof(disp));
 }
 sprintf_s(line, "M8.50 VR draw distance %.0f m: patched %zu of %zu sites", distance, patched, report.sites.size());
 log(line);
}

// M8.51: live change. Returns false when the startup patch changed nothing
// (VRDrawDistance=off, or no site found in this game build).
bool setDistance(double distance) {
 std::lock_guard<std::mutex> lock(patchMutex);
 const float farValue = float(distance);
 bool changed = false;
 if(patchedSlot) {
  DWORD old = 0;
  if(VirtualProtect(patchedSlot, 16, PAGE_READWRITE, &old)) {
   // Aligned stores: a game thread reading the constant sees the old or the new value.
   *reinterpret_cast<volatile float*>(patchedSlot) = farValue;
   *reinterpret_cast<volatile double*>(patchedSlot + 8) = distance;
   DWORD ignored = 0;
   VirtualProtect(patchedSlot, 16, old, &ignored);
   changed = true;
  }
 }
 for(const auto& [at, kind] : patchedImmediates)
  changed |= kind == Kind::FloatImmediate ? write(at, &farValue, sizeof(farValue)) : write(at, &distance, sizeof(distance));
 return changed;
}
} // namespace beamng_far
