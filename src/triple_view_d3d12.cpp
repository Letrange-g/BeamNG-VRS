// M8.54 triple view, Direct3D 12 composition; see triple_view_d3d12.hpp.
// The C COM interface, like d3d12_probe.cpp. The shader is compiled at start with
// the system's d3dcompiler_47.dll, and the root signature serialized with d3d12.dll,
// so the DLL links neither library.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include "triple_view_d3d12.hpp"
#include "d3d12_probe.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace beamng_triple_d3d12 {
namespace {

// Fullscreen triangle; each output pixel is a direction in the eye's full field of
// view (tangent space). Inside the sharp window it shows the game's eye image,
// elsewhere the middle view seen from that direction, blended over `feather`.
const char shaderSource[] = R"(
cbuffer Constants : register(b0) {
 float4 outTan;   // left, right, up, down
 float4 winTan;
 float4 rect;     // u0, v0, u1, v1
 float4 middle;   // half x, half y, feather, on
 float4 rot0; float4 rot1; float4 rot2;
 float4 flags;    // round, sharp conversion, middle conversion, unused
};
Texture2D sharpTex : register(t0);
Texture2D middleTex : register(t1);
SamplerState linearClamp : register(s0);

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
 uv = float2((id << 1) & 2, id & 2);
 pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0), 1 / 2.4) - 0.055; }
float3 convert(float3 c, float mode) { return mode > 1.5 ? toSrgb(c) : mode > 0.5 ? toLinear(c) : c; }

float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
 const float tx = lerp(outTan.x, outTan.y, uv.x);
 const float ty = lerp(outTan.z, outTan.w, uv.y);
 float3 color = 0;
 if (middle.w > 0.5) {
  const float3 d = float3(tx, ty, -1);
  const float3 m = float3(dot(rot0.xyz, d), dot(rot1.xyz, d), dot(rot2.xyz, d));
  if (m.z < -1e-4) {
   const float2 t = m.xy / -m.z;
   const float2 muv = float2(0.5 + 0.5 * t.x / middle.x, 0.5 - 0.5 * t.y / middle.y);
   if (all(muv >= 0) && all(muv <= 1)) color = convert(middleTex.SampleLevel(linearClamp, muv, 0).rgb, flags.z);
  }
 }
 const float2 wuv = float2((tx - winTan.x) / (winTan.y - winTan.x), (winTan.z - ty) / (winTan.z - winTan.w));
 const float2 c = wuv * 2 - 1;
 const float r = flags.x > 0.5 ? length(c) : max(abs(c.x), abs(c.y));
 const float f = max(middle.z, 1e-4);
 const float w = 1 - smoothstep(1 - f, 1, r);
 if (w > 0) {
  const float2 suv = lerp(rect.xy, rect.zw, saturate(wuv));
  color = lerp(color, convert(sharpTex.SampleLevel(linearClamp, suv, 0).rgb, flags.y), w);
 }
 return float4(color, 1);
}
)";

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
constexpr UINT ringSize = 8;

struct Slot { ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; uint64_t value = 0; };
struct Sharp { ID3D12Resource* copy = nullptr; D3D12_RESOURCE_DESC desc{}; D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST; bool valid = false; };

LogFn logFn = nullptr;
std::mutex mutex;
std::atomic<bool> isReady{false};
ID3D12Device* device = nullptr;
ID3D12CommandQueue* queue = nullptr;
ID3D12Fence* fence = nullptr; uint64_t fenceValue = 0; HANDLE fenceEvent = nullptr;
Slot slots[ringSize]{}; UINT nextSlot = 0;
ID3D12RootSignature* rootSignature = nullptr;
ID3DBlob* vsCode = nullptr; ID3DBlob* psCode = nullptr;
ID3D12PipelineState* pipeline = nullptr; DXGI_FORMAT pipelineFormat = DXGI_FORMAT_UNKNOWN;
ID3D12DescriptorHeap* srvHeap = nullptr; UINT srvIncrement = 0;
ID3D12DescriptorHeap* rtvHeap = nullptr; UINT rtvIncrement = 0;
std::unordered_map<uint64_t, UINT> rtvSlots;
constexpr UINT rtvCount = 16;
Sharp sharp[2]{};
std::atomic<uint64_t> composed{0}, skipped{0};

void log(const char* msg) { if(logFn) logFn(msg); }

// The format a view of `format` reads raw (typeless resources take their UNORM view).
DXGI_FORMAT viewFormat(DXGI_FORMAT format, bool srgb) {
 switch(format) {
  case DXGI_FORMAT_R8G8B8A8_TYPELESS: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
  case DXGI_FORMAT_B8G8R8X8_TYPELESS: return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default: return format;
 }
}
bool isSrgb(DXGI_FORMAT f) {
 return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
bool isTypeless(DXGI_FORMAT f) { return viewFormat(f, false) != f; }
// 0: none, 1: decode sRGB to linear, 2: encode linear to sRGB. A source and an output
// that both hold sRGB-encoded bytes need nothing (both raw, or both views convert).
float conversion(bool sourceEncoded, bool sourceViewSrgb, bool outputViewSrgb) {
 const bool sampledLinear = sourceViewSrgb || !sourceEncoded; // what the shader receives
 if(outputViewSrgb) return sampledLinear ? 0.f : 1.f;
 return sampledLinear && sourceViewSrgb ? 2.f : 0.f;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
 D3D12_RESOURCE_BARRIER b{};
 b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 b.Transition.pResource = resource;
 b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
 b.Transition.StateBefore = before;
 b.Transition.StateAfter = after;
 return b;
}

// Caller holds mutex. A free command list, or null when the GPU is far behind.
Slot* takeSlot() {
 Slot& s = slots[nextSlot];
 if(ID3D12Fence_GetCompletedValue(fence) < s.value) {
  ID3D12Fence_SetEventOnCompletion(fence, s.value, fenceEvent);
  if(WaitForSingleObject(fenceEvent, 20) != WAIT_OBJECT_0) return nullptr;
 }
 if(FAILED(ID3D12CommandAllocator_Reset(s.allocator)) || FAILED(ID3D12GraphicsCommandList_Reset(s.list, s.allocator, nullptr))) return nullptr;
 nextSlot = (nextSlot + 1) % ringSize;
 return &s;
}
void submit(Slot& s) {
 ID3D12GraphicsCommandList_Close(s.list);
 beamng_d3d12::executeOwn(queue, s.list);
 ID3D12CommandQueue_Signal(queue, fence, ++fenceValue);
 s.value = fenceValue;
}

bool compile(CompileFn compileFn, const char* entry, const char* target, ID3DBlob** code) {
 ID3DBlob* errors = nullptr;
 const HRESULT hr = compileFn(shaderSource, sizeof(shaderSource) - 1, "beamng_vrs_triple_view", nullptr, nullptr, entry, target,
   0x800 /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, code, &errors);
 if(FAILED(hr)) {
  char msg[600]{};
  sprintf_s(msg, "M8.55 quad views WARNING: shader %s failed hr=0x%08lX %s", entry, hr,
    errors ? static_cast<const char*>(ID3D10Blob_GetBufferPointer(errors)) : "");
  log(msg);
 }
 if(errors) ID3D10Blob_Release(errors);
 return SUCCEEDED(hr);
}

bool createRootSignature() {
 D3D12_DESCRIPTOR_RANGE range{};
 range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors = 2;range.BaseShaderRegister = 0;
 range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
 D3D12_ROOT_PARAMETER params[2]{};
 params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
 params[0].Constants.ShaderRegister = 0;params[0].Constants.Num32BitValues = 32;
 params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
 params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
 params[1].DescriptorTable.NumDescriptorRanges = 1;params[1].DescriptorTable.pDescriptorRanges = &range;
 params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_STATIC_SAMPLER_DESC sampler{};
 sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
 sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
 sampler.MaxLOD = D3D12_FLOAT32_MAX;sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_ROOT_SIGNATURE_DESC desc{};
 desc.NumParameters = 2;desc.pParameters = params;desc.NumStaticSamplers = 1;desc.pStaticSamplers = &sampler;
 auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
   reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature")));
 if(!serialize) { log("M8.55 quad views WARNING: D3D12SerializeRootSignature not found"); return false; }
 ID3DBlob* blob = nullptr;ID3DBlob* errors = nullptr;
 HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
 if(errors) ID3D10Blob_Release(errors);
 if(SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
   IID_ID3D12RootSignature, reinterpret_cast<void**>(&rootSignature));
 if(blob) ID3D10Blob_Release(blob);
 if(FAILED(hr)) { char msg[160]{};sprintf_s(msg, "M8.55 quad views WARNING: root signature failed hr=0x%08lX", hr);log(msg); }
 return SUCCEEDED(hr);
}

// Caller holds mutex. One pipeline per output format (the swapchain's never changes).
bool ensurePipeline(DXGI_FORMAT format) {
 if(pipeline && pipelineFormat == format) return true;
 if(pipeline) { ID3D12PipelineState_Release(pipeline);pipeline = nullptr; }
 D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
 d.pRootSignature = rootSignature;
 d.VS = {ID3D10Blob_GetBufferPointer(vsCode), ID3D10Blob_GetBufferSize(vsCode)};
 d.PS = {ID3D10Blob_GetBufferPointer(psCode), ID3D10Blob_GetBufferSize(psCode)};
 d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
 d.SampleMask = UINT_MAX;
 d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
 d.RasterizerState.DepthClipEnable = TRUE;
 d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
 d.NumRenderTargets = 1;d.RTVFormats[0] = format;d.SampleDesc.Count = 1;
 const HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(device, &d, IID_ID3D12PipelineState, reinterpret_cast<void**>(&pipeline));
 if(FAILED(hr)) { char msg[160]{};sprintf_s(msg, "M8.55 quad views WARNING: pipeline for format %u failed hr=0x%08lX", unsigned(format), hr);log(msg);return false; }
 pipelineFormat = format;
 return true;
}

// Caller holds mutex.
bool ensureSharp(int eye, const D3D12_RESOURCE_DESC& source) {
 Sharp& s = sharp[eye];
 if(s.copy && s.desc.Width == source.Width && s.desc.Height == source.Height && s.desc.Format == source.Format) return true;
 if(s.copy) { ID3D12Resource_Release(s.copy);s.copy = nullptr;s.valid = false; }
 D3D12_HEAP_PROPERTIES heap{};heap.Type = D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC d{};
 d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width = source.Width;d.Height = source.Height;
 d.DepthOrArraySize = 1;d.MipLevels = 1;d.Format = source.Format;d.SampleDesc.Count = 1;
 d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
 const HRESULT hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
   IID_ID3D12Resource, reinterpret_cast<void**>(&s.copy));
 if(FAILED(hr)) { char msg[160]{};sprintf_s(msg, "M8.55 quad views WARNING: eye copy %llux%u failed hr=0x%08lX", (unsigned long long)d.Width, d.Height, hr);log(msg);return false; }
 s.desc = d;s.state = D3D12_RESOURCE_STATE_COPY_DEST;
 return true;
}

// Caller holds mutex.
D3D12_CPU_DESCRIPTOR_HANDLE outputRtv(ID3D12Resource* resource, DXGI_FORMAT format, bool& ok) {
 const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(resource));
 D3D12_CPU_DESCRIPTOR_HANDLE h{};
 ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvHeap, &h);
 auto it = rtvSlots.find(key);
 UINT slot = 0;
 if(it == rtvSlots.end()) {
  if(rtvSlots.size() >= rtvCount) rtvSlots.clear(); // Swapchains were recreated; start over.
  slot = UINT(rtvSlots.size());
  h.ptr += SIZE_T(slot) * rtvIncrement;
  D3D12_RENDER_TARGET_VIEW_DESC v{};v.Format = format;v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  ID3D12Device_CreateRenderTargetView(device, resource, &v, h);
  rtvSlots[key] = slot;
 } else h.ptr += SIZE_T(it->second) * rtvIncrement;
 ok = true;
 return h;
}

} // namespace

bool init(void* d, void* q, LogFn fn) {
 std::lock_guard<std::mutex> g(mutex);
 if(isReady) return true;
 logFn = fn;
 device = static_cast<ID3D12Device*>(d);queue = static_cast<ID3D12CommandQueue*>(q);
 if(!device || !queue) return false;
 HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
 auto compileFn = compiler ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
 if(!compileFn) { log("M8.55 quad views WARNING: d3dcompiler_47.dll not found; quad views off");return false; }
 if(!compile(compileFn, "vs", "vs_5_0", &vsCode) || !compile(compileFn, "ps", "ps_5_0", &psCode) || !createRootSignature()) return false;
 D3D12_DESCRIPTOR_HEAP_DESC sd{};sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sd.NumDescriptors = ringSize * 4;
 sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 D3D12_DESCRIPTOR_HEAP_DESC rd{};rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rd.NumDescriptors = rtvCount;
 HRESULT hr = ID3D12Device_CreateDescriptorHeap(device, &sd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&srvHeap));
 if(SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(device, &rd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtvHeap));
 if(SUCCEEDED(hr)) hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, reinterpret_cast<void**>(&fence));
 for(auto& s : slots) {
  if(FAILED(hr)) break;
  hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, reinterpret_cast<void**>(&s.allocator));
  if(SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr,
    IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&s.list));
  if(SUCCEEDED(hr)) ID3D12GraphicsCommandList_Close(s.list);
 }
 fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
 if(FAILED(hr) || !fenceEvent) { char msg[160]{};sprintf_s(msg, "M8.55 quad views WARNING: device objects failed hr=0x%08lX; quad views off", hr);log(msg);return false; }
 srvIncrement = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 rtvIncrement = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 ID3D12Device_AddRef(device);ID3D12CommandQueue_AddRef(queue);
 isReady = true;
 log("M8.55 quad views: composition shader ready");
 return true;
}

bool ready() { return isReady; }

void captureEye(uint64_t texture, int eye) {
 if(!isReady || eye < 0 || eye > 1 || !texture) return;
 auto* source = reinterpret_cast<ID3D12Resource*>(uintptr_t(texture));
 D3D12_RESOURCE_DESC rd{};ID3D12Resource_GetDesc(source, &rd);
 if(rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1 || rd.DepthOrArraySize != 1) return;
 std::lock_guard<std::mutex> g(mutex);
 if(!ensureSharp(eye, rd)) return;
 Slot* s = takeSlot();
 if(!s) { skipped.fetch_add(1);return; }
 Sharp& sh = sharp[eye];
 D3D12_RESOURCE_BARRIER before[2]{transition(source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
 UINT n = 1;
 if(sh.state != D3D12_RESOURCE_STATE_COPY_DEST) before[n++] = transition(sh.copy, sh.state, D3D12_RESOURCE_STATE_COPY_DEST);
 ID3D12GraphicsCommandList_ResourceBarrier(s->list, n, before);
 ID3D12GraphicsCommandList_CopyResource(s->list, sh.copy, source);
 D3D12_RESOURCE_BARRIER after[2]{transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
   transition(sh.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
 ID3D12GraphicsCommandList_ResourceBarrier(s->list, 2, after);
 sh.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
 submit(*s);
 sh.valid = true;
}

bool compose(const EyeInput eyes[2], const uint64_t outputs[2], bool middleWanted) {
 if(!isReady || !outputs[0] || !outputs[1]) return false;
 std::lock_guard<std::mutex> g(mutex);
 if(!sharp[0].valid || !sharp[1].valid) return false;
 auto* out0 = reinterpret_cast<ID3D12Resource*>(uintptr_t(outputs[0]));
 D3D12_RESOURCE_DESC od{};ID3D12Resource_GetDesc(out0, &od);
 // A typeless output is written raw (UNORM); a typed one through its own format.
 const DXGI_FORMAT outFormat = viewFormat(od.Format, false);
 const bool outSrgb = isSrgb(outFormat);
 if(!ensurePipeline(outFormat)) return false;

 // M8.55: each eye's own wide view; a view not drawn for 45 frames reads black.
 beamng_d3d12::MiddleTarget m[2]{};
 ID3D12Resource* middleResource[2]{};
 D3D12_RESOURCE_DESC md[2]{};
 for(int eye = 0; eye < 2; ++eye) {
  if(middleWanted && beamng_d3d12::middleTarget(eye, m[eye]) && beamng_d3d12::frameCount() - m[eye].bindFrame < 45)
   middleResource[eye] = reinterpret_cast<ID3D12Resource*>(uintptr_t(m[eye].resource));
  if(middleResource[eye]) ID3D12Resource_GetDesc(middleResource[eye], &md[eye]);
 }

 Slot* s = takeSlot();
 if(!s) { skipped.fetch_add(1);return false; }
 const UINT base = UINT((s - slots) * 4);
 D3D12_CPU_DESCRIPTOR_HANDLE cpu{};D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
 ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srvHeap, &cpu);
 ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvHeap, &gpu);

 float sharpMode[2]{}, middleMode[2]{};
 for(int eye = 0; eye < 2; ++eye) {
  const DXGI_FORMAT sf = viewFormat(sharp[eye].desc.Format, outSrgb && isTypeless(sharp[eye].desc.Format));
  D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format = sf;v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;h.ptr += SIZE_T(base + eye * 2) * srvIncrement;
  ID3D12Device_CreateShaderResourceView(device, sharp[eye].copy, &v, h);
  // The game's eye images hold sRGB-encoded colours.
  sharpMode[eye] = conversion(true, isSrgb(sf), outSrgb);
  D3D12_SHADER_RESOURCE_VIEW_DESC mv = v;
  ID3D12Resource* mr = middleResource[eye];
  if(mr) {
   mv.Format = viewFormat(md[eye].Format, outSrgb && isTypeless(md[eye].Format));
   // The wide view's final 8-bit image is sRGB-encoded like the eyes' (BeamNG's UNORM targets hold encoded bytes).
   middleMode[eye] = conversion(true, isSrgb(mv.Format), outSrgb);
  } else mv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Null descriptor: reads black.
  h.ptr += srvIncrement;
  ID3D12Device_CreateShaderResourceView(device, mr, &mv, h);
 }

 D3D12_RESOURCE_STATES middleState[2]{};
 bool middleBarrier[2]{};
 for(int eye = 0; eye < 2; ++eye) {
  middleState[eye] = D3D12_RESOURCE_STATES(m[eye].state);
  middleBarrier[eye] = middleResource[eye] && !(middleState[eye] & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if(middleBarrier[eye]) {
   const auto b = transition(middleResource[eye], middleState[eye], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   ID3D12GraphicsCommandList_ResourceBarrier(s->list, 1, &b);
  }
 }
 ID3D12GraphicsCommandList_SetGraphicsRootSignature(s->list, rootSignature);
 ID3D12GraphicsCommandList_SetPipelineState(s->list, pipeline);
 ID3D12GraphicsCommandList_SetDescriptorHeaps(s->list, 1, &srvHeap);
 ID3D12GraphicsCommandList_IASetPrimitiveTopology(s->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 for(int eye = 0; eye < 2; ++eye) {
  auto* out = reinterpret_cast<ID3D12Resource*>(uintptr_t(outputs[eye]));
  D3D12_RESOURCE_DESC d{};ID3D12Resource_GetDesc(out, &d);
  bool ok = false;
  const auto rtv = outputRtv(out, outFormat, ok);
  ID3D12GraphicsCommandList_OMSetRenderTargets(s->list, 1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0, 0, float(d.Width), float(d.Height), 0, 1};
  const D3D12_RECT sc{0, 0, LONG(d.Width), LONG(d.Height)};
  ID3D12GraphicsCommandList_RSSetViewports(s->list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(s->list, 1, &sc);
  beamng_triple::Constants c = eyes[eye].constants;
  const double sw = double(sharp[eye].desc.Width), sh = double(sharp[eye].desc.Height);
  c.rect[0] = float(eyes[eye].rectX / sw);c.rect[1] = float(eyes[eye].rectY / sh);
  c.rect[2] = float((eyes[eye].rectX + eyes[eye].rectWidth) / sw);c.rect[3] = float((eyes[eye].rectY + eyes[eye].rectHeight) / sh);
  c.middle[3] = middleResource[eye] ? 1.f : 0.f;
  c.flags[1] = sharpMode[eye];c.flags[2] = middleMode[eye];
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(s->list, 0, 32, &c, 0);
  D3D12_GPU_DESCRIPTOR_HANDLE table = gpu;table.ptr += UINT64(base + eye * 2) * srvIncrement;
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(s->list, 1, table);
  ID3D12GraphicsCommandList_DrawInstanced(s->list, 3, 1, 0, 0);
 }
 for(int eye = 0; eye < 2; ++eye)
  if(middleBarrier[eye]) {
   const auto b = transition(middleResource[eye], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, middleState[eye]);
   ID3D12GraphicsCommandList_ResourceBarrier(s->list, 1, &b);
  }
 submit(*s);
 const uint64_t n = composed.fetch_add(1) + 1;
 if(n == 1 || n % 5400 == 0) {
  char msg[320]{};
  auto status = [&](int eye) { return middleResource[eye] ? "on" : middleWanted ? "not found yet" : "off"; };
  sprintf_s(msg, "M8.55 quad views composed=%llu skipped=%llu output %llux%u format %u, focus %llux%u, wide L %s R %s (format %u state 0x%X)",
    (unsigned long long)n, (unsigned long long)skipped.load(), (unsigned long long)od.Width, od.Height, unsigned(outFormat),
    (unsigned long long)sharp[0].desc.Width, sharp[0].desc.Height, status(0), status(1), unsigned(md[0].Format), m[0].state);
  log(msg);
 }
 return true;
}

} // namespace beamng_triple_d3d12
