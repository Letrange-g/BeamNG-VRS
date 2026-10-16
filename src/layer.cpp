// Vulkan side of BeamNG-VRS. Public INI parsing lives in ini_config.hpp;
// OpenXR eye/gaze discovery lives in openxr_probe.cpp. See docs/SOURCE-GUIDE.md.
// Most M8.xx names are historical experiment identifiers, not user settings.
// This layer forwards to the real driver using copied dispatch tables. Resource
// maps use mu; async uploads must retain buffers/images until their fence signals.
#include "windows_config.hpp"
// BeamNG VRS M5.7 - opt-in experimental shadow render-pass binding.
#include "ffr_pattern.hpp"
#include "beamng_gaze_shared.hpp"
#include "ffr_gpu_image.hpp"
#include "ffr_upload_commands.hpp"
#include "m818_stencil_mask.hpp"
#include "m821_gaze_timing.hpp"
#include "m825_gaze_hold.hpp"
#include "gaze_freshness.hpp"
#include "scissor_crop.hpp"
#include "early_vrs.hpp"
#include "shared_scene_vrs.hpp"
#include "unorm_pipeline_vrs.hpp"
#include "unorm_compare.hpp"
#include "framebuffer_reuse.hpp"
#include "early_cpu_timing.hpp"
#include "gpu_pass_profile.hpp"
#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <array>
#include <vector>
#include <algorithm>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <map>
#include <string>
#include <type_traits>

// Forward declarations required by loader negotiation and dispatch lookup.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);

namespace {
uint32_t m813Width();
uint32_t m813Height();
std::atomic<uint32_t> m814ActiveWidth{5190},m814ActiveHeight{4168};
// M8.25: suppression is DEBUG-OUTLINE ONLY; VRS never pauses during resize.
std::atomic<uint64_t> m825LastResizeMs{0};
uint32_t m825SettingMs(const char* name,uint32_t fallback,uint32_t max){
 char b[24]{};const DWORD len=beamng_config::read(name,b,sizeof(b));
 if(!len || len>=sizeof(b))return fallback;
 char* end=nullptr;const unsigned long value=strtoul(b,&end,10);
 return end!=b && !*end && value<=max ? static_cast<uint32_t>(value):fallback;
}
uint32_t m825GraceMs(){static const uint32_t ms=m825SettingMs("BEAMNG_VRS_GAZE_GRACE_MS",30,1000);return ms;}
uint32_t m825OutlinePauseMs(){static const uint32_t ms=m825SettingMs("BEAMNG_VRS_OUTLINE_RESIZE_PAUSE_MS",350,2000);return ms;}


std::mutex mu;
struct InstanceDispatch { PFN_vkGetInstanceProcAddr gipa; };
std::unordered_map<VkInstance, InstanceDispatch> instances;
struct DeviceDispatch {
#define EARLY_ENTRY(result,name,args) PFN_##name early_##name=nullptr;
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY
 PFN_vkGetDeviceProcAddr gdpa;
 beamng_ffr::Api ffrApi{};
 PFN_vkGetDeviceQueue getDeviceQueue;
 PFN_vkGetDeviceQueue2 getDeviceQueue2;
 PFN_vkQueueSubmit m812gQueueSubmit = nullptr;
 PFN_vkQueueSubmit2 m812gQueueSubmit2 = nullptr;
 PFN_vkQueueSubmit2KHR m812gQueueSubmit2KHR = nullptr;
 PFN_vkCreateFence m812hCreateFence = nullptr;
 PFN_vkGetFenceStatus m812hGetFenceStatus = nullptr;
 PFN_vkDestroyFence m812hDestroyFence = nullptr;
 PFN_vkDestroyDevice destroyDevice;
 VkPhysicalDevice physical = VK_NULL_HANDLE;
 PFN_vkCreateGraphicsPipelines createPipelines;
 PFN_vkCmdBindPipeline cmdBindPipeline;
 PFN_vkDestroyPipeline destroyPipeline;
 bool vrsEnabled;
 bool unorm4x4Supported=false;
 PFN_vkCreateRenderPass createRenderPass;
 PFN_vkDestroyRenderPass destroyRenderPass;
 PFN_vkCreateRenderPass2 createRenderPass2;
 PFN_vkCreateFramebuffer createFramebuffer;
 PFN_vkCmdCopyImage cmdCopyImage;
 PFN_vkCmdBlitImage cmdBlitImage;
 PFN_vkCmdResolveImage cmdResolveImage;
 PFN_vkCmdCopyImage2 cmdCopyImage2;
 PFN_vkCmdBlitImage2 cmdBlitImage2;
 PFN_vkCmdResolveImage2 cmdResolveImage2;
 PFN_vkCreateImageView createImageView;
 PFN_vkDestroyImageView destroyImageView;
 PFN_vkCreateImage createImage;
 PFN_vkAllocateCommandBuffers allocateCommandBuffers;
 PFN_vkFreeCommandBuffers freeCommandBuffers;
 PFN_vkCmdBeginRendering cmdBeginRendering;
 PFN_vkCmdBeginRenderingKHR cmdBeginRenderingKHR;
 PFN_vkCmdBeginRenderPass cmdBeginRenderPass;
 PFN_vkCmdBeginRenderPass2 cmdBeginRenderPass2;
 PFN_vkDestroyFramebuffer destroyFramebuffer;
 PFN_vkCmdSetScissor cmdSetScissor;
 PFN_vkCmdClearAttachments cmdClearAttachments;
 PFN_vkCmdEndRenderPass cmdEndRenderPass;
 PFN_vkCmdEndRenderPass2 cmdEndRenderPass2;
 PFN_vkCmdNextSubpass cmdNextSubpass;
 PFN_vkCmdNextSubpass2 cmdNextSubpass2;
};
// Dispatch snapshots are copied on every intercepted command. Keep owning
// containers out of this structure so copying it never allocates on the heap.
static_assert(std::is_trivially_copyable_v<DeviceDispatch>);
// Hot dispatch copies must not own vectors/maps: copying them per draw would
// allocate repeatedly. Queue-family ownership stays in this device-side map.
std::unordered_map<VkDevice,std::vector<uint32_t>> m827GraphicsFamilies;
std::unordered_map<VkCommandBuffer, VkDevice> commandDevices;
// Per-command-buffer crop context. eye=0 is left, 1 right, -1 unknown. Values
// remain tied to the render pass that began recording, not to later frame order.
struct CropState { bool active=false; uint32_t width=0,height=0; VkRect2D renderArea{}; uint32_t subpass=0; int eye=-1; };
std::unordered_map<VkCommandBuffer,CropState> cropStates;
bool settingFlag(const char* name){char v[8]{};return beamng_config::read(name,v,sizeof(v)) && strcmp(v,"1")==0;}
int m833Mode(){
 static const int mode=[] {char v[16]{};beamng_config::read("BEAMNG_VRS_EARLY_PASSES",v,sizeof(v));
  return !strcmp(v,"on")?2:!strcmp(v,"trace")?1:0;}();return mode;
}
void log(const char*);
bool m838Enabled(){static const bool on=settingFlag("BEAMNG_VRS_UNORM_SCENE") &&
 settingFlag("BEAMNG_VRS_M8_EXPERIMENTAL") && m833Mode()==0;return on;}
bool m838Compare(){static const bool on=m838Enabled() && settingFlag("BEAMNG_VRS_COMPARE_UNORM");return on;}
bool m835Enabled(){static const bool on=settingFlag("BEAMNG_VRS_PROFILE_GPU") || m838Compare();return on;}
beamng_profile::Profiler m835Profiler(log);
bool m835Entry(const char* name){
 // Profile mode needs descriptor/resource lifetime hooks for sampled resource
 // links. This does not activate the early-pass classifier or widen VRS scope.
 (void)name;return m835Enabled();
}
void m835BeginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,bool,bool);
bool m833Enabled(){static const bool enabled=m833Mode()!=0 && settingFlag("BEAMNG_VRS_M8_EXPERIMENTAL");return enabled;}
bool m837ApplyShared(){static const bool enabled=settingFlag("BEAMNG_VRS_SHARED_SCENE") &&
 settingFlag("BEAMNG_VRS_M8_EXPERIMENTAL") && m833Mode()==0;return enabled;}
bool m837Enabled(){return m837ApplyShared() || m838Enabled();}
bool m837Tracking(){return m833Enabled() || m837Enabled();}
std::unordered_map<VkDevice,beamng_shared::Tracker> m837Trackers;
std::unordered_set<VkImage> m837SceneImages;
std::unordered_set<VkRenderPass> m837ScenePasses;
std::unordered_set<VkCommandBuffer> m837ActiveShared;
std::unordered_set<VkImage> m838UnormImages;
std::unordered_set<VkRenderPass> m838UnormPasses;
std::unordered_map<VkRenderPass,uint32_t> m838DepthSlots;
std::unordered_map<VkRenderPass,VkDevice> m838PassDevices;
struct M838Scope { bool candidate=false,on=false,selected=false; int phase=-1; uint64_t target=0; };
std::unordered_map<VkCommandBuffer,M838Scope> m838Scopes;
std::unordered_map<VkDevice,beamng_unorm::CompareClock> m838Clocks;
std::unordered_set<VkImageView> m838DepthViews;
struct M838DepthImage { VkDevice device; uint32_t width,height; };
std::unordered_map<VkImage,M838DepthImage> m838DepthImages;
std::unordered_map<VkCommandBuffer,VkPipeline> m838OriginalBindings;
std::unordered_set<VkCommandBuffer> m838BoundCopies;
// Producer evidence is per device and never feeds the scissor classifier.
std::unordered_map<VkDevice,beamng_early::Tracker> m833Trackers;
template<class Work> void m837Each(VkDevice device,Work work){
 if(m833Enabled())work(m833Trackers[device]);
 if(m837Enabled())work(m837Trackers[device]);
}
std::unordered_map<VkRenderPass,uint32_t> m833ColorSlots;
std::unordered_set<VkImageView> m833ColorViews;
std::unordered_set<VkCommandBuffer> m833ActiveEarlier;
template<class T> uint64_t m833Id(T handle){return uint64_t(uintptr_t(handle));}
void m833Begin(VkCommandBuffer,const VkRenderPassBeginInfo*,bool);
void m833End(VkCommandBuffer);
void m833Invalidate(VkCommandBuffer);
int m833Eye(VkDevice,VkCommandBuffer,const VkRenderPassBeginInfo*);
void m833ForgetImage(VkImage);
void m833Report(VkDevice);
bool m837Select(VkDevice,VkCommandBuffer,const VkRenderPassBeginInfo*);
void m837Submitted(VkQueue,const std::vector<uint64_t>&);
bool cropEnabled(){static const bool on=settingFlag("BEAMNG_VRS_SCISSOR_ENABLE");return on;}
bool outlineEnabled(){static const bool on=settingFlag("BEAMNG_VRS_DEBUG_OUTLINE");return on;}
// Read at process startup (restart BeamNG to apply new crop settings).
// Fractions of eye height for top/bottom, eye width for left/right.
double cropFraction(const char* key, double fallback) {
  char buffer[64] = {};
  DWORD len = beamng_config::read(key, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (!len || len >= sizeof(buffer)) return fallback;
  char* end = nullptr;
  const double value = strtod(buffer, &end);
  if (end == buffer || *end != '\0' || !std::isfinite(value)) return fallback;
  return std::clamp(value, 0.0, 0.49);
}
const std::array<beamng_scissor::Margins,2>& cropMargins() {
  static const auto margins=[] {
    const double top=cropFraction("BEAMNG_VRS_SCISSOR_TOP",0.30);
    const double bottom=cropFraction("BEAMNG_VRS_SCISSOR_BOTTOM",0.30);
    const double left=cropFraction("BEAMNG_VRS_SCISSOR_LEFT",0.0);
    const double right=cropFraction("BEAMNG_VRS_SCISSOR_RIGHT",0.0);
    return std::array<beamng_scissor::Margins,2>{{
      {top,bottom,cropFraction("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT",left),
                  cropFraction("BEAMNG_VRS_SCISSOR_LEFT_EYE_RIGHT",right)},
      {top,bottom,cropFraction("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT",left),
                  cropFraction("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT",right)}}};
  }();
  return margins;
}
bool cropAsymmetric() {
  const auto& m=cropMargins();
  return m[0].left!=m[1].left || m[0].right!=m[1].right;
}
beamng_scissor::Crop cropGeometry(uint32_t w,uint32_t h,int eye=-1) {
  const auto& m=cropMargins();
  if(eye==0 || eye==1)return beamng_scissor::make(w,h,m[eye]);
  // Until an eye is known, retain everything either eye needs. Never guess by
  // render-pass order: classification comes from OpenXR image transfers.
  return beamng_scissor::make(w,h,{m[0].top,m[0].bottom,
    std::min(m[0].left,m[1].left),std::min(m[0].right,m[1].right)});
}
VkRect2D cropVkRect(beamng_scissor::Rect r) {
  return {{static_cast<int32_t>(r.x),static_cast<int32_t>(r.y)},{r.width,r.height}};
}
VkRect2D cropRect(uint32_t w,uint32_t h,int eye=-1) {
  return cropVkRect(cropGeometry(w,h,eye).keep);
}
VkRect2D intersectRect(VkRect2D a,VkRect2D b) {
  int64_t x=std::max<int64_t>(a.offset.x,b.offset.x), y=std::max<int64_t>(a.offset.y,b.offset.y);
  int64_t r=std::min<int64_t>(int64_t(a.offset.x)+a.extent.width,int64_t(b.offset.x)+b.extent.width);
  int64_t d=std::min<int64_t>(int64_t(a.offset.y)+a.extent.height,int64_t(b.offset.y)+b.extent.height);
  return {{static_cast<int32_t>(x),static_cast<int32_t>(y)},{static_cast<uint32_t>(std::max<int64_t>(0,r-x)),static_cast<uint32_t>(std::max<int64_t>(0,d-y))}};
}

// M8.14: manual override remains optional; otherwise the active dimensions
// track OpenXR eye swapchain resolution. Initial 5190x4168 resources preserve
// the proven M8.13 boot path until the first actual eye target is identified.
uint32_t m813Dimension(const char* key, uint32_t fallback) {
 char text[32]{};
 DWORD len=beamng_config::read(key,text,sizeof(text));
 if(!len || len>=sizeof(text))return fallback;
 char* end=nullptr;
 unsigned long v=strtoul(text,&end,10);
 return end!=text && *end=='\0' && v>=64 && v<=16384 ? static_cast<uint32_t>(v) : fallback;
}
uint32_t m813Width(){return m814ActiveWidth.load(std::memory_order_acquire);}
uint32_t m813Height(){return m814ActiveHeight.load(std::memory_order_acquire);}
struct FramebufferInfo { uint32_t width, height, layers, attachments; };
std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffers;
// M7.4g: diagnostic-only VkImageView -> VkImage association.
struct M74gView { VkDevice device; VkImage image; };
std::unordered_map<VkImageView, M74gView> m74gViews;
uint32_t m74gLogCount = 0;
// M7.4d: identify render-target attachment views without changing Vulkan objects.
struct M74dAttachmentSet { std::vector<VkImageView> views; VkRenderPass pass; };
std::unordered_map<VkFramebuffer, M74dAttachmentSet> m74dAttachments;
std::unordered_set<VkFramebuffer> m74dReported;

struct PassInfo { uint32_t attachments, subpasses, colors, samples; bool hasDepth, eligible; };
std::unordered_map<VkRenderPass, PassInfo> passInfos;
// M8.8: compatibility signature of legacy VkRenderPassCreateInfo.
// Conservative compatibility key: identical descriptions except for the
// load/store operations and layouts Vulkan explicitly permits to differ.
// Keeping attachment indices/counts exact can miss legal matches, but cannot
// broaden compatibility to unrelated targets. Unknown extension chains fail
// closed. This key is also used to select VRS pipelines, not just diagnostics.
struct M88PassSignature {
 std::vector<uint64_t> fields;
};
std::unordered_map<VkRenderPass,M88PassSignature> m88PassSignatures;
std::unordered_set<uint64_t> m88ReportedPairs;
std::atomic<unsigned> m88Reports{0};
static M88PassSignature m88Signature(const VkRenderPassCreateInfo& ci) {
 M88PassSignature s;
 if(ci.pNext || (ci.attachmentCount && !ci.pAttachments) ||
    !ci.subpassCount || !ci.pSubpasses || (ci.dependencyCount && !ci.pDependencies))return s;
 auto add=[&](uint64_t v){s.fields.push_back(v);};
 add(ci.flags);add(ci.attachmentCount);add(ci.subpassCount);
 for(uint32_t i=0;i<ci.attachmentCount;++i){
  const auto& a=ci.pAttachments[i];add(a.flags);add(uint32_t(a.format));add(uint32_t(a.samples));
 }
 for(uint32_t i=0;i<ci.subpassCount;++i){
  const auto& sp=ci.pSubpasses[i];
  add(uint32_t(sp.flags));add(uint32_t(sp.pipelineBindPoint));
  add(sp.inputAttachmentCount);add(sp.colorAttachmentCount);
  auto ref=[&](const VkAttachmentReference* r){add(r?uint64_t(r->attachment):uint64_t(VK_ATTACHMENT_UNUSED));};
  for(uint32_t j=0;j<sp.inputAttachmentCount;++j)ref(sp.pInputAttachments?&sp.pInputAttachments[j]:nullptr);
  for(uint32_t j=0;j<sp.colorAttachmentCount;++j)ref(sp.pColorAttachments?&sp.pColorAttachments[j]:nullptr);
  add(sp.pResolveAttachments?1:0);
  if(sp.pResolveAttachments)for(uint32_t j=0;j<sp.colorAttachmentCount;++j)ref(&sp.pResolveAttachments[j]);
  ref(sp.pDepthStencilAttachment);
  add(sp.preserveAttachmentCount);
  for(uint32_t j=0;j<sp.preserveAttachmentCount;++j)add(sp.pPreserveAttachments?sp.pPreserveAttachments[j]:VK_ATTACHMENT_UNUSED);
 }
 add(ci.dependencyCount);
 for(uint32_t i=0;i<ci.dependencyCount;++i){
  const auto& dep=ci.pDependencies[i];
  add(dep.srcSubpass);add(dep.dstSubpass);add(dep.srcStageMask);add(dep.dstStageMask);
  add(dep.srcAccessMask);add(dep.dstAccessMask);add(dep.dependencyFlags);
 }
 return s;
}
static bool m832Compatible(const M88PassSignature& a,const M88PassSignature& b){
 return !a.fields.empty() && a.fields==b.fields;
}
// All shadows use the same added R8_UINT attachment, 16x16 tile size and
// single-subpass translation. Equal original keys therefore imply equal
// shadow compatibility. Entries live exactly as long as their shadow pass.
std::unordered_map<VkRenderPass,M88PassSignature> m832ShadowSignatures;

struct ShadowPass { VkDevice device; VkRenderPass shadow; };
struct ShadowFramebuffer { VkDevice device; VkFramebuffer shadow; };
std::unordered_map<VkFramebuffer, ShadowFramebuffer> shadowFramebuffers;
std::unordered_map<VkRenderPass, ShadowPass> shadowPasses;
std::unordered_map<VkRenderPass, uint32_t> shadowRejectFlags;
std::unordered_set<VkRenderPass> reportedPasses;
std::unordered_set<unsigned long long> m53FramebufferPairs;
std::unordered_set<unsigned long long> m53PipelinePasses;
// M8.7: track actual pipeline handles, rather than only render-pass hashes.
struct M87Pipeline { VkDevice device; VkRenderPass renderPass; uint32_t subpass; };
std::unordered_map<VkPipeline,M87Pipeline> m87Pipelines;
// M8.9: shadow-compatible pipeline variants, owned by the layer.
struct M89Variant { VkDevice device; VkPipeline pipeline; VkRenderPass shadowPass;
                    VkPipeline maskedPipeline=VK_NULL_HANDLE;
                    // Pipelines may outlive the render pass used to create them.
                    // Own the key so destroyed/recycled handles cannot change it.
                    M88PassSignature compatibility; };
std::unordered_map<VkPipeline,M89Variant> m89Variants;
struct M838Variant { VkDevice device; VkPipeline pipeline; M88PassSignature compatibility; };
std::unordered_map<VkPipeline,M838Variant> m838Variants;
std::atomic<uint64_t> m838VariantCreated{0},m838VariantSkipped{0},m838VariantFailed{0};
// M8.30: only asymmetric static scissors need additional pipeline copies.
// Keyed by the real base pipeline, including compatible VRS/stencil variants.
struct M830CropVariants { VkDevice device; std::array<VkPipeline,2> eyes{}; };
std::unordered_map<VkPipeline,M830CropVariants> m830CropVariants;
std::unordered_map<VkCommandBuffer,VkRenderPass> m89ActiveShadows;
// M8.18: true stencil rejection is only legal for an eye render pass which
// already owns a writable stencil attachment. Never invent or replace BeamNG's depth.
uint64_t m814Key(uint32_t w,uint32_t h);
std::unordered_map<VkRenderPass,bool> m818StencilCapable;
std::unordered_map<VkRenderPass,VkFormat> m819DepthFormatByPass;
std::atomic<unsigned> m819DepthOnlyPasses{0},m819StencilPasses{0},m819NoDepthPasses{0};

std::unordered_map<VkRenderPass,uint32_t> m818MaskedVariantsByPass;
std::unordered_map<VkCommandBuffer,int> m818SelectedEye;
std::unordered_set<VkCommandBuffer> m818MaskedCommands;
std::atomic<unsigned long long> m818MaskedCount{0},m818NoStencilCount{0},m818NoMeshCount{0};
std::atomic<unsigned long long> m818StencilPipelineBinds{0};
bool m818Enabled(){static const bool value=[](){char b[8]{};
 return beamng_config::read("BEAMNG_VRS_M818_STENCIL",b,sizeof(b))&&strcmp(b,"1")==0;
 }();return value;
}
// Geometry is immutable until session recreation, but resources change with resolution.
// Protect cached raster strips separately from existing Vulkan state mutex.
struct M818Rects { uint32_t sourceSequence=0;std::vector<VkClearRect> visible; };
std::map<uint64_t,std::array<M818Rects,2>> m818RectCache;
std::mutex m818RectMutex;
HANDLE m818MeshMapping=nullptr;
beamng_visibility_mesh::Data* m818MeshShared=nullptr;
bool m818ReadMesh(int eye,beamng_visibility_mesh::Eye& out,uint32_t& seq){
 if(eye<0||eye>1)return false;
 if(!m818MeshShared){
  m818MeshMapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_visibility_mesh::mappingName);
  if(m818MeshMapping)m818MeshShared=static_cast<beamng_visibility_mesh::Data*>(
      MapViewOfFile(m818MeshMapping,FILE_MAP_READ,0,0,sizeof(beamng_visibility_mesh::Data)));
 }
 if(!m818MeshShared)return false;
 auto* d=m818MeshShared;
 for(int attempt=0;attempt<3;++attempt){
  const LONG first=d->sequence;MemoryBarrier();
  if(first&1)continue;
  if(d->magic!=beamng_visibility_mesh::magicValue || d->processId!=GetCurrentProcessId() ||
     !(d->validMask&(1u<<eye)))return false;
  auto tmp=d->eyes[eye];MemoryBarrier();
  if(first==d->sequence && !(first&1)){
   if(tmp.count<3||tmp.count>beamng_visibility_mesh::maxVertices)return false;
   out=tmp;seq=(uint32_t)first;return true;
  }
 }
 return false;
}
bool m818VisibilityRects(int eye,uint32_t width,uint32_t height,std::vector<VkClearRect>& out){
 beamng_visibility_mesh::Eye mesh{};uint32_t sequence=0;
 if(!m818ReadMesh(eye,mesh,sequence))return false;
 const auto key=m814Key(width,height);
 {
  std::lock_guard<std::mutex> guard(m818RectMutex);
  auto it=m818RectCache.find(key);
  if(it!=m818RectCache.end() && it->second[eye].sourceSequence==sequence){
   out=it->second[eye].visible;return !out.empty();
  }
 }
 std::array<beamng_stencil::Point,beamng_visibility_mesh::maxVertices> points{};
 for(uint32_t i=0;i<mesh.count;++i)points[i]={mesh.vertices[i].x,mesh.vertices[i].y};
 beamng_stencil::Fov f{mesh.tanLeft,mesh.tanRight,mesh.tanDown,mesh.tanUp};
 std::vector<beamng_stencil::Strip> spans;
 if(!beamng_stencil::makeStrips(points.data(),mesh.count,f,width,height,spans,48,.025f))return false;
 std::vector<VkClearRect> made;
 for(const auto& r:spans)if(r.width&&r.height)
  made.push_back({{{int32_t(r.x),int32_t(r.y)},{r.width,r.height}},0,1});
 if(made.empty())return false;
 {
  std::lock_guard<std::mutex> guard(m818RectMutex);
  auto& cached=m818RectCache[key][eye];cached={sequence,made};
 }
 out=std::move(made);return true;
}

// M8.10: capped, independent diagnostics (avoid flooding the VR render thread).
std::atomic<unsigned> m810ShadowReports{0};
std::atomic<unsigned> m810BindReports{0};
std::atomic<unsigned> m810MissingReports{0};
std::atomic<unsigned> m89VariantReports{0};

std::unordered_map<VkCommandBuffer,VkRenderPass> m87ActivePasses;
std::unordered_map<VkCommandBuffer,uint32_t> m87PassBindCounts;
std::atomic<unsigned> m87Reports{0};
std::unordered_set<unsigned long long> seenBeginDimensions;
std::unordered_set<unsigned long long> seenImageDimensions;
std::unordered_map<VkDevice, DeviceDispatch> devices;
struct CapturedQueue { VkQueue queue; uint32_t family; uint32_t index; };
std::unordered_map<VkDevice, std::vector<CapturedQueue>> capturedQueues;
std::unordered_map<VkDevice, beamng_ffr::GpuImage> uploadedPatterns;
// M8.3: independent left/right shading-rate images. Static until a safe
// frame-synchronized update path is implemented.
std::unordered_map<VkDevice, std::array<beamng_ffr::GpuImage,2>> m83EyePatterns;
// M8.16: old spare GPU pool structure retained for resource-generation
// compatibility; the eye-tracked uploader uses the two primary per-eye images.
struct M812ImagePool { std::array<std::array<beamng_ffr::GpuImage,2>,2> extra{}; };
std::unordered_map<VkDevice,M812ImagePool> m812ImagePools;
// M8.12g: observe application submissions without modifying them.
std::unordered_map<VkQueue,VkDevice> m812gQueueDevices;
// M8.12h: queue-order completion markers; no staging/image slots are reused yet.
struct M812hMarker { VkDevice device; VkQueue queue; VkFence fence; uint64_t serial; };
std::vector<M812hMarker> m812hPending;
uint64_t m812hNextSerial=0;


// M8.12a: three persistently mapped transfer-source buffers per eye.
// No GPU writes/reads or image rebinding yet; safe preparation stage only.
struct M812Staging { std::array<std::array<beamng_ffr::StagingSlot,3>,2> slots{}; };
std::unordered_map<VkDevice, M812Staging> m812Staging;
// M8.14: cache a COMPLETE resource generation for each observed resolution.
// Old generations stay alive until vkDestroyDevice; old application's framebuffers
// may still refer to their views after a swapchain resize.
struct M814ResourceSet {
 beamng_ffr::GpuImage base{};
 std::array<beamng_ffr::GpuImage,2> eyes{};
 M812ImagePool imagePool{};
 M812Staging staging{};
};
std::unordered_map<VkDevice,std::map<uint64_t,M814ResourceSet>> m814ResourceSets;
std::mutex m814ResourceMutex;
uint64_t m814Key(uint32_t w,uint32_t h){return (uint64_t(w)<<32)|h;}

// M8.12b: CPU-to-host-visible staging only. No GPU transfer until safe synchronization is installed.
std::array<uint64_t,2> m812bPreparedCount{};
struct M825PatternCenter { float u=.5f, v=.5f; };
std::array<M825PatternCenter,2> m825StagedCenters{}; // guarded by mu

// M8.12-live: last fully uploaded CPU sequence per eye.
std::unordered_map<VkDevice,std::array<uint64_t,2>> m812liveUploaded;
std::mutex m812liveQueueMutex;
// M8.15: each (device, resolution) owns three persistently mapped source slots
// per eye.  An in-flight upload pins its source slot until the upload fence
// signals; CPU preparation skips pinned slots rather than blocking a VR frame.
struct M815SlotState {
 std::array<std::array<bool,3>,2> busy{};
 // Source timestamp follows a particular pinned/free staging slot so upload
 // telemetry measures the sample actually submitted, not the newest eye pose.
 std::array<std::array<uint64_t,3>,2> sourceTickMs{};
 std::array<std::array<uint64_t,3>,2> preparedTickMs{};
 std::array<int,2> latestSlot{{-1,-1}};
 std::array<uint64_t,2> latestSeq{};
};
struct M815Upload : beamng_ffr::UploadCommands {
 VkDevice device=VK_NULL_HANDLE;
 uint64_t resolution=0;
 std::array<int,2> slots{{-1,-1}};
};
std::unordered_map<VkDevice,std::map<uint64_t,M815SlotState>> m815SlotStates;
std::vector<M815Upload> m815Pending;
std::unordered_map<VkDevice,beamng_ffr::UploadCommandCache> m827UploadCaches;
struct M821GazeStats {
 std::atomic<uint64_t> staged{0}, unchanged{0}, identicalPattern{0}, stale{0}, noFreeSlot{0};
 std::atomic<uint64_t> uploadSamples{0}, uploadAgeMsSum{0}, uploadAgeMsMax{0};
 std::atomic<uint64_t> prepareAgeMsSum{0};
};
M821GazeStats m821GazeStats;
uint32_t m821UpdateIntervalMs(){static const uint32_t interval=[](){
 char value[16]{};const DWORD n=beamng_config::read("BEAMNG_VRS_GAZE_UPDATE_MS",value,sizeof(value));
 if(!n || n>=sizeof(value))return 8u; // <= one 90 Hz frame, ~125 Hz polling maximum
 char* end=nullptr;unsigned long requested=strtoul(value,&end,10);
 return (end==value || *end) ? 8u : beamng_gaze_timing::clampInterval(requested);
 }();return interval;}
void m821RecordUploadAge(uint64_t ageMs){
 m821GazeStats.uploadSamples.fetch_add(1,std::memory_order_relaxed);
 m821GazeStats.uploadAgeMsSum.fetch_add(ageMs,std::memory_order_relaxed);
 auto prior=m821GazeStats.uploadAgeMsMax.load(std::memory_order_relaxed);
 while(prior<ageMs && !m821GazeStats.uploadAgeMsMax.compare_exchange_weak(prior,ageMs,std::memory_order_relaxed)){}
}
bool m815AsyncEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M815_ASYNC");return on;}
// Caller holds 'mu'. Polling never waits and never touches an in-use command pool.
void m815PollLocked(VkDevice device,const DeviceDispatch& dispatch) {
 if(!dispatch.m812hGetFenceStatus)return;
 for(auto it=m815Pending.begin();it!=m815Pending.end();) {
  if(it->device!=device){++it;continue;}
  const VkResult status=dispatch.m812hGetFenceStatus(device,it->fence);
  if(status!=VK_SUCCESS){++it;continue;}
  auto state=m815SlotStates.find(device);
  if(state!=m815SlotStates.end()){
   auto gen=state->second.find(it->resolution);
   if(gen!=state->second.end())for(int eye=0;eye<2;++eye)
    if(it->slots[eye]>=0)gen->second.busy[eye][it->slots[eye]]=false;
  }
  beamng_ffr::UploadCommands completed=*it;
  m827UploadCaches[device].recycle(dispatch.ffrApi,device,completed,status);
  it=m815Pending.erase(it);
 }
}


struct M85EyeFramebufferPair {
 VkDevice device = VK_NULL_HANDLE;
 // Slots 0/1 use gaze-updated eye maps; slot 2 is the fixed common scene map.
 std::array<VkFramebuffer,3> eyes{};
};
std::unordered_map<VkFramebuffer, M85EyeFramebufferPair> m83EyeFramebuffers;
// M8.23: optional LAZY eye framebuffer variants. Save the original attachment
// handles during the original framebuffer lifetime. M8.34 may retain an idle
// variant while its views/pass remain alive. Non-trivial pNext uses eager fallback.
struct M823LazyFramebuffer {
 VkDevice device=VK_NULL_HANDLE;
 VkRenderPass shadowPass=VK_NULL_HANDLE;
 VkFramebufferCreateFlags flags=0;
 uint32_t width=0,height=0,layers=1;
 std::vector<VkImageView> views; // original attachments, rate view appended on creation
 bool hasAllocator=false;
 VkAllocationCallbacks allocator{};
};
std::unordered_map<VkFramebuffer,M823LazyFramebuffer> m823LazyPlans;
std::mutex m823CreateMutex;
bool m823LazyEnabled(){static const bool on=[](){char b[8]{};
 return beamng_config::read("BEAMNG_VRS_M823_LAZY_FB",b,sizeof(b)) && strcmp(b,"1")==0;}();return on;}
struct M823Stats {
 std::atomic<uint64_t> originals{0}, destroyed{0}, eligible{0}, lazyPlans{0};
 std::atomic<uint64_t> eagerVariants{0}, lazyVariants{0}, lazyFailures{0}, shadowSelections{0}, skipped{0};
};
M823Stats m823Stats;
// Protected by m823CreateMutex, acquired before mu. Idle entries never own
// application views. Rate-image generations remain alive until device teardown.
beamng_framebuffer::IdleCache m834FramebufferCache;
std::atomic<uint64_t> m834FbCreateNs{0},m834FbCreateCalls{0};
void m834Report();


// M8.16: legacy spare framebuffer bookkeeping retained for compatibility with
// the device cleanup path; no spare framebuffers are created anymore.
struct M812FramebufferPool {
 VkDevice device = VK_NULL_HANDLE;
 std::array<std::array<VkFramebuffer,2>,2> spare{}; // [eye][slot - 1]
};
std::unordered_map<VkFramebuffer,M812FramebufferPool> m812FramebufferPools;

// Shadow render passes change render-pass compatibility. Without compatible
// pipeline variants, the experimental binding MUST remain off.
bool m85UnsafeBindEnabled() {
 char v[8]{};
 return beamng_config::read("BEAMNG_VRS_M85_UNSAFE_BIND",v,sizeof(v)) && strcmp(v,"1")==0;
}
std::unordered_set<VkDevice> uploadAttempted;
PFN_vkGetInstanceProcAddr lastGipa = nullptr;
PFN_vkGetDeviceProcAddr lastGdpa = nullptr;
VkInstance lastInstance = VK_NULL_HANDLE;

// M5.9: adjustable foveation. The framebuffer and GPU image remain tied to
// the currently detected 5190x4168 Dream Air path; no universal headset claim.
int m58PatternMode() {
 char v[8] = {};
 if (!beamng_config::read("BEAMNG_VRS_M58_PATTERN", v, sizeof(v))) return 0;
 return strcmp(v, "1") == 0 ? 1 : strcmp(v, "2") == 0 ? 2 : 0;
}
float m59Radius(const char* name, float fallback) {
 char v[64] = {};
 DWORD n = beamng_config::read(name, v, sizeof(v));
 if (!n || n >= sizeof(v)) return fallback;
 char* end = nullptr;
 float value = strtof(v, &end);
 if (end == v || *end != '\0' || !std::isfinite(value) || value < 0.0f || value > 1.0f) return fallback;
 return value;
}
// M8.24: user-friendly per-eye region shape. Default is viewport-relative ellipse.
beamng_ffr::Shape m824Shape() {
 char value[24]{};
 if(!beamng_config::read("BEAMNG_VRS_SHAPE",value,sizeof(value)))
  return beamng_ffr::Shape::Ellipse;
 if(!_stricmp(value,"circle"))return beamng_ffr::Shape::Circle;
 if(!_stricmp(value,"square"))return beamng_ffr::Shape::Square;
 if(!_stricmp(value,"ellipse"))return beamng_ffr::Shape::Ellipse;
 return beamng_ffr::Shape::Ellipse;
}
uint8_t m59Rate(const char* name, uint8_t fallback) {
 char v[16] = {};
 if (!beamng_config::read(name, v, sizeof(v))) return fallback;
 if (!strcmp(v, "1x1")) return beamng_ffr::rate(1,1);
 if (!strcmp(v, "2x2")) return beamng_ffr::rate(2,2);
 if (!strcmp(v, "4x4")) return beamng_ffr::rate(4,4);
 return fallback;
}
beamng_ffr::Pattern m58MakePattern(uint32_t width=0,uint32_t height=0) {
 if(!width)width=m813Width();if(!height)height=m813Height();
 const int mode=m58PatternMode();
 const auto shape=m824Shape();
 // Mode 2 is a deliberately UNIFORM 1x1 diagnostic, not a circle of size 1.
 if(mode==2){
  auto p=beamng_ffr::make(width,height,16,16,1.f,1.f,shape);
  std::fill(p.texels.begin(),p.texels.end(),beamng_ffr::rate(1,1));
  return p;
 }
 const float center=mode==1?.10f:m59Radius("BEAMNG_VRS_CENTER_RADIUS",.30f);
 float middle=mode==1?.18f:m59Radius("BEAMNG_VRS_MID_RADIUS",.65f);
 middle=std::max(center,middle);
 return beamng_ffr::makeGaze(width,height,16,16,.5f,.5f,center,middle,
    m59Rate("BEAMNG_VRS_MID_RATE",beamng_ffr::rate(2,2)),
    m59Rate("BEAMNG_VRS_OUTER_RATE",beamng_ffr::rate(4,4)),shape);
}
bool m8Enabled(){static const bool on=settingFlag("BEAMNG_VRS_M8_EXPERIMENTAL");return on;}
bool m57BindEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M57_BIND_SHADOW");return on;}
bool m55FramebufferEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M55_SHADOW_FRAMEBUFFERS");return on;}
bool m54ShadowEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M54_SHADOW_PASSES");return on;}
// M8.17: logging is opt-in by severity, with one buffered file handle.
// 0 = errors/warnings, 1 = lifecycle + periodic stats (default), 2 = full trace.
int m817LogLevel() {
 static const int level=[]() {
  char v[8]{};
  const DWORD n=beamng_config::read("BEAMNG_VRS_LOG_LEVEL",v,sizeof(v));
  if(n && n<sizeof(v) && strcmp(v,"-1")==0)return -1;
  return n && n<sizeof(v) && v[0]=='0' && !v[1] ? 0 :
         n && n<sizeof(v) && v[0]=='2' && !v[1] ? 2 : 1;
 }();
 return level;
}
bool m817Verbose() { return m817LogLevel()>=2; }
int m817MessageLevel(const char* msg) {
 if(strstr(msg,"FAILED") || strstr(msg,"ERROR") || strstr(msg,"WARNING") ||
    strstr(msg,"FATAL") || strstr(msg,"VkResult=-") || strstr(msg,"result=-")) return 0;
 if(strstr(msg,"M8.38 ") || strstr(msg,"M8.37 ") || strstr(msg,"M8.36 ") || strstr(msg,"M8.35 ") || strstr(msg,"M8.34 ") || strstr(msg,"M8.33 ") || strstr(msg,"M8.17") || strstr(msg,"M8.18") || strstr(msg,"M8.21 ") || strstr(msg,"M8.23 ") || strstr(msg,"M8.24 ") || strstr(msg,"M8.25 ") || strstr(msg,"M8.27 ") || strstr(msg,"M8.28 ") || strstr(msg,"M8.14 AUTO_RESOLUTION") ||
    strstr(msg,"M8.14 RESOURCE_") || strstr(msg,"M8.16 EYE_FB_STATS") ||
    strstr(msg,"M8.16 UNUSED_VRS") || strstr(msg,"M8.15 STATS") ||
    strstr(msg,"M3 VRS enabled") || strstr(msg,"M4.2 GPU:") ||
    strstr(msg,"M5.9 configuration") || strstr(msg,"M5.8 uploaded pattern") ||
    strstr(msg,"M8.12a STAGING_READY") || strstr(msg,"M8.3 two independent")) return 1;
 return 2;
}
void log(const char* message) {
 if(m817LogLevel()<0)return;
 const int severity=m817MessageLevel(message);
 if(severity>m817LogLevel()) return;
 static std::mutex fileMutex;
 struct LogStream {
  FILE* file=nullptr;
  char buffer[64*1024]{};
  ~LogStream(){if(file)fclose(file);}
 };
 static LogStream stream;
 static bool attempted=false;
 static unsigned writes=0;
 std::lock_guard<std::mutex> lock(fileMutex);
 if(!attempted){
  attempted=true;
  char path[MAX_PATH]{};
  const DWORD n=beamng_config::read("TEMP",path,MAX_PATH);
  if(n && n<MAX_PATH && strcat_s(path,"\\BeamNG_VRS_layer.log")==0){
   if(fopen_s(&stream.file,path,"a")==0 && stream.file)
    setvbuf(stream.file,stream.buffer,_IOFBF,sizeof(stream.buffer));
  }
 }
 if(!stream.file)return;
 fprintf(stream.file,"%s\n",message);
 // Crash diagnostics survive; routine traces are buffered rather than flushed
 // after every draw call or framebuffer creation.
 if(severity<=1 || (++writes%512)==0)fflush(stream.file);
}

// M7.3a: shared-memory read-only diagnostics. Never modifies VRS/scissor.
void m73ReadGaze() {
 static std::atomic<unsigned> calls{0};
 if ((calls.fetch_add(1,std::memory_order_relaxed)%180)!=0) return;
 static HANDLE mapping=nullptr;
 static const beamng_gaze::Data* shared=nullptr;
 if (!shared) {
  if (!mapping) mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_gaze::mappingName);
  if (mapping) shared=static_cast<const beamng_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_gaze::Data)));
  if (!shared) { static unsigned misses=0; if((++misses%20)==1)log("M7.3a Vulkan: gaze mapping not yet available"); return; }
  log("M7.3a Vulkan: gaze shared memory connected");
 }
 beamng_gaze::Data copy{};
 bool valid=false;
 for(int retry=0;retry<3;++retry) {
  LONG before=shared->sequence;
  MemoryBarrier();
  if(before&1)continue;
  copy.magic=shared->magic;copy.processId=shared->processId;
  copy.tickMs=shared->tickMs;copy.predictedTime=shared->predictedTime;
  copy.qx=shared->qx;copy.qy=shared->qy;copy.qz=shared->qz;copy.qw=shared->qw;
  copy.px=shared->px;copy.py=shared->py;copy.pz=shared->pz;copy.flags=shared->flags;
  for(int i=0;i<2;++i){copy.eyeU[i]=shared->eyeU[i];copy.eyeV[i]=shared->eyeV[i];}
  copy.eyeValidMask=shared->eyeValidMask;
  MemoryBarrier();
  LONG after=shared->sequence;
  if(before==after && !(after&1) && copy.magic==beamng_gaze::magicValue && copy.processId==GetCurrentProcessId()){valid=true;break;}
 }
 if(!valid){log("M7.3a Vulkan: gaze sample unavailable/invalid");return;}
 const auto age=GetTickCount64()-copy.tickMs;
 char msg[320]{};
 sprintf_s(msg,"M7.3a Vulkan: gaze age=%llums flags=0x%X quat=(%.4f,%.4f,%.4f,%.4f) predicted=%lld",
  (unsigned long long)age,copy.flags,copy.qx,copy.qy,copy.qz,copy.qw,(long long)copy.predictedTime);
 log(msg);
 if(copy.eyeValidMask){char p[240]{};sprintf_s(p,"M7.3b Vulkan: mode=diagnostic gazeUV mask=%u L=(%.3f,%.3f) R=(%.3f,%.3f) age=%llums",copy.eyeValidMask,copy.eyeU[0],copy.eyeV[0],copy.eyeU[1],copy.eyeV[1],(unsigned long long)age);log(p);}
}

// M7.4b: render-pass / gaze correlation. No Vulkan resource mutations.
// A 5190x4168 target is potentially a PER-EYE target, not a packed stereo image.
// Log framebuffer/renderpass identities alongside both OpenXR projected gaze points.
bool m817Verbose();
void m74LogEyePass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info) {
  if (!m817Verbose()) return;
  char mode[16]{};
  if (!beamng_config::read("BEAMNG_VRS_FOVEATION_MODE",mode,sizeof(mode)) || strcmp(mode,"eye")) return;
  if (!info) return;
  VkDevice device=VK_NULL_HANDLE;
  FramebufferInfo fb{};
  bool known=false;
  {
    std::lock_guard<std::mutex> lock(mu);
    auto ci=commandDevices.find(cmd);
    if(ci!=commandDevices.end())device=ci->second;
    auto fi=framebuffers.find(info->framebuffer);
    if(fi!=framebuffers.end()){fb=fi->second;known=true;}
  }
  if (!known || fb.width!=m813Width() || fb.height!=m813Height()) return;
  // Log distinct framebuffer-to-attachment identities once, bounded to 80 entries.
  // Same image view in two framebuffers means shared render target, NOT eye identity.
  {
    std::vector<VkImageView> views;
    bool report = false;
    {
      std::lock_guard<std::mutex> lock(mu);
      auto it = m74dAttachments.find(info->framebuffer);
      if (it != m74dAttachments.end() && m74dReported.size() < 80 &&
          m74dReported.insert(info->framebuffer).second) {
        views = it->second.views;
        report = true;
      }
    }
    if (report) {
      char msg[512]{};
      sprintf_s(msg, "M7.4d FB identity cmd=%p fb=%p pass=%p count=%u view0=%p view1=%p view2=%p view3=%p",
        (void*)cmd, (void*)info->framebuffer, (void*)info->renderPass,
        (unsigned)views.size(),
        views.size()>0?(void*)views[0]:nullptr,
        views.size()>1?(void*)views[1]:nullptr,
        views.size()>2?(void*)views[2]:nullptr,
        views.size()>3?(void*)views[3]:nullptr);
      log(msg);
      // Resolve framebuffer attachments to source VkImages using recorded image views.
      for (size_t index=0; index<views.size() && index<16; ++index) {
        VkImage img = VK_NULL_HANDLE;
        bool found = false;
        {
          std::lock_guard<std::mutex> lock(mu);
          auto it = m74gViews.find(views[index]);
          if (it != m74gViews.end()) { img = it->second.image; found = true; }
        }
        char link[256]{};
        sprintf_s(link, "M7.4g FB link fb=%p attachment=%u view=%p VkImage=%p mapped=%u",
          (void*)info->framebuffer, (unsigned)index, (void*)views[index],
          (void*)img, found ? 1u : 0u);
        log(link);
      }
    }
  }

  static HANDLE mapping=nullptr;
  static const beamng_gaze::Data* shared=nullptr;
  if(!shared){
    if(!mapping)mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_gaze::mappingName);
    if(mapping)shared=static_cast<const beamng_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_gaze::Data)));
  }
  beamng_gaze::Data sample{};
  bool valid=false;
  if(shared)for(int retry=0;retry<3;++retry){
    LONG a=shared->sequence;MemoryBarrier();if(a&1)continue;
    sample.magic=shared->magic;sample.processId=shared->processId;sample.tickMs=shared->tickMs;
    sample.eyeValidMask=shared->eyeValidMask;
    for(int i=0;i<2;++i){sample.eyeU[i]=shared->eyeU[i];sample.eyeV[i]=shared->eyeV[i];}
    MemoryBarrier();LONG b=shared->sequence;
    if(a==b && !(b&1) && sample.magic==beamng_gaze::magicValue && sample.processId==GetCurrentProcessId()) {valid=true;break;}
  }
  uint64_t age=valid && GetTickCount64()>=sample.tickMs ? GetTickCount64()-sample.tickMs : UINT64_MAX;
  bool tracked=valid && age<=100 && (sample.eyeValidMask&3)==3;
  static std::atomic<unsigned> calls{0};
  if(calls.fetch_add(1,std::memory_order_relaxed)%120) return;
  char b[512]{};
  sprintf_s(b,"M7.4b eye-pass mode=diagnostic device=%p cmd=%p fb=%p pass=%p dims=%ux%u tracked=%u age=%llums mask=%u L=(%.3f,%.3f) R=(%.3f,%.3f)",
    (void*)device,(void*)cmd,(void*)info->framebuffer,(void*)info->renderPass,fb.width,fb.height,
    tracked?1u:0u,(unsigned long long)age,valid?sample.eyeValidMask:0,
    sample.eyeU[0],sample.eyeV[0],sample.eyeU[1],sample.eyeV[1]);
  log(b);
}

// M7.4c: safe eye-mode CPU candidate, not yet written into the live GPU image.
// The 5190x4168 attachments are full-sized; do not assume packed stereo.
// An explicit eye selection is required until framebuffer-to-eye ownership is known.
int m74EyeSelection() {
 char v[16]{};
 if (!beamng_config::read("BEAMNG_VRS_EYE_INDEX",v,sizeof(v))) return -1;
 return strcmp(v,"0")==0 ? 0 : strcmp(v,"1")==0 ? 1 : -1;
}
bool m74SampleEye(float& u,float& v,uint64_t& age) {
 static HANDLE mapping=nullptr;
 static const beamng_projected_gaze::Data* shared=nullptr;
 if (!shared) {
  if (!mapping) mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_projected_gaze::mappingName);
  if (mapping) shared=static_cast<const beamng_projected_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_projected_gaze::Data)));
 }
 if (!shared) return false;
 const int eye=m74EyeSelection();
 if (eye<0) return false;
 for(int retry=0;retry<3;++retry) {
  LONG a=shared->sequence;MemoryBarrier();if(a&1)continue;
  uint32_t magic=shared->magic, pid=shared->processId, mask=shared->eyeValidMask;
  uint64_t tick=shared->projectionTickMs,poseTick=shared->poseTickMs;
  float x=shared->eyeU[eye], y=shared->eyeV[eye];
  MemoryBarrier();LONG b=shared->sequence;
  if(a!=b || (b&1) || magic!=beamng_projected_gaze::magicValue || pid!=GetCurrentProcessId())continue;
  const uint64_t now=GetTickCount64();
  if(!beamng_gaze_freshness::validProjection(now,poseTick,tick,mask,eye,x,y))return false;
  u=x;v=y;age=now-tick;return true;
 }
 return false;
}
beamng_ffr::Pattern m74MakeEyeCandidate(float u,float v) {
 const int mode=m58PatternMode();
 const float center=mode==1?.10f:mode==2?1.f:m59Radius("BEAMNG_VRS_CENTER_RADIUS",.30f);
 float middle=mode==1?.18f:mode==2?1.f:m59Radius("BEAMNG_VRS_MID_RADIUS",.65f);
 middle=std::max(center,middle);
 auto p=beamng_ffr::makeGaze(m813Width(),m813Height(),16,16,u,v,center,middle,
   m59Rate("BEAMNG_VRS_MID_RATE",beamng_ffr::rate(2,2)),
   m59Rate("BEAMNG_VRS_OUTER_RATE",beamng_ffr::rate(4,4)),m824Shape());
 if(mode==2)std::fill(p.texels.begin(),p.texels.end(),beamng_ffr::rate(1,1));
 return p;
}
void m74LogEyeCandidate() {
 if(!m817Verbose())return;
 char mode[16]{};
 if (!beamng_config::read("BEAMNG_VRS_FOVEATION_MODE",mode,sizeof(mode)) || strcmp(mode,"eye")) return;
 static std::atomic<unsigned> n{0};
 if(n.fetch_add(1,std::memory_order_relaxed)%240) return;
 float u=0,v=0;uint64_t age=0;
 if(!m74SampleEye(u,v,age)) {
  log("M7.4c CPU candidate: fixed fallback (missing/stale/offscreen gaze or BEAMNG_VRS_EYE_INDEX unset)");
  return;
 }
 auto pattern=m74MakeEyeCandidate(u,v);
 size_t fine=std::count(pattern.texels.begin(),pattern.texels.end(),beamng_ffr::rate(1,1));
 char line[256]{};
 sprintf_s(line,"M7.4c CPU candidate: eye=%d UV=(%.3f,%.3f) age=%llums fineTiles=%llu GPU=UNCHANGED",
  m74EyeSelection(),u,v,(unsigned long long)age,(unsigned long long)fine);
 log(line);
}

bool diagnosticEnabled();
void logM44Pattern() {
 if (!diagnosticEnabled()) return;
 auto pattern = m58MakePattern();
 unsigned counts[3] = {};
 for (uint8_t t : pattern.texels) ++counts[t == 0 ? 0 : t == 5 ? 1 : 2];
 char msg[256];
 sprintf_s(msg, "M4.4 CPU FFR texture: %ux%u R8_UINT, 1x1=%u 2x2=%u 4x4=%u; GPU attachment NOT ACTIVE",
   pattern.width, pattern.height, counts[0], counts[1], counts[2]);
 log(msg);
}
bool diagnosticEnabled() {
 char value[8] = {};
 return beamng_config::read("BEAMNG_VRS_M4_DIAGNOSTICS", value, sizeof(value)) && strcmp(value, "1") == 0;
}
void diagnosticLog(const char* message) {
 if (!diagnosticEnabled()) return;
 static std::atomic<unsigned> lines{0};
 if (lines.fetch_add(1, std::memory_order_relaxed) < 1200) log(message);
}
void logResult(const char* label, VkResult r) {
  char b[160]; sprintf_s(b, "%s: VkResult=%d", label, static_cast<int>(r)); log(b);
}
bool uniformVrsEnabled() {
  char value[8] = {};
  return beamng_config::read("BEAMNG_VRS_UNIFORM_4X4", value, sizeof(value)) && strcmp(value, "1") == 0;
}
bool enabledByEnv() {
  char value[8] = {};
  return beamng_config::read("BEAMNG_VRS_ENABLE_FEATURES", value, sizeof(value)) && strcmp(value, "1") == 0;
}

const VkBaseInStructure* findChain(const void* p, VkStructureType type) {
  auto* node = static_cast<const VkBaseInStructure*>(p);
  for (unsigned i = 0; node && i < 128; ++i, node = node->pNext)
    if (node->sType == type) return node;
  return nullptr;
}
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
 if(m835Enabled())m835Profiler.destroyDevice(device);
 std::lock_guard<std::mutex> lazyGuard(m823CreateMutex);
  PFN_vkDestroyDevice next = nullptr;
  PFN_vkDestroyFence m812hDestroy = nullptr;
  PFN_vkDeviceWaitIdle m812hWaitIdle = nullptr;
  std::vector<VkFence> m812hOrphans;
  std::vector<M815Upload> m815Orphans;
  beamng_ffr::Api api{};
  beamng_ffr::GpuImage image{};
  std::array<beamng_ffr::GpuImage,2> eyeImages{};
  M812Staging staging{};
  M812ImagePool imagePool{};
  std::map<uint64_t,M814ResourceSet> m814Sets{};
  PFN_vkDestroyRenderPass destroyPass = nullptr;
  std::vector<VkRenderPass> orphanShadows;
  std::vector<VkFramebuffer> orphanFramebuffers;
  for(auto id:m834FramebufferCache.invalidate([&](const auto& k){return k.device==m833Id(device);}))
   orphanFramebuffers.push_back(reinterpret_cast<VkFramebuffer>(uintptr_t(id)));
  PFN_vkDestroyFramebuffer destroyFb = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu);
    auto it = devices.find(device);
    auto cache=m827UploadCaches.find(device);
    if(cache!=m827UploadCaches.end()){
     if(it!=devices.end())cache->second.destroyIdle(it->second.ffrApi,device);
     m827UploadCaches.erase(cache);
    }
    if(it!=devices.end())for(auto v=m838Variants.begin();v!=m838Variants.end();){
      if(v->second.device==device){if(it->second.destroyPipeline)it->second.destroyPipeline(device,v->second.pipeline,nullptr);v=m838Variants.erase(v);}else ++v;
    }
    m838Clocks.erase(device);
    for(auto p=m838PassDevices.begin();p!=m838PassDevices.end();)if(p->second==device){m838UnormPasses.erase(p->first);m838DepthSlots.erase(p->first);p=m838PassDevices.erase(p);}else ++p;
    if (it != devices.end()) { m812hDestroy=it->second.m812hDestroyFence; m812hWaitIdle=it->second.ffrApi.deviceWaitIdle; next = it->second.destroyDevice; destroyPass = it->second.destroyRenderPass; destroyFb = it->second.destroyFramebuffer; api = it->second.ffrApi; devices.erase(it); }
    for(auto it=m815Pending.begin();it!=m815Pending.end();) {
      if(it->device==device){m815Orphans.push_back(*it);it=m815Pending.erase(it);}
      else ++it;
    }
    m815SlotStates.erase(device);
    m827GraphicsFamilies.erase(device);
    m833Trackers.erase(device);
    auto shared=m837Trackers.find(device);
    if(shared!=m837Trackers.end()){
     for(const auto& [id,image]:shared->second.images){auto handle=reinterpret_cast<VkImage>(uintptr_t(id));m837SceneImages.erase(handle);m838UnormImages.erase(handle);}
     for(auto i=m838DepthImages.begin();i!=m838DepthImages.end();)if(i->second.device==device)i=m838DepthImages.erase(i);else ++i;
     m837Trackers.erase(shared);
    }
    for(const auto& [view,info]:m74gViews)if(info.device==device){m833ColorViews.erase(view);m838DepthViews.erase(view);}
    m812liveUploaded.erase(device);
    for(auto it=m823LazyPlans.begin();it!=m823LazyPlans.end();){
     if(it->second.device==device)it=m823LazyPlans.erase(it);else ++it;
    }
    auto rc=m814ResourceSets.find(device);
    if(rc!=m814ResourceSets.end()){m814Sets=std::move(rc->second);m814ResourceSets.erase(rc);}
    auto uploaded = uploadedPatterns.find(device);
    if (uploaded != uploadedPatterns.end()) { image = uploaded->second; uploadedPatterns.erase(uploaded); }
    auto pool=m812ImagePools.find(device);
    if(pool!=m812ImagePools.end()){imagePool=pool->second;m812ImagePools.erase(pool);}
    auto st=m812Staging.find(device);
    if(st!=m812Staging.end()){staging=st->second;m812Staging.erase(st);}
    auto ep=m83EyePatterns.find(device);
    if(ep!=m83EyePatterns.end()){eyeImages=ep->second;m83EyePatterns.erase(ep);}
    for(auto it=m812FramebufferPools.begin();it!=m812FramebufferPools.end();) {
      if(it->second.device!=device){++it;continue;}
      for(auto& eye:it->second.spare)for(auto f:eye)if(f)orphanFramebuffers.push_back(f);
      it=m812FramebufferPools.erase(it);
    }
    for(auto it=m83EyeFramebuffers.begin();it!=m83EyeFramebuffers.end();) {
      if(it->second.device!=device){++it;continue;}
      for(auto f:it->second.eyes)if(f)orphanFramebuffers.push_back(f);
      it=m83EyeFramebuffers.erase(it);
    }
    for (auto it3=shadowPasses.begin(); it3!=shadowPasses.end();) {
      if (it3->second.device==device) { orphanShadows.push_back(it3->second.shadow); m832ShadowSignatures.erase(it3->second.shadow); m833ColorSlots.erase(it3->first);m837ScenePasses.erase(it3->first);m838UnormPasses.erase(it3->first);m838DepthSlots.erase(it3->first); it3=shadowPasses.erase(it3); }
      else ++it3;
    }
    for (auto it4=shadowFramebuffers.begin(); it4!=shadowFramebuffers.end();) {
      if (it4->second.device==device) { if(it4->second.shadow)orphanFramebuffers.push_back(it4->second.shadow); it4=shadowFramebuffers.erase(it4); }
      else ++it4;
    }
    for(auto it=m89Variants.begin();it!=m89Variants.end();) {
      if(it->second.device==device) {
        // vkDestroyDevice implicitly destroys pipelines; avoid calls after dispatch removal.
        it=m89Variants.erase(it);
      } else ++it;
    }
    for(auto it=m830CropVariants.begin();it!=m830CropVariants.end();){
      if(it->second.device==device)it=m830CropVariants.erase(it);else ++it;
    }
    for(auto it=m87Pipelines.begin();it!=m87Pipelines.end();) {
      if(it->second.device==device)it=m87Pipelines.erase(it);else ++it;
    }
    for(auto it=m812hPending.begin();it!=m812hPending.end();) {
      if(it->device==device){m812hOrphans.push_back(it->fence);it=m812hPending.erase(it);}else ++it;
    }
    for(auto it=m812gQueueDevices.begin();it!=m812gQueueDevices.end();) {
      if(it->second==device)it=m812gQueueDevices.erase(it);else ++it;
    }
    uploadAttempted.erase(device);
    capturedQueues.erase(device);
    for (auto it2=commandDevices.begin(); it2!=commandDevices.end();) {
      if (it2->second==device) {m838Scopes.erase(it2->first);m838OriginalBindings.erase(it2->first);m838BoundCopies.erase(it2->first);m833ActiveEarlier.erase(it2->first);m837ActiveShared.erase(it2->first);m87ActivePasses.erase(it2->first);m87PassBindCounts.erase(it2->first);it2=commandDevices.erase(it2);} else ++it2;
    }
  }
  // Every shadow framebuffer is destroyed before its associated generation.
  if(m812hWaitIdle && !m814Sets.empty())m812hWaitIdle(device);
  // The application's destruction contract already requires all submitted work to finish.
  // Wait explicitly before destroying our own tracking fences and resources.
  if(m812hWaitIdle && (!m812hOrphans.empty() || !m815Orphans.empty()))m812hWaitIdle(device);
  if(api.complete())for(const auto& pending:m815Orphans){
   if(pending.fence)api.destroyFence(device,pending.fence,nullptr);
   if(pending.pool)api.destroyCommandPool(device,pending.pool,nullptr);
  }
  for(VkFence f:m812hOrphans)if(m812hDestroy)m812hDestroy(device,f,nullptr);
  for (auto fb : orphanFramebuffers) if (destroyFb) destroyFb(device,fb,nullptr);
  for (auto pass : orphanShadows) if (destroyPass) destroyPass(device,pass,nullptr);
  if(api.complete() && !m814Sets.empty()){
   for(auto& set:m814Sets){
    auto& res=set.second;
    if(res.base.image)beamng_ffr::destroy(api,device,res.base);
    for(auto& img:res.eyes)if(img.image)beamng_ffr::destroy(api,device,img);
    for(auto& eye:res.imagePool.extra)for(auto& img:eye)if(img.image)beamng_ffr::destroy(api,device,img);
    for(auto& eye:res.staging.slots)for(auto& slot:eye)beamng_ffr::destroyStaging(api,device,slot);
   }
  }else{
   if (image.image && api.complete())beamng_ffr::destroy(api,device,image);
   for(auto& eyeImage:eyeImages)if(eyeImage.image && api.complete())beamng_ffr::destroy(api,device,eyeImage);
   if(api.complete())for(auto& eye:imagePool.extra)for(auto& img:eye)if(img.image)beamng_ffr::destroy(api,device,img);
   if(api.complete())for(auto& eye:staging.slots)for(auto& slot:eye)beamng_ffr::destroyStaging(api,device,slot);
  }
  if (next) next(device, alloc);
}

// M8.11: controlled A/B comparison. BASELINE=1 disables only the shadow
// render-pass substitution; all other layer features and gaze preview remain.
static bool m811BaselineEnabled() {
 static const bool on=settingFlag("BEAMNG_VRS_M811_BASELINE");return on;
}
static void m811ReportMode() {
 static std::once_flag once;
 std::call_once(once, [] {
   log(m811BaselineEnabled()
     ? "M8.11 MODE=BASELINE shadow VRS binding disabled; gaze preview still active"
     : "M8.11 MODE=STATIC_TEST shadow VRS binding permitted when M89_STATIC_TEST=1");
 });
}
static bool m89StaticEnabled() {
 static const bool on=settingFlag("BEAMNG_VRS_M89_STATIC_TEST");return on;
}
// M8.12h: optional layer-owned completion markers, submitted AFTER the app's work
// on the same queue. No VRS image copies, rotation, or slot retirement yet.
namespace {
bool m812hEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M812H_FENCE_TEST");return on;}
void m812hLog(const char* action, uint64_t serial, VkResult result, size_t pending){
 static std::atomic<unsigned> n{0};if(n.fetch_add(1)>=100)return;
 char b[240]{};sprintf_s(b,"M8.12h FENCE action=%s serial=%llu result=%d pending=%llu GPU_UPDATE=DISABLED",action,
  static_cast<unsigned long long>(serial),int(result),static_cast<unsigned long long>(pending));log(b);
}
void m812hPoll(VkDevice device, PFN_vkGetFenceStatus status, PFN_vkDestroyFence destroy){
 if(!status || !destroy)return;
 std::vector<M812hMarker> snapshot;
 {std::lock_guard<std::mutex> lock(mu);for(const auto& m:m812hPending)if(m.device==device)snapshot.push_back(m);}
 for(const auto& m:snapshot){
  VkResult r=status(device,m.fence);
  if(r!=VK_SUCCESS)continue;
  bool owned=false;size_t remaining=0;
  {std::lock_guard<std::mutex> lock(mu);
   auto it=std::find_if(m812hPending.begin(),m812hPending.end(),[&](const M812hMarker& x){return x.fence==m.fence && x.device==device;});
   if(it!=m812hPending.end()){m812hPending.erase(it);owned=true;}remaining=m812hPending.size();}
  if(owned){destroy(device,m.fence,nullptr);m812hLog("RETIRED",m.serial,r,remaining);}
 }
}
void m812hAfterSubmit(VkQueue q,VkResult appResult){
 if(appResult!=VK_SUCCESS || !m812hEnabled())return;
 VkDevice device=VK_NULL_HANDLE;PFN_vkQueueSubmit submit=nullptr;
 PFN_vkCreateFence create=nullptr;PFN_vkGetFenceStatus status=nullptr;PFN_vkDestroyFence destroy=nullptr;
 {std::lock_guard<std::mutex> lock(mu);auto qi=m812gQueueDevices.find(q);
  if(qi==m812gQueueDevices.end())return;device=qi->second;auto di=devices.find(device);if(di==devices.end())return;
  submit=di->second.m812gQueueSubmit;create=di->second.m812hCreateFence;
  status=di->second.m812hGetFenceStatus;destroy=di->second.m812hDestroyFence;}
 if(!submit||!create||!status||!destroy)return;
 m812hPoll(device,status,destroy);
 {std::lock_guard<std::mutex> lock(mu);if(m812hPending.size()>=128)return;}
 VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence=VK_NULL_HANDLE;
 VkResult r=create(device,&ci,nullptr,&fence);if(r!=VK_SUCCESS)return;
 // Zero-submit fence signals after earlier queue work, without changing app submissions.
 r=submit(q,0,nullptr,fence);
 if(r!=VK_SUCCESS){destroy(device,fence,nullptr);m812hLog("SUBMIT_FAILED",0,r,0);return;}
 uint64_t serial=0;size_t pending=0;
 {std::lock_guard<std::mutex> lock(mu);serial=++m812hNextSerial;m812hPending.push_back({device,q,fence,serial});pending=m812hPending.size();}
 m812hLog("SUBMITTED",serial,r,pending);
}
void m812gReport(const char* api, VkQueue queue, uint32_t count, uint64_t commands, VkFence fence) {
 static std::atomic<unsigned> reports{0};
 if(reports.fetch_add(1,std::memory_order_relaxed)>=48)return;
 char msg[240]{};
 sprintf_s(msg,"M8.12g SUBMIT api=%s queueHash=%llu batches=%u commandBuffers=%llu fencePresent=%u GPU_UPDATE=DISABLED",
  api,static_cast<unsigned long long>(std::hash<VkQueue>{}(queue)),count,
  static_cast<unsigned long long>(commands),fence!=VK_NULL_HANDLE?1u:0u);
 log(msg);
}
}
// M8.15: opt-in queue-ordered upload, preserving the proven synchronous fallback.
// On a single captured graphics queue, Vulkan stage barriers order earlier VRS
// attachment reads before this transfer, then the app submission follows it.
// Submission fences own transfer command pools and staging slots until complete.
// Multiple graphics queues use the conservative M8.14a device-idle fallback.
namespace {
bool m812liveEnabled(){static const bool on=settingFlag("BEAMNG_VRS_M812_LIVE_GAZE");return on;}
void m812liveBeforeSubmit(VkQueue queue){
 auto reportExit=[](const char* reason){
  static std::mutex logMu;
  static std::unordered_map<std::string,unsigned> counts;
  std::lock_guard<std::mutex> lock(logMu);
  auto& count=counts[reason];
  if(count++<5){char msg[240]{};sprintf_s(msg,"M8.15 EXIT reason=%s occurrence=%u",reason,count);log(msg);}
 };
 if(!m812liveEnabled())return;
 // Caller holds m814ResourceMutex. This serializes our upload and the
 // immediately following application submit on the same VkQueue.
 std::lock_guard<std::mutex> queueGuard(m812liveQueueMutex);
 std::lock_guard<std::mutex> guard(mu);
 auto qi=m812gQueueDevices.find(queue);if(qi==m812gQueueDevices.end()){reportExit("QUEUE_UNKNOWN");return;}
 const VkDevice device=qi->second;
 auto di=devices.find(device);
 auto si=m812Staging.find(device);
 auto ii=m83EyePatterns.find(device);
 if(di==devices.end()||si==m812Staging.end()||ii==m83EyePatterns.end()){reportExit("DEVICE_STAGING_OR_IMAGES_MISSING");return;}
 const DeviceDispatch& dispatch=di->second;
 auto& api=di->second.ffrApi;
 if(!api.complete())return;
 auto queues=capturedQueues.find(device);
 if(queues==capturedQueues.end())return;
 uint32_t family=UINT32_MAX;
 for(const auto& q:queues->second)if(q.queue==queue){family=q.family;break;}
 if(family==UINT32_MAX)return; // Only graphics queues can read VRS attachments.
 const uint64_t resolution=m814Key(m813Width(),m813Height());
 auto& slotState=m815SlotStates[device][resolution];
 auto& uploaded=m812liveUploaded[device];
 // Retire completed upload command pools and unpin their staging slots.
 m815PollLocked(device,dispatch);
 std::array<int,2> uploadSlots{{-1,-1}};
 std::array<uint64_t,2> next=uploaded;
 for(int eye=0;eye<2;++eye){
  if(slotState.latestSeq[eye]<=uploaded[eye])continue;
  const int idx=slotState.latestSlot[eye];
  if(idx<0 || idx>=3 || slotState.busy[eye][idx])continue;
  auto& staging=si->second.slots[eye][idx];
  if(!staging.buffer || !staging.mapped ||
     staging.capacity<VkDeviceSize(ii->second[eye].width)*ii->second[eye].height)continue;
  uploadSlots[eye]=idx;
  next[eye]=slotState.latestSeq[eye];
 }
 if(uploadSlots[0]<0 && uploadSlots[1]<0)return;
 // This is CPU sample-to-queue-submit age, NOT photon/display latency.
 const uint64_t m821SampleTime=GetTickCount64();
 LARGE_INTEGER uploadStart{}, uploadStop{}, uploadFrequency{};
 QueryPerformanceCounter(&uploadStart);
 // Async upload is safe only when ALL eye VRS reads and this upload are
 // submitted to this one captured graphics queue. A same-queue Vulkan image
 // barrier orders old shading-rate reads -> transfer -> later reads.
 // If another graphics queue is captured, keep M8.14a's device-idle path.
 const bool async=m815AsyncEnabled() && queues->second.size()==1 &&
                  dispatch.m812hGetFenceStatus!=nullptr && m815Pending.size()<64;
 if(!async){
  const VkResult idle=api.deviceWaitIdle(device);
  if(idle!=VK_SUCCESS){reportExit("DEVICE_WAIT_IDLE_FAILED");return;}
  m815PollLocked(device,dispatch);
 }
 VkCommandPool pool=VK_NULL_HANDLE;
 VkFence fence=VK_NULL_HANDLE;
 VkCommandBuffer cmd=VK_NULL_HANDLE;
 bool submitted=false;
 try{
  std::array<beamng_ffr::StagingSlot,2> sources{};
  for(int eye=0;eye<2;++eye)if(uploadSlots[eye]>=0)
   sources[eye]=si->second.slots[eye][uploadSlots[eye]];
  const auto key=beamng_ffr::gazeTransferKey(sources,ii->second);
  auto commands=m827UploadCaches[device].acquire(api,device,family,&key);
  pool=commands.pool;fence=commands.fence;cmd=commands.command;
  beamng_ffr::recordGazeCommands(api,commands,sources,ii->second);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
  beamng_ffr::check(api.queueSubmit(queue,1,&submit,fence),"M8.15 submit");
  submitted=true;
  if(async){
   // The fence will release this pool and both pinned staging slots, WITHOUT
   // a host-side wait. The application's submit follows on the same queue.
   m815Pending.push_back({commands,device,resolution,uploadSlots});
   for(int eye=0;eye<2;++eye)if(uploadSlots[eye]>=0)
    slotState.busy[eye][uploadSlots[eye]]=true;
   fence=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;
  }else{
   beamng_ffr::check(api.waitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"M8.15 wait fence");
   auto completed=commands;
   m827UploadCaches[device].recycle(api,device,completed,VK_SUCCESS);
   fence=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;
  }
  uploaded=next;
  for(int eye=0;eye<2;++eye)if(uploadSlots[eye]>=0){
   const uint64_t source=slotState.sourceTickMs[eye][uploadSlots[eye]];
   if(source && m821SampleTime>=source)
    m821RecordUploadAge(beamng_gaze_timing::ageMs(m821SampleTime,source));
  }
  QueryPerformanceCounter(&uploadStop);
  QueryPerformanceFrequency(&uploadFrequency);
  const unsigned long long cpuUs=uploadFrequency.QuadPart>0 ?
   static_cast<unsigned long long>((uploadStop.QuadPart-uploadStart.QuadPart)*1000000LL/uploadFrequency.QuadPart):0ULL;
  // M8.17: collect real CPU upload costs without writing every sample to disk.
  static std::atomic<unsigned long long> statsCount{0},statsCpuSum{0},statsCpuMax{0},statsAsync{0};
  statsCpuSum.fetch_add(cpuUs,std::memory_order_relaxed);
  if(async)statsAsync.fetch_add(1,std::memory_order_relaxed);
  unsigned long long priorMax=statsCpuMax.load(std::memory_order_relaxed);
  while(priorMax<cpuUs && !statsCpuMax.compare_exchange_weak(priorMax,cpuUs,std::memory_order_relaxed)){}
  const auto sampleCount=statsCount.fetch_add(1,std::memory_order_relaxed)+1;
  if(sampleCount%256==0){
   char report[256]{};
   sprintf_s(report,"M8.17 STATS uploads=%llu avgCpuUs=%llu maxCpuUs=%llu asyncUploads=%llu resolution=%ux%u",
    sampleCount,statsCpuSum.load(std::memory_order_relaxed)/sampleCount,
    statsCpuMax.load(std::memory_order_relaxed),statsAsync.load(std::memory_order_relaxed),
    m813Width(),m813Height());
   log(report);
   const uint64_t ages=m821GazeStats.uploadSamples.load(std::memory_order_relaxed);
   const uint64_t staged=m821GazeStats.staged.load(std::memory_order_relaxed);
   char ageReport[300]{};
   sprintf_s(ageReport,"M8.21 GAZE_STATS intervalMs=%u staged=%llu unchanged=%llu stale=%llu noFreeSlot=%llu sourceToSubmitAvgMs=%llu sourceToSubmitMaxMs=%llu ageSamples=%llu",
    m821UpdateIntervalMs(),(unsigned long long)staged,
    (unsigned long long)m821GazeStats.unchanged.load(std::memory_order_relaxed),
    (unsigned long long)m821GazeStats.stale.load(std::memory_order_relaxed),
    (unsigned long long)m821GazeStats.noFreeSlot.load(std::memory_order_relaxed),
    (unsigned long long)(ages ? m821GazeStats.uploadAgeMsSum.load(std::memory_order_relaxed)/ages : 0),
    (unsigned long long)m821GazeStats.uploadAgeMsMax.load(std::memory_order_relaxed),
    (unsigned long long)ages);
   log(ageReport);
   const auto& cache=m827UploadCaches[device];
   char cacheReport[220]{};
   sprintf_s(cacheReport,"M8.28 UPLOAD_CACHE created=%llu reused=%llu recordingsReused=%llu idle=%zu",
    (unsigned long long)cache.created(),(unsigned long long)cache.reused(),
    (unsigned long long)cache.recordingReused(),cache.idleCount());
   log(cacheReport);
   char patternReport[160]{};
   sprintf_s(patternReport,"M8.27 PATTERN_STATS identicalSkipped=%llu",
    (unsigned long long)m821GazeStats.identicalPattern.load(std::memory_order_relaxed));
   log(patternReport);
  }
  static std::atomic<unsigned> reports{0};
  if(m817Verbose() && reports.fetch_add(1,std::memory_order_relaxed)<64){char msg[256]{};
   sprintf_s(msg,"M8.15 GPU_GAZE_UPLOADED L=%llu R=%llu SYNC=%s pending=%zu cpuUs=%llu",
    (unsigned long long)uploaded[0],(unsigned long long)uploaded[1],
    async?"QUEUE_ORDERED_FENCE":"DEVICE_IDLE_FALLBACK",m815Pending.size(),cpuUs);log(msg);}
 }catch(const std::exception& ex){char msg[320]{};
  sprintf_s(msg,"M8.15 UPLOAD_FAILED: %s",ex.what());log(msg);
  // If a queue submission succeeded, wait before destroying its resources.
  // This is an exceptional path; the normal async path NEVER waits for GPU idle.
  if(submitted)api.deviceWaitIdle(device);
 }
 if(fence)api.destroyFence(device,fence,nullptr);
 if(pool)api.destroyCommandPool(device,pool,nullptr);
}
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
 PFN_vkQueueSubmit next=nullptr;
 {std::lock_guard<std::mutex> guard(mu);auto qi=m812gQueueDevices.find(q);if(qi!=m812gQueueDevices.end()){
  auto di=devices.find(qi->second);if(di!=devices.end())next=di->second.m812gQueueSubmit;}}
 if(!next)return VK_ERROR_INITIALIZATION_FAILED;
 uint64_t cmds=0;for(uint32_t i=0;infos&&i<count;++i)cmds+=infos[i].commandBufferCount;
 m812gReport("vkQueueSubmit",q,count,cmds,fence);
 std::lock_guard<std::mutex> queueSerial(m814ResourceMutex);
 m812liveBeforeSubmit(q);
 VkResult r=next(q,count,infos,fence);
 if(m835Enabled() && r==VK_SUCCESS)for(uint32_t i=0;infos && i<count;++i)
  for(uint32_t j=0;j<infos[i].commandBufferCount;++j)m835Profiler.submitted(infos[i].pCommandBuffers[j]);
 if(m837Enabled() && r==VK_SUCCESS){std::vector<uint64_t> ids;
  for(uint32_t i=0;infos && i<count;++i){for(uint32_t j=0;j<infos[i].commandBufferCount;++j)ids.push_back(m833Id(infos[i].pCommandBuffers[j]));}
  m837Submitted(q,ids);
 }
 m812hAfterSubmit(q,r); return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence) {
 PFN_vkQueueSubmit2 next=nullptr;
 {std::lock_guard<std::mutex> guard(mu);auto qi=m812gQueueDevices.find(q);if(qi!=m812gQueueDevices.end()){
  auto di=devices.find(qi->second);if(di!=devices.end())next=di->second.m812gQueueSubmit2;}}
 if(!next)return VK_ERROR_INITIALIZATION_FAILED;
 uint64_t cmds=0;for(uint32_t i=0;infos&&i<count;++i)cmds+=infos[i].commandBufferInfoCount;
 m812gReport("vkQueueSubmit2",q,count,cmds,fence);
 std::lock_guard<std::mutex> queueSerial(m814ResourceMutex);
 m812liveBeforeSubmit(q);
 VkResult r=next(q,count,infos,fence);
 if(m835Enabled() && r==VK_SUCCESS)for(uint32_t i=0;infos && i<count;++i)
  for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)m835Profiler.submitted(infos[i].pCommandBufferInfos[j].commandBuffer);
 if(m837Enabled() && r==VK_SUCCESS){std::vector<uint64_t> ids;
  for(uint32_t i=0;infos && i<count;++i){for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)ids.push_back(m833Id(infos[i].pCommandBufferInfos[j].commandBuffer));}
  m837Submitted(q,ids);
 }
 m812hAfterSubmit(q,r); return r;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue q,uint32_t count,const VkSubmitInfo2* infos,VkFence fence) {
 PFN_vkQueueSubmit2KHR next=nullptr;
 {std::lock_guard<std::mutex> guard(mu);auto qi=m812gQueueDevices.find(q);if(qi!=m812gQueueDevices.end()){
  auto di=devices.find(qi->second);if(di!=devices.end())next=di->second.m812gQueueSubmit2KHR;}}
 if(!next)return VK_ERROR_INITIALIZATION_FAILED;
 uint64_t cmds=0;for(uint32_t i=0;infos&&i<count;++i)cmds+=infos[i].commandBufferInfoCount;
 m812gReport("vkQueueSubmit2KHR",q,count,cmds,fence);
 std::lock_guard<std::mutex> queueSerial(m814ResourceMutex);
 m812liveBeforeSubmit(q);
 VkResult r=next(q,count,infos,fence);
 if(m835Enabled() && r==VK_SUCCESS)for(uint32_t i=0;infos && i<count;++i)
  for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)m835Profiler.submitted(infos[i].pCommandBufferInfos[j].commandBuffer);
 if(m837Enabled() && r==VK_SUCCESS){std::vector<uint64_t> ids;
  for(uint32_t i=0;infos && i<count;++i){for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)ids.push_back(m833Id(infos[i].pCommandBufferInfos[j].commandBuffer));}
  m837Submitted(q,ids);
 }
 m812hAfterSubmit(q,r); return r;
}
#define EARLY_ENTRY(result,name,args) extern "C" VKAPI_ATTR result VKAPI_CALL name args;
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice, const VkRenderPassCreateInfo2*, const VkAllocationCallbacks*, VkRenderPass*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,uint32_t,const VkImageCopy*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,uint32_t,const VkImageBlit*,VkFilter);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,uint32_t,const VkImageResolve*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer,const VkCopyImageInfo2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer,const VkBlitImageInfo2*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer,const VkResolveImageInfo2*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer, const VkRenderingInfo*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(VkCommandBuffer, const VkRenderPassBeginInfo*, const VkSubpassBeginInfo*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer,uint32_t,uint32_t,const VkRect2D*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer,const VkSubpassEndInfo*);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer,VkSubpassContents);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer,const VkSubpassBeginInfo*,const VkSubpassEndInfo*);



extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
  if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (p->loaderLayerInterfaceVersion > 2) p->loaderLayerInterfaceVersion = 2;
  p->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  p->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  p->pfnGetPhysicalDeviceProcAddr = nullptr;
  log("M2 loader negotiation OK");
  return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkInstance* out) {
  if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
  auto* chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(ci->pNext));
  while (chain && (chain->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || chain->function != VK_LAYER_LINK_INFO))
    chain = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
  if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
  auto* link = chain->u.pLayerInfo;
  auto gipa = link->pfnNextGetInstanceProcAddr;
  if (!gipa) return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!create) return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = link->pNext;
  VkResult r = create(ci, alloc, out);
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> guard(mu);
    instances[*out] = {gipa}; lastGipa = gipa; lastInstance = *out;
    log("M2 vkCreateInstance forwarded OK");
  } else logResult("M2 vkCreateInstance failed", r);
  return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,
    const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkDevice* out) {
  if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
  auto* chain = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(ci->pNext));
  while (chain && (chain->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || chain->function != VK_LAYER_LINK_INFO))
    chain = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain->pNext));
  if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
  auto* link = chain->u.pLayerInfo;
  auto gipa = link->pfnNextGetInstanceProcAddr;
  auto gdpa = link->pfnNextGetDeviceProcAddr;
  if (!gipa || !gdpa) return VK_ERROR_INITIALIZATION_FAILED;

  VkInstance instance;
  {
    std::lock_guard<std::mutex> guard(mu);
    instance = lastInstance; // M2 assumes one live Vulkan instance (BeamNG test setup).
  }
  auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
  if (!create) return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = link->pNext;

  bool alreadyExt = false;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    if (strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0)
      alreadyExt = true;
  log(alreadyExt ? "M2 application requests VRS extension" : "M2 application does not request VRS extension");

  VkDeviceCreateInfo modified = *ci;
  std::vector<const char*> extensions;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  bool inject = false;
  if (enabledByEnv()) {
    // If application already specifies this feature struct, don't override its choices.
    bool alreadyFeatures = findChain(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR) != nullptr;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!getFeatures)
      getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    bool supportedExt = false;
    if (enumerate) {
      uint32_t count = 0;
      if (enumerate(physical, nullptr, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkExtensionProperties> props(count);
        if (enumerate(physical, nullptr, &count, props.data()) == VK_SUCCESS)
          supportedExt = std::any_of(props.begin(), props.begin() + count, [](const auto& e) {
            return strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
          });
      }
    }
    if (supportedExt && getFeatures && !alreadyFeatures) {
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &vrs;
      getFeatures(physical, &f2);
      if (vrs.pipelineFragmentShadingRate && vrs.attachmentFragmentShadingRate) {
        // Only enable capabilities actually supported; preserve all existing app feature structs.
        vrs.primitiveFragmentShadingRate = VK_FALSE;
        vrs.pNext = const_cast<void*>(ci->pNext);
        modified.pNext = &vrs;
        if (!alreadyExt) {
          extensions.assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
          extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
          modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
          modified.ppEnabledExtensionNames = extensions.data();
        }
        inject = true;
        log("M2 VRS feature enablement requested (pipeline + attachment)");
      } else log("M2 VRS feature enablement skipped: insufficient GPU features");
    } else log("M2 VRS enablement skipped: extension/query unavailable or app already provides VRS feature struct");
  } else log("M2 VRS feature enablement OFF (set BEAMNG_VRS_ENABLE_FEATURES=1 to test)");


  // M4.2: query actual shading-rate attachment tile size and supported rates.
  // This does not change GPU rendering or allocate any Vulkan resources.
  if (diagnosticEnabled()) {
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      gipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!getProps) getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      gipa(instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (getProps) {
      VkPhysicalDeviceFragmentShadingRatePropertiesKHR rateProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &rateProps;
      getProps(physical, &properties);
      char b[360];
      sprintf_s(b, "M4.2 GPU: %s; shadingRateAttachmentTexelSize min=%ux%u max=%ux%u; maxFragmentSize=%ux%u; layered=%u",
        properties.properties.deviceName,
        rateProps.minFragmentShadingRateAttachmentTexelSize.width,
        rateProps.minFragmentShadingRateAttachmentTexelSize.height,
        rateProps.maxFragmentShadingRateAttachmentTexelSize.width,
        rateProps.maxFragmentShadingRateAttachmentTexelSize.height,
        rateProps.maxFragmentSize.width, rateProps.maxFragmentSize.height,
        unsigned(rateProps.layeredShadingRateAttachments));
      diagnosticLog(b);
      // This is a hypothetical pattern in attachment texels, NOT a bound image.
      auto tile = rateProps.minFragmentShadingRateAttachmentTexelSize;
      if (tile.width && tile.height) {
        const unsigned w = (5190u + tile.width - 1) / tile.width;
        const unsigned h = (4168u + tile.height - 1) / tile.height;
        sprintf_s(b, "M4.2 planned per-eye shading-rate image: %ux%u texels (tile=%ux%u), center=50%%,50%%, center radius=20%%, mid radius=42%%; NOT ACTIVE",
          w,h,tile.width,tile.height);
        diagnosticLog(b);
      }
    } else diagnosticLog("M4.2 vkGetPhysicalDeviceProperties2 unavailable");
    auto getRates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
      gipa(instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
    if (getRates) {
      uint32_t count=0;
      if (getRates(physical, &count, nullptr) == VK_SUCCESS && count) {
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count);
        for (auto& rate: rates) rate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        if (getRates(physical, &count, rates.data()) == VK_SUCCESS) {
          for (uint32_t i=0;i<count && i<32;++i) {
            char b[180];
            sprintf_s(b, "M4.2 supported rate: %ux%u sampleCounts=0x%X",
              rates[i].fragmentSize.width, rates[i].fragmentSize.height,
              unsigned(rates[i].sampleCounts));
            diagnosticLog(b);
          }
        }
      }
    } else diagnosticLog("M4.2 vkGetPhysicalDeviceFragmentShadingRatesKHR unavailable");
  }

  bool unorm4x4=false;
  if(m838Enabled() && inject){
    auto getRates=reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(gipa(instance,"vkGetPhysicalDeviceFragmentShadingRatesKHR"));
    uint32_t count=0;
    if(getRates && getRates(physical,&count,nullptr)==VK_SUCCESS && count && count<=256){
      std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count);
      for(auto& rate:rates)rate.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
      if(getRates(physical,&count,rates.data())==VK_SUCCESS)
        for(const auto& rate:rates)unorm4x4|=rate.fragmentSize.width==4 && rate.fragmentSize.height==4 && (rate.sampleCounts&VK_SAMPLE_COUNT_1_BIT);
    }
  }
  VkResult r = create(physical, inject ? &modified : ci, alloc, out);
  bool fallback = false;
  if (r != VK_SUCCESS && inject) {
    fallback = true;
    logResult("M2 modified vkCreateDevice failed; retrying original", r);
    r = create(physical, ci, alloc, out);
  }
  if (r == VK_SUCCESS) {
    std::lock_guard<std::mutex> guard(mu);
    std::vector<uint32_t> graphicsFamilies;
    auto createPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gdpa(*out, "vkCreateGraphicsPipelines"));
    DeviceDispatch dispatch{};
#define EARLY_ENTRY(result,name,args) dispatch.early_##name=reinterpret_cast<PFN_##name>(gdpa(*out,#name));
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY

    dispatch.getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*out, "vkGetDeviceQueue"));
    dispatch.getDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(gdpa(*out, "vkGetDeviceQueue2"));
    dispatch.m812gQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(gdpa(*out,"vkQueueSubmit"));
    dispatch.m812gQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(gdpa(*out,"vkQueueSubmit2"));
    dispatch.m812gQueueSubmit2KHR = reinterpret_cast<PFN_vkQueueSubmit2KHR>(gdpa(*out,"vkQueueSubmit2KHR"));
    dispatch.m812hCreateFence = reinterpret_cast<PFN_vkCreateFence>(gdpa(*out,"vkCreateFence"));
    dispatch.m812hGetFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(gdpa(*out,"vkGetFenceStatus"));
    dispatch.m812hDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(gdpa(*out,"vkDestroyFence"));
    dispatch.destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*out, "vkDestroyDevice"));
    dispatch.cmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(gdpa(*out,"vkCmdBindPipeline"));
    dispatch.destroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(gdpa(*out,"vkDestroyPipeline"));
    dispatch.physical = physical;
    // Family flags are queried from the physical device, not inferred from queue index.
    auto getFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (getFamilies) {
      uint32_t familyCount = 0; getFamilies(physical, &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> props(familyCount);
      if (familyCount) getFamilies(physical, &familyCount, props.data());
      for (uint32_t family = 0; family < familyCount; ++family)
        if (props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphicsFamilies.push_back(family);
    }
    if(m835Enabled()){
     auto getProps=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance,"vkGetPhysicalDeviceProperties"));
     const auto group=reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(findChain(ci->pNext,VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO));
     if(getProps && getFamilies && (!group || group->physicalDeviceCount<=1)){
      VkPhysicalDeviceProperties props{};getProps(physical,&props);
      uint32_t n=0;getFamilies(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);if(n)getFamilies(physical,&n,families.data());families.resize(n);
      beamng_profile::Api profile{};
      profile.create=reinterpret_cast<PFN_vkCreateQueryPool>(gdpa(*out,"vkCreateQueryPool"));
      profile.destroy=reinterpret_cast<PFN_vkDestroyQueryPool>(gdpa(*out,"vkDestroyQueryPool"));
      profile.results=reinterpret_cast<PFN_vkGetQueryPoolResults>(gdpa(*out,"vkGetQueryPoolResults"));
      profile.reset=reinterpret_cast<PFN_vkCmdResetQueryPool>(gdpa(*out,"vkCmdResetQueryPool"));
      profile.stamp=reinterpret_cast<PFN_vkCmdWriteTimestamp>(gdpa(*out,"vkCmdWriteTimestamp"));
      m835Profiler.focusUnorm(m838Compare());
      m835Profiler.device(*out,profile,props.limits.timestampPeriod,std::move(families));
      if(m838Compare())log("M8.38 UNORM_COMPARE enabled: alternating 10s baseline/VRS phases; first 2s excluded; focused nonblocking GPU timestamps");
      else log("M8.35 GPU_PROFILE enabled: sampled primary single-submit recordings; resource links M8.36; readback at reset/free; diagnostic overhead expected");
     }else log("M8.35 GPU_PROFILE unsupported device/query setup");
    }
    dispatch.gdpa = gdpa;
    // Resolve all calls from the NEXT layer, never from Vulkan loader exports.
    // Physical-device memory properties use the NEXT instance dispatch.
    auto& api = dispatch.ffrApi;
    api.getPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
#define RESOLVE_FFR(field, function) api.field = reinterpret_cast<PFN_##function>(gdpa(*out, #function))
    RESOLVE_FFR(createImage, vkCreateImage);
    RESOLVE_FFR(getImageMemoryRequirements, vkGetImageMemoryRequirements);
    RESOLVE_FFR(allocateMemory, vkAllocateMemory);
    RESOLVE_FFR(bindImageMemory, vkBindImageMemory);
    RESOLVE_FFR(createBuffer, vkCreateBuffer);
    RESOLVE_FFR(getBufferMemoryRequirements, vkGetBufferMemoryRequirements);
    RESOLVE_FFR(bindBufferMemory, vkBindBufferMemory);
    RESOLVE_FFR(mapMemory, vkMapMemory);
    RESOLVE_FFR(unmapMemory, vkUnmapMemory);
    RESOLVE_FFR(createCommandPool, vkCreateCommandPool);
    RESOLVE_FFR(allocateCommandBuffers, vkAllocateCommandBuffers);
    RESOLVE_FFR(resetCommandPool, vkResetCommandPool);
    RESOLVE_FFR(beginCommandBuffer, vkBeginCommandBuffer);
    RESOLVE_FFR(cmdPipelineBarrier, vkCmdPipelineBarrier);
    RESOLVE_FFR(cmdCopyBufferToImage, vkCmdCopyBufferToImage);
    RESOLVE_FFR(endCommandBuffer, vkEndCommandBuffer);
    RESOLVE_FFR(createFence, vkCreateFence);
    RESOLVE_FFR(resetFences, vkResetFences);
    RESOLVE_FFR(queueSubmit, vkQueueSubmit);
    RESOLVE_FFR(waitForFences, vkWaitForFences);
    RESOLVE_FFR(createImageView, vkCreateImageView);
    RESOLVE_FFR(destroyImageView, vkDestroyImageView);
    RESOLVE_FFR(destroyImage, vkDestroyImage);
    RESOLVE_FFR(freeMemory, vkFreeMemory);
    RESOLVE_FFR(destroyFence, vkDestroyFence);
    RESOLVE_FFR(destroyCommandPool, vkDestroyCommandPool);
    RESOLVE_FFR(destroyBuffer, vkDestroyBuffer);
    RESOLVE_FFR(deviceWaitIdle, vkDeviceWaitIdle);
#undef RESOLVE_FFR
    diagnosticLog(api.complete() ? "M4.8 FFR dispatch resolved: COMPLETE; upload OFF by default" : "M4.8 FFR dispatch INCOMPLETE; upload unavailable");
    dispatch.createPipelines = createPipelines;
    dispatch.vrsEnabled = inject && !fallback;
    dispatch.unorm4x4Supported=dispatch.vrsEnabled && unorm4x4;
    if(m838Enabled())log(dispatch.unorm4x4Supported?"M8.38 UNORM_DEVICE uniform 4x4 at one sample supported":"M8.38 UNORM_DEVICE unavailable: retaining original pipelines");
    dispatch.createRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(gdpa(*out, "vkCreateRenderPass"));
    dispatch.destroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(gdpa(*out, "vkDestroyRenderPass"));
    dispatch.createRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(gdpa(*out, "vkCreateRenderPass2"));
    dispatch.createFramebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(gdpa(*out, "vkCreateFramebuffer"));
    dispatch.cmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(gdpa(*out, "vkCmdCopyImage"));
    dispatch.cmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(gdpa(*out, "vkCmdBlitImage"));
    dispatch.cmdResolveImage = reinterpret_cast<PFN_vkCmdResolveImage>(gdpa(*out, "vkCmdResolveImage"));
    dispatch.cmdCopyImage2 = reinterpret_cast<PFN_vkCmdCopyImage2>(gdpa(*out, "vkCmdCopyImage2"));
    dispatch.cmdBlitImage2 = reinterpret_cast<PFN_vkCmdBlitImage2>(gdpa(*out, "vkCmdBlitImage2"));
    dispatch.cmdResolveImage2 = reinterpret_cast<PFN_vkCmdResolveImage2>(gdpa(*out, "vkCmdResolveImage2"));
    dispatch.createImageView = reinterpret_cast<PFN_vkCreateImageView>(gdpa(*out, "vkCreateImageView"));
    dispatch.destroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(gdpa(*out, "vkDestroyImageView"));
    dispatch.createImage = reinterpret_cast<PFN_vkCreateImage>(gdpa(*out, "vkCreateImage"));
    dispatch.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gdpa(*out, "vkAllocateCommandBuffers"));
    dispatch.freeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(gdpa(*out, "vkFreeCommandBuffers"));
    dispatch.cmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa(*out, "vkCmdBeginRendering"));
    dispatch.cmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(gdpa(*out, "vkCmdBeginRenderingKHR"));
    dispatch.cmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(gdpa(*out, "vkCmdBeginRenderPass"));
    dispatch.cmdBeginRenderPass2 = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(gdpa(*out, "vkCmdBeginRenderPass2"));
    dispatch.destroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(gdpa(*out, "vkDestroyFramebuffer"));
    dispatch.cmdSetScissor = reinterpret_cast<PFN_vkCmdSetScissor>(gdpa(*out,"vkCmdSetScissor"));
    dispatch.cmdClearAttachments = reinterpret_cast<PFN_vkCmdClearAttachments>(gdpa(*out,"vkCmdClearAttachments"));
    dispatch.cmdEndRenderPass = reinterpret_cast<PFN_vkCmdEndRenderPass>(gdpa(*out,"vkCmdEndRenderPass"));
    dispatch.cmdEndRenderPass2 = reinterpret_cast<PFN_vkCmdEndRenderPass2>(gdpa(*out,"vkCmdEndRenderPass2"));
    dispatch.cmdNextSubpass = reinterpret_cast<PFN_vkCmdNextSubpass>(gdpa(*out,"vkCmdNextSubpass"));
    dispatch.cmdNextSubpass2 = reinterpret_cast<PFN_vkCmdNextSubpass2>(gdpa(*out,"vkCmdNextSubpass2"));
    m827GraphicsFamilies[*out]=std::move(graphicsFamilies);
    devices[*out] = dispatch; lastGdpa = gdpa;
    log(inject && !fallback ? "M3 VRS enabled at device creation" : "M3 VRS not enabled on device");
  } else logResult("M2 vkCreateDevice failed", r);
  return r;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!name) return nullptr;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (!strcmp(name, "vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (!strcmp(name, "vkGetDeviceQueue2")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
  if (!strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit);
  if (!strcmp(name, "vkQueueSubmit2")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2);
  if (!strcmp(name, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2KHR);
  if (!strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
#define EARLY_ENTRY(result,entry,args) if((m837Tracking() || m835Entry(#entry)) && !strcmp(name,#entry))return reinterpret_cast<PFN_vkVoidFunction>(entry);
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY
  if (!strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines);
  if (!strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline);
  if (!strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline);
  if (!strcmp(name, "vkCreateRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass);
  if (!strcmp(name, "vkCreateRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass2);
  if (!strcmp(name, "vkDestroyRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyRenderPass);
  if (!strcmp(name, "vkCreateFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateFramebuffer);
  if (!strcmp(name, "vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage);
  if (!strcmp(name, "vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage);
  if (!strcmp(name, "vkCmdResolveImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage);
  if (!strcmp(name, "vkCmdCopyImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2);
  if (!strcmp(name, "vkCmdBlitImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2);
  if (!strcmp(name, "vkCmdResolveImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2);
  if (!strcmp(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
  if (!strcmp(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView);
  if (!strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
  if (!strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers);
  if (!strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers);
  if (!strcmp(name, "vkCmdBeginRendering")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
  if (!strcmp(name, "vkCmdBeginRenderingKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderingKHR);
  if (!strcmp(name, "vkCmdSetScissor")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissor);
  if (!strcmp(name, "vkCmdEndRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass);
  if (!strcmp(name, "vkCmdEndRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass2);
  if (!strcmp(name, "vkCmdNextSubpass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass);
  if (!strcmp(name, "vkCmdNextSubpass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass2);
  if (!strcmp(name, "vkCmdBeginRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass);
  if (!strcmp(name, "vkCmdBeginRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass2);
  if (!strcmp(name, "vkDestroyFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFramebuffer);

  if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
  if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
#define EARLY_ENTRY(result,entry,args) if((m837Tracking() || m835Entry(#entry)) && !strcmp(name,#entry))return reinterpret_cast<PFN_vkVoidFunction>(entry);
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY
  if (!strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines);
  if (!strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline);
  if (!strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline);
  if (!strcmp(name, "vkCreateRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass);
  if (!strcmp(name, "vkCreateRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass2);
  if (!strcmp(name, "vkDestroyRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyRenderPass);
  if (!strcmp(name, "vkCreateFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateFramebuffer);
  if (!strcmp(name, "vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage);
  if (!strcmp(name, "vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage);
  if (!strcmp(name, "vkCmdResolveImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage);
  if (!strcmp(name, "vkCmdCopyImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2);
  if (!strcmp(name, "vkCmdBlitImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2);
  if (!strcmp(name, "vkCmdResolveImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2);
  if (!strcmp(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
  if (!strcmp(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView);
  if (!strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
  if (!strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers);
  if (!strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers);
  if (!strcmp(name, "vkCmdBeginRendering")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
  if (!strcmp(name, "vkCmdBeginRenderingKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderingKHR);
  if (!strcmp(name, "vkCmdSetScissor")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissor);
  if (!strcmp(name, "vkCmdEndRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass);
  if (!strcmp(name, "vkCmdEndRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass2);
  if (!strcmp(name, "vkCmdNextSubpass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass);
  if (!strcmp(name, "vkCmdNextSubpass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass2);
  if (!strcmp(name, "vkCmdBeginRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass);
  if (!strcmp(name, "vkCmdBeginRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass2);
  if (!strcmp(name, "vkDestroyFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFramebuffer);

  PFN_vkGetInstanceProcAddr gipa = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu);
    auto it = instances.find(instance);
    gipa = it != instances.end() ? it->second.gipa : lastGipa;
  }
  return gipa ? gipa(instance, name) : nullptr;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (!name) return nullptr;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (!strcmp(name, "vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (!strcmp(name, "vkGetDeviceQueue2")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
  if (!strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit);
  if (!strcmp(name, "vkQueueSubmit2")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2);
  if (!strcmp(name, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit2KHR);
  if (!strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
#define EARLY_ENTRY(result,entry,args) if((m837Tracking() || m835Entry(#entry)) && !strcmp(name,#entry)){std::lock_guard<std::mutex> g(mu);auto d=devices.find(device);return d!=devices.end() && d->second.early_##entry?reinterpret_cast<PFN_vkVoidFunction>(entry):nullptr;}
#include "early_vrs_entrypoints.inc"
#undef EARLY_ENTRY
  if (!strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateGraphicsPipelines);
  if (!strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBindPipeline);
  if (!strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyPipeline);
  if (!strcmp(name, "vkCreateRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass);
  if (!strcmp(name, "vkCreateRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateRenderPass2);
  if (!strcmp(name, "vkDestroyRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyRenderPass);
  if (!strcmp(name, "vkCreateFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateFramebuffer);
  if (!strcmp(name, "vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage);
  if (!strcmp(name, "vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage);
  if (!strcmp(name, "vkCmdResolveImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage);
  if (!strcmp(name, "vkCmdCopyImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdCopyImage2);
  if (!strcmp(name, "vkCmdBlitImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBlitImage2);
  if (!strcmp(name, "vkCmdResolveImage2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdResolveImage2);
  if (!strcmp(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImageView);
  if (!strcmp(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyImageView);
  if (!strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateImage);
  if (!strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkAllocateCommandBuffers);
  if (!strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(vkFreeCommandBuffers);
  if (!strcmp(name, "vkCmdBeginRendering")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
  if (!strcmp(name, "vkCmdBeginRenderingKHR")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderingKHR);
  if (!strcmp(name, "vkCmdSetScissor")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissor);
  if (!strcmp(name, "vkCmdEndRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass);
  if (!strcmp(name, "vkCmdEndRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderPass2);
  if (!strcmp(name, "vkCmdNextSubpass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass);
  if (!strcmp(name, "vkCmdNextSubpass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdNextSubpass2);
  if (!strcmp(name, "vkCmdBeginRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass);
  if (!strcmp(name, "vkCmdBeginRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderPass2);
  if (!strcmp(name, "vkDestroyFramebuffer")) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyFramebuffer);

  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu);
    auto it = devices.find(device);
    gdpa = it != devices.end() ? it->second.gdpa : lastGdpa;
  }
  return gdpa ? gdpa(device, name) : nullptr;
}

// M3: pipeline-level 4x4 VRS. This changes only pipelines created while opt-in is active.
// Attachment-based/per-eye foveation is NOT implemented in M3.
// M4.6: queue capture only. No GPU work is submitted from these interceptors.
// M5.0: prepares pipelines to consume a shading-rate attachment once render-pass
// integration is implemented. Does not attach any image by itself.
static bool attachmentCombinerOptIn() {
  char value[8] = {};
  return beamng_config::read("BEAMNG_VRS_ATTACHMENT_PIPELINES", value, sizeof(value)) && strcmp(value, "1") == 0;
}
static bool gpuUploadOptIn() {
  char value[8] = {};
  return beamng_config::read("BEAMNG_VRS_UPLOAD_TEST", value, sizeof(value)) && strcmp(value, "1") == 0;
}
static void recordGraphicsQueue(VkDevice device, VkQueue queue, uint32_t family, uint32_t index) {
  if (!queue) { log("M5.2 queue hook returned null queue"); return; }
  static std::atomic<unsigned> queueLogs{0};
  if (queueLogs.fetch_add(1) < 12) {
    char b[180];
    sprintf_s(b, "M5.2 queue intercepted: family=%u index=%u uploadRequested=%u", family, index, gpuUploadOptIn() ? 1u : 0u);
    log(b);
  }
  DeviceDispatch dispatch{};
  bool shouldUpload = false;
  {
    std::lock_guard<std::mutex> guard(mu);
    auto it = devices.find(device);
    if (it == devices.end()) { log("M5.2 queue device not tracked"); return; }
    const auto& families = m827GraphicsFamilies[device];
    if (std::find(families.begin(), families.end(), family) == families.end()) { log("M5.2 queue family is not graphics-capable"); return; }
    auto& list = capturedQueues[device];
    bool found = false;
    for (const auto& q : list) if (q.queue == queue) { found = true; break; }
    if (!found) list.push_back({queue, family, index});
    dispatch = it->second;
    shouldUpload = gpuUploadOptIn() && dispatch.vrsEnabled && dispatch.ffrApi.complete() &&
      uploadAttempted.insert(device).second;
  }
  if (!shouldUpload) {
    static std::atomic<unsigned> skipLogs{0};
    if (skipLogs.fetch_add(1) < 12) {
      char b[240];
      sprintf_s(b, "M5.2 upload skipped: optIn=%u vrs=%u apiComplete=%u",
        gpuUploadOptIn() ? 1u : 0u, dispatch.vrsEnabled ? 1u : 0u,
        dispatch.ffrApi.complete() ? 1u : 0u);
      log(b);
    }
    return;
  }
  // Experimental explicit opt-in: vkGetDeviceQueue is expected to occur before
  // application queue submissions. Never run this on a queue used concurrently.
  try {
    log("M5.2 GPU upload starting");
    m814ActiveWidth.store(m813Dimension("BEAMNG_VRS_TARGET_WIDTH",5190),std::memory_order_release);
    m814ActiveHeight.store(m813Dimension("BEAMNG_VRS_TARGET_HEIGHT",4168),std::memory_order_release);
    auto pattern = m58MakePattern();
    {
      char settings[320];
      sprintf_s(settings, "M5.9 configuration mode=%d centerSize=%.3f midSize=%.3f midRate=%u outerRate=%u framebuffer=%ux%u",
        m58PatternMode(), m59Radius("BEAMNG_VRS_CENTER_RADIUS", 0.30f),
        m59Radius("BEAMNG_VRS_MID_RADIUS", 0.65f),
        static_cast<unsigned>(m59Rate("BEAMNG_VRS_MID_RATE", beamng_ffr::rate(2,2))),
        static_cast<unsigned>(m59Rate("BEAMNG_VRS_OUTER_RATE", beamng_ffr::rate(4,4))),m813Width(),m813Height());
      log(settings);
      char shapeMsg[240]{};
      sprintf_s(shapeMsg,"M8.24 SHAPE shape=%s sizeBasis=DIAMETER_OVER_EYE_HEIGHT size0.5=halfEyeHeight ellipseWidth=viewportRelative",
        beamng_ffr::shapeName(m824Shape()));
      log(shapeMsg);
    }
    unsigned rateCounts[3] = {};
    for (uint8_t texel : pattern.texels) ++rateCounts[texel == 0 ? 0 : texel == 5 ? 1 : 2];
    char m58Log[256];
    sprintf_s(m58Log, "M5.8 uploaded pattern mode=%d tiles=%ux%u rate1x1=%u rate2x2=%u rate4x4=%u",
      m58PatternMode(), pattern.width, pattern.height, rateCounts[0], rateCounts[1], rateCounts[2]);
    log(m58Log);
    auto uploaded = beamng_ffr::upload(dispatch.ffrApi, dispatch.physical, device, queue, family, pattern);
    {
      std::lock_guard<std::mutex> guard(mu);
      uploadedPatterns[device] = uploaded;
    }
    if (m8Enabled()) {
      std::array<beamng_ffr::GpuImage,2> eyeImages{};
      try {
        eyeImages[0]=beamng_ffr::upload(dispatch.ffrApi,dispatch.physical,device,queue,family,pattern);
        eyeImages[1]=beamng_ffr::upload(dispatch.ffrApi,dispatch.physical,device,queue,family,pattern);
        {std::lock_guard<std::mutex> guard(mu);m83EyePatterns[device]=eyeImages;}
        log("M8.3 two independent eye shading-rate images uploaded; FIXED patterns, not gaze-updated");
        // M8.16: obsolete spare VRS images were never attached to the active
        // eye render passes. Keep an empty pool entry for M8.14 generation
        // compatibility, but do not allocate four unused VkImages per scale.
        {std::lock_guard<std::mutex> guard(mu);m812ImagePools[device]=M812ImagePool{};}
        log("M8.16 UNUSED_VRS_IMAGE_POOL_SKIPPED eyeImages=2 spareImages=0");
        // M8.12a: allocate reusable upload buffers after initial static images
        // are ready. Never submit or overwrite a GPU-visible shading-rate image.
        M812Staging staging{};
        try {
          const VkDeviceSize bytes = static_cast<VkDeviceSize>(pattern.texels.size());
          for(auto& eye : staging.slots)
            for(auto& slot : eye)
              slot=beamng_ffr::createStaging(dispatch.ffrApi,dispatch.physical,device,bytes);
          {std::lock_guard<std::mutex> guard(mu);m812Staging[device]=staging;}
          char msg[220]{};
          sprintf_s(msg,"M8.12a STAGING_READY eyes=2 slotsPerEye=3 bytesPerSlot=%llu GPU_SUBMIT=DISABLED",
              static_cast<unsigned long long>(bytes));
          log(msg);
        } catch(const std::exception& ex) {
          for(auto& eye:staging.slots)
            for(auto& slot:eye)beamng_ffr::destroyStaging(dispatch.ffrApi,device,slot);
          char msg[300]{};
          sprintf_s(msg,"M8.12a STAGING_FAILED: %s; static VRS unaffected",ex.what());
          log(msg);
        }

      } catch (...) {
        for(auto& eyeImage:eyeImages)beamng_ffr::destroy(dispatch.ffrApi,device,eyeImage);
        throw;
      }
    }
    log("M4.8 GPU shading-rate image uploaded and transitioned successfully; NOT BOUND to render pass");
  } catch (const std::exception& e) {
    char b[512]; sprintf_s(b, "M4.8 GPU upload FAILED: %s", e.what()); log(b);
  }
}

// OpenXR's independently versioned shared-memory target; avoids inferring eye
// resolution from arbitrary large Vulkan framebuffers.
bool m814ReadXRTarget(uint32_t& w,uint32_t& h){
 static HANDLE mapping=nullptr;
 static const beamng_eye_resolution::Data* shared=nullptr;
 if(!shared){
  if(!mapping)mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_eye_resolution::mappingName);
  if(mapping)shared=static_cast<const beamng_eye_resolution::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_eye_resolution::Data)));
 }
 if(!shared)return false;
 for(int attempt=0;attempt<3;++attempt){
  LONG a=shared->sequence;MemoryBarrier();if(a&1)continue;
  uint32_t magic=shared->magic,pid=shared->processId;
  uint32_t rw=shared->width,rh=shared->height;
  MemoryBarrier();LONG b=shared->sequence;
  if(a==b && !(b&1) && magic==beamng_eye_resolution::magicValue &&
     pid==GetCurrentProcessId() && rw>=640 && rh>=640 && rw<=16384 && rh<=16384){w=rw;h=rh;return true;}
 }
 return false;
}
// Return the single authoritative target: both overrides, or OpenXR pair.
bool m814Target(uint32_t& w,uint32_t& h){
 uint32_t ow=m813Dimension("BEAMNG_VRS_TARGET_WIDTH",0);
 uint32_t oh=m813Dimension("BEAMNG_VRS_TARGET_HEIGHT",0);
 if(ow && oh){w=ow;h=oh;return true;}
 return m814ReadXRTarget(w,h);
}
// Cropping still needs the OpenXR eye size when shading-rate images are off.
// Resolve it during object creation, never by guessing from a desktop target
// and never by allocating VRS resources or waiting for the GPU.
void m831RefreshCropTarget(){
 if(m8Enabled() || !cropEnabled())return;
 uint32_t w=0,h=0;
 if(!m814Target(w,h))return;
 std::lock_guard<std::mutex> guard(mu);
 m814ActiveWidth.store(w,std::memory_order_release);
 m814ActiveHeight.store(h,std::memory_order_release);
}
// Safe experimental setup: serialize with live uploads and wait for GPU idle.
// No old framebuffers or old VkImageViews are destroyed on a resolution switch.
bool m814EnsureResources(VkDevice device,uint32_t w,uint32_t h){
 if(!m8Enabled())return false;
 const uint64_t key=m814Key(w,h);
 std::lock_guard<std::mutex> serial(m814ResourceMutex);
 std::lock_guard<std::mutex> uploadSerial(m812liveQueueMutex);
 DeviceDispatch d{};CapturedQueue q{};bool hasQueue=false;
 {
  std::lock_guard<std::mutex> guard(mu);
  auto di=devices.find(device);if(di==devices.end() || !di->second.vrsEnabled)return false;
  d=di->second;
  auto qi=capturedQueues.find(device);
  if(qi!=capturedQueues.end() && !qi->second.empty()){q=qi->second.front();hasQueue=true;}
 }
 if(!hasQueue || !d.ffrApi.complete())return false;
 // Bootstrap all resources from M8.13 when first needed, without allocating twice.
 {
  std::lock_guard<std::mutex> guard(mu);
  auto& sets=m814ResourceSets[device];
  if(sets.empty()){
   auto it = uploadedPatterns.find(device);
   auto eyes = m83EyePatterns.find(device);
   auto pool = m812ImagePools.find(device);
   auto st = m812Staging.find(device);
   if(it!=uploadedPatterns.end() && eyes!=m83EyePatterns.end() && pool!=m812ImagePools.end() && st!=m812Staging.end()){
    sets.emplace(m814Key(5190,4168),M814ResourceSet{it->second,eyes->second,pool->second,st->second});
    log("M8.14 BOOTSTRAP_RESOURCE_GENERATION_CACHED");
    // If the user supplied a manual target when launching M8.13, use that.
    const uint32_t bootW=m813Dimension("BEAMNG_VRS_TARGET_WIDTH",5190);
    const uint32_t bootH=m813Dimension("BEAMNG_VRS_TARGET_HEIGHT",4168);
    if(bootW!=5190 || bootH!=4168){auto node=sets.extract(m814Key(5190,4168));node.key()=m814Key(bootW,bootH);sets.insert(std::move(node));}
   }
  }
  auto it=sets.find(key);
  if(it!=sets.end()){
   // Most framebuffers are created in batches. Do not reset the uploaded gaze
   // sequence on each same-resolution VkFramebuffer creation.
   if(m813Width()==w && m813Height()==h)return true;
   uploadedPatterns[device]=it->second.base;
   m83EyePatterns[device]=it->second.eyes;
   m812ImagePools[device]=it->second.imagePool;
   m812Staging[device]=it->second.staging;
   m812liveUploaded[device]=m812bPreparedCount;
   m815SlotStates[device][key].latestSeq=m812bPreparedCount;
   m815SlotStates[device][key].latestSlot={{-1,-1}};
   m814ActiveWidth.store(w,std::memory_order_release);
   m814ActiveHeight.store(h,std::memory_order_release);
   m825LastResizeMs.store(GetTickCount64(),std::memory_order_release);
   char outlineMsg[180]{};sprintf_s(outlineMsg,"M8.25 OUTLINE_RESIZE_GUARD width=%u height=%u pauseMs=%u",w,h,m825OutlinePauseMs());log(outlineMsg);
   char b[240]{};sprintf_s(b,"M8.14 AUTO_RESOLUTION_REUSED width=%u height=%u tiles=%ux%u",w,h,(w+15)/16,(h+15)/16);log(b);
   return true;
  }
 }
 M814ResourceSet fresh{};
 try{
  // vkCreateFramebuffer may be reached while other render work is still in
  // flight. The existing live uploader is similarly synchronous.
  beamng_ffr::check(d.ffrApi.deviceWaitIdle(device),"M8.14 wait before resize");
  const auto pattern=m58MakePattern(w,h);
  fresh.base=beamng_ffr::upload(d.ffrApi,d.physical,device,q.queue,q.family,pattern);
  for(auto& image:fresh.eyes)image=beamng_ffr::upload(d.ffrApi,d.physical,device,q.queue,q.family,pattern);
  // M8.16: the active live uploader updates fresh.eyes in place; it never
  // binds the former spare GPU image pool, so no extra allocations are needed.
  for(auto& eye:fresh.staging.slots)for(auto& slot:eye)
   slot=beamng_ffr::createStaging(d.ffrApi,d.physical,device,pattern.texels.size());
 }catch(const std::exception& e){
  char b[320]{};sprintf_s(b,"M8.14 RESOURCE_CREATE_FAILED dims=%ux%u reason=%s",w,h,e.what());log(b);
  d.ffrApi.deviceWaitIdle(device);
  if(fresh.base.image)beamng_ffr::destroy(d.ffrApi,device,fresh.base);
  for(auto& image:fresh.eyes)if(image.image)beamng_ffr::destroy(d.ffrApi,device,image);
  for(auto& eye:fresh.imagePool.extra)for(auto& image:eye)if(image.image)beamng_ffr::destroy(d.ffrApi,device,image);
  for(auto& eye:fresh.staging.slots)for(auto& slot:eye)beamng_ffr::destroyStaging(d.ffrApi,device,slot);
  return false;
 }
 {
  std::lock_guard<std::mutex> guard(mu);
  auto& stored=m814ResourceSets[device][key];stored=fresh;
  uploadedPatterns[device]=fresh.base;
  m83EyePatterns[device]=fresh.eyes;
  m812ImagePools[device]=fresh.imagePool;
  m812Staging[device]=fresh.staging;
  m812liveUploaded[device]=m812bPreparedCount;
  m815SlotStates[device][key].latestSeq=m812bPreparedCount;
  m815SlotStates[device][key].latestSlot={{-1,-1}};
  m814ActiveWidth.store(w,std::memory_order_release);
  m814ActiveHeight.store(h,std::memory_order_release);
  m825LastResizeMs.store(GetTickCount64(),std::memory_order_release);
 }
 char outlineMsg[180]{};sprintf_s(outlineMsg,"M8.25 OUTLINE_RESIZE_GUARD width=%u height=%u pauseMs=%u",w,h,m825OutlinePauseMs());log(outlineMsg);
 char b[260]{};sprintf_s(b,"M8.14 AUTO_RESOLUTION_READY width=%u height=%u tiles=%ux%u resourceGenerations=CACHED",w,h,(w+15)/16,(h+15)/16);log(b);
 return true;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
  PFN_vkGetDeviceQueue next = nullptr;
  { std::lock_guard<std::mutex> guard(mu); auto it=devices.find(device); if(it!=devices.end()) next=it->second.getDeviceQueue; }
  if (!next) { log("M5.2 vkGetDeviceQueue missing next dispatch"); if(out) *out=VK_NULL_HANDLE; return; }
  next(device, family, index, out);
  if (out && *out) { std::lock_guard<std::mutex> guard(mu); m812gQueueDevices[*out]=device; }
  if (out) recordGraphicsQueue(device, *out, family, index);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* out) {
  PFN_vkGetDeviceQueue2 next = nullptr;
  { std::lock_guard<std::mutex> guard(mu); auto it=devices.find(device); if(it!=devices.end()) next=it->second.getDeviceQueue2; }
  if (!next) { log("M5.2 vkGetDeviceQueue2 missing next dispatch"); if(out) *out=VK_NULL_HANDLE; return; }
  next(device, info, out);
  if (out && *out) { std::lock_guard<std::mutex> guard(mu); m812gQueueDevices[*out]=device; }
  if (info && out) recordGraphicsQueue(device, *out, info->queueFamilyIndex, info->queueIndex);
}
// Build static crop variants at pipeline creation, never during draw recording.
// Dynamic-scissor pipelines are cropped by the command hook instead.
static void m830CreateCropVariants(VkDevice device,VkPipelineCache cache,VkPipeline base,
 const VkGraphicsPipelineCreateInfo& info,const VkPipelineViewportStateCreateInfo* original,
 const VkAllocationCallbacks* alloc,const DeviceDispatch& d) {
 if(!cropEnabled() || !cropAsymmetric() || !base || !d.destroyPipeline ||
    !original || !original->pScissors || !original->scissorCount || original->scissorCount>16 ||
    !info.pViewportState || !info.pViewportState->pScissors || info.basePipelineHandle ||
    (info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT))return;
 if(info.pDynamicState)for(uint32_t i=0;i<info.pDynamicState->dynamicStateCount;++i)
  if(info.pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_SCISSOR ||
     info.pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT)return;
 const uint32_t width=m813Width(),height=m813Height();
 bool eligible=false;
 for(uint32_t i=0;i<original->scissorCount;++i) {
  const auto& r=original->pScissors[i];
  eligible|=r.offset.x==0 && r.offset.y==0 && r.extent.width==width && r.extent.height==height;
 }
 if(!eligible)return;
 std::array<VkPipeline,2> copies{};
 bool complete=true;
 for(int eye=0;eye<2;++eye){
  std::array<VkRect2D,16> scissors{};
  for(uint32_t i=0;i<original->scissorCount;++i){
   const auto& r=original->pScissors[i];
   scissors[i]=info.pViewportState->pScissors[i];
   if(r.offset.x==0 && r.offset.y==0 && r.extent.width==width && r.extent.height==height)
    scissors[i]=intersectRect(scissors[i],cropRect(width,height,eye));
  }
  auto viewport=*info.pViewportState;viewport.pScissors=scissors.data();
  auto variant=info;variant.pViewportState=&viewport;
  const auto result=d.createPipelines(device,cache,1,&variant,alloc,&copies[eye]);
  if(result!=VK_SUCCESS || !copies[eye]){complete=false;break;}
 }
 if(!complete){
  for(auto copy:copies)if(copy)d.destroyPipeline(device,copy,alloc);
  log("M8.30 per-eye static crop variants unavailable; retaining common crop and eye-specific border clears");
  return;
 }
 std::lock_guard<std::mutex> guard(mu);
 m830CropVariants[base]={device,copies};
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* infos,
    const VkAllocationCallbacks* alloc, VkPipeline* out) {
  DeviceDispatch d{};
  {
    std::lock_guard<std::mutex> guard(mu);
    auto it = devices.find(device);
    if (it == devices.end()) return VK_ERROR_INITIALIZATION_FAILED;
    d = it->second;
  }
  if (!d.createPipelines) return VK_ERROR_INITIALIZATION_FAILED;
  m831RefreshCropTarget();
  const bool uniform = uniformVrsEnabled();
  const bool attachment = attachmentCombinerOptIn();
  // The modes are mutually exclusive. Prefer the safe pass-through if both are set.
  if (!count || !infos) return d.createPipelines(device, cache, count, infos, alloc, out);
  const bool modifyVrs = d.vrsEnabled && (uniform != attachment);
  // Filled only on the crop/modification path. Compatible VRS copies must
  // inherit the same static-scissor crop as their original pipeline.
  std::vector<VkPipelineViewportStateCreateInfo> viewportStates;
  // UNORM pilot variants remain compatible with the ORIGINAL render pass.
  // Missing/unsupported variants therefore safely use the application's pipeline.
  auto m838CreateVariants=[&](VkResult result){
    if(result!=VK_SUCCESS || !m838Enabled() || !d.unorm4x4Supported || !d.destroyPipeline)return;
    for(uint32_t i=0;out && i<count;++i){
      M88PassSignature compatibility;
      {std::lock_guard<std::mutex> guard(mu);
       auto key=m88PassSignatures.find(infos[i].renderPass);
       if(!m838UnormPasses.count(infos[i].renderPass) || key==m88PassSignatures.end())continue;
       compatibility=key->second;
      }
      if(!out[i] || !beamng_unorm::eligible(infos[i])){++m838VariantSkipped;continue;}
      auto variant=infos[i];auto rate=beamng_unorm::rate4x4();variant.pNext=&rate;
      if(i<viewportStates.size() && viewportStates[i].pScissors)variant.pViewportState=&viewportStates[i];
      VkPipeline replacement=VK_NULL_HANDLE;
      if(d.createPipelines(device,cache,1,&variant,nullptr,&replacement)==VK_SUCCESS && replacement){
        std::lock_guard<std::mutex> guard(mu);m838Variants[out[i]]={device,replacement,std::move(compatibility)};++m838VariantCreated;
      }else {if(replacement)d.destroyPipeline(device,replacement,nullptr);++m838VariantFailed;}
    }
  };
  // M8.9: run for BOTH pipeline creation branches. Previously only the
  // passthrough branch created variants, while attachment-combiner mode used
  // the modified branch exclusively.
  auto m89CreateVariants = [&](VkResult result) {
    // M8.9: create a separate pipeline against the shadow render pass.
    // Keep original handles unchanged. Only complete, successful variants can be used.
    if(result==VK_SUCCESS && m8Enabled() && m89StaticEnabled() && d.vrsEnabled && d.destroyPipeline) {
      for(uint32_t i=0;i<count;++i) {
        if(!out || !out[i] || !infos[i].renderPass || infos[i].basePipelineHandle ||
           (infos[i].flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT))continue;
        VkRenderPass shadow=VK_NULL_HANDLE;
        M88PassSignature compatibility;
        {std::lock_guard<std::mutex> guard(mu);
         auto it=shadowPasses.find(infos[i].renderPass);
         if(it!=shadowPasses.end() && it->second.device==device){
          const auto key=m832ShadowSignatures.find(it->second.shadow);
          if(key!=m832ShadowSignatures.end() && !key->second.fields.empty()){
           shadow=it->second.shadow;compatibility=key->second;
          }
         }
        }
        if(!shadow){
          if(m89VariantReports.fetch_add(1,std::memory_order_relaxed)<40)
            log("M8.9 VARIANT skipped: no matching shadow render pass");
          continue;
        }
        VkGraphicsPipelineCreateInfo variant=infos[i];
        if(i<viewportStates.size() && viewportStates[i].pScissors)
          variant.pViewportState=&viewportStates[i];
        variant.renderPass=shadow;
        VkPipelineFragmentShadingRateStateCreateInfoKHR rate{};
        rate.sType=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
        rate.pNext=variant.pNext;
        rate.fragmentSize={1,1};
        rate.combinerOps[0]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        rate.combinerOps[1]=VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
        // Do not insert duplicate VRS state in the pNext chain.
        if(findChain(variant.pNext,VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR)) {
          if(m89VariantReports.fetch_add(1,std::memory_order_relaxed)<40)
            log("M8.9 VARIANT skipped: existing pipeline VRS pNext state");
          continue;
        }
        variant.pNext=&rate;
        VkPipeline replacement=VK_NULL_HANDLE;
        VkResult vr=d.createPipelines(device,cache,1,&variant,alloc,&replacement);
        if(vr==VK_SUCCESS && replacement) {
          VkPipeline masked=VK_NULL_HANDLE;
          bool stencilPass=false;
          {std::lock_guard<std::mutex> guard(mu);
           auto si=m818StencilCapable.find(infos[i].renderPass);
           stencilPass=si!=m818StencilCapable.end() && si->second;
          }
          // Do not interfere with pipelines already using stencil, or with
          // dynamic stencil state that could override our fixed reference=1.
          bool stencilDynamic=false;
          if(infos[i].pDynamicState){
           for(uint32_t j=0;j<infos[i].pDynamicState->dynamicStateCount;++j){
            const auto dynamic=infos[i].pDynamicState->pDynamicStates[j];
            if(dynamic==VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK ||
               dynamic==VK_DYNAMIC_STATE_STENCIL_WRITE_MASK ||
               dynamic==VK_DYNAMIC_STATE_STENCIL_REFERENCE)stencilDynamic=true;
           }
          }
          if(m818Enabled() && stencilPass && !stencilDynamic && infos[i].pDepthStencilState &&
             infos[i].pDepthStencilState->stencilTestEnable==VK_FALSE &&
             infos[i].subpass==0){
           VkPipelineDepthStencilStateCreateInfo ds=*infos[i].pDepthStencilState;
           ds.stencilTestEnable=VK_TRUE;
           VkStencilOpState stencil{};
           stencil.failOp=VK_STENCIL_OP_KEEP;stencil.passOp=VK_STENCIL_OP_KEEP;
           stencil.depthFailOp=VK_STENCIL_OP_KEEP;stencil.compareOp=VK_COMPARE_OP_EQUAL;
           stencil.compareMask=0xff;stencil.writeMask=0;stencil.reference=1;
           ds.front=stencil;ds.back=stencil;
           VkGraphicsPipelineCreateInfo stencilVariant=variant;
           stencilVariant.pDepthStencilState=&ds;
           if(d.createPipelines(device,cache,1,&stencilVariant,alloc,&masked)!=VK_SUCCESS)
            masked=VK_NULL_HANDLE;
           if(masked)m830CreateCropVariants(device,cache,masked,stencilVariant,infos[i].pViewportState,alloc,d);
          }
          m830CreateCropVariants(device,cache,replacement,variant,infos[i].pViewportState,alloc,d);
          std::lock_guard<std::mutex> guard(mu);
          m89Variants[out[i]]={device,replacement,shadow,masked,std::move(compatibility)};
          if(masked)m818MaskedVariantsByPass[infos[i].renderPass]++;
        }
        if(m89VariantReports.fetch_add(1,std::memory_order_relaxed)<40) {
          char msg[200];sprintf_s(msg,"M8.9 VARIANT created=%u VkResult=%d originalPassHash=%llu",
            replacement?1u:0u,int(vr),
            static_cast<unsigned long long>(std::hash<VkRenderPass>{}(infos[i].renderPass)));
          log(msg);
        }
      }
    }
  };

  // M8.6: inventory EVERY graphics pipeline, including pass-through creation.
  // The previous early return skipped the entire M5.3 pipeline inventory.
  // This is read-only: never modify application pipelines in inventory mode.
  if (infos && count) {
    for (uint32_t i=0;i<count;++i) {
      if (infos[i].renderPass==VK_NULL_HANDLE) continue;
      bool first=false, shadow=false;
      {
        std::lock_guard<std::mutex> guard(mu);
        const auto hash=static_cast<unsigned long long>(std::hash<VkRenderPass>{}(infos[i].renderPass));
        first=m53PipelinePasses.size()<256 && m53PipelinePasses.insert(hash).second;
        shadow=shadowPasses.find(infos[i].renderPass)!=shadowPasses.end();
      }
      if (first) {
        char msg[256];
        sprintf_s(msg,"M8.6 PIPELINE originalPassHash=%llu subpass=%u shadowAsOriginal=%u",
          static_cast<unsigned long long>(std::hash<VkRenderPass>{}(infos[i].renderPass)),
          infos[i].subpass,shadow?1u:0u);
        log(msg);
      }
    }
  }
  if (!modifyVrs && !cropEnabled()) {
    VkResult result=d.createPipelines(device,cache,count,infos,alloc,out);
    if(out) {
      std::lock_guard<std::mutex> guard(mu);
      for(uint32_t i=0;i<count;++i)if(out[i] && infos[i].renderPass)
        m87Pipelines[out[i]]={device,infos[i].renderPass,infos[i].subpass};
    }
    m89CreateVariants(result);
    m838CreateVariants(result);
    return result;
  }

  std::vector<VkGraphicsPipelineCreateInfo> modified(infos, infos + count);
  std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> states(count);
  viewportStates.resize(count);
  std::vector<std::vector<VkRect2D>> staticScissors(count);
  uint32_t cropChanged = 0;
  uint32_t changed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    // Static scissors can be clipped at pipeline creation. Dynamic scissors are
    // handled by vkCmdSetScissor during matching eye render passes.
    if (cropEnabled() && infos[i].pViewportState && infos[i].pViewportState->pScissors) {
      bool dynamicScissor = false;
      if (infos[i].pDynamicState)
        for (uint32_t k=0;k<infos[i].pDynamicState->dynamicStateCount;++k)
          if (infos[i].pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_SCISSOR ||
              infos[i].pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT)
            dynamicScissor = true;
      const auto* vp = infos[i].pViewportState;
      if (!dynamicScissor && vp->scissorCount && vp->scissorCount <= 16) {
        staticScissors[i].assign(vp->pScissors, vp->pScissors + vp->scissorCount);
        bool eligible = false;
        for (auto& r : staticScissors[i]) {
          if (r.extent.width == m813Width() && r.extent.height == m813Height() &&
              r.offset.x == 0 && r.offset.y == 0) {
            r = intersectRect(r, cropRect(m813Width(),m813Height()));
            eligible = true;
          }
        }
        if (eligible) {
          viewportStates[i] = *vp;
          viewportStates[i].pScissors = staticScissors[i].data();
          modified[i].pViewportState = &viewportStates[i];
          ++cropChanged;
        }
      }
    }
    if (!modifyVrs) continue;
    // Respect existing application VRS configuration; do not chain duplicate structs.
    if (findChain(infos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR))
      continue;
    auto& state = states[i];
    state.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
    state.pNext = infos[i].pNext;
    state.fragmentSize = uniform ? VkExtent2D{4, 4} : VkExtent2D{1, 1};
    state.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    // REPLACE selects the shading-rate attachment's texel rate when present.
    // In the absence of an attachment, the Vulkan default rate remains 1x1.
    state.combinerOps[1] = uniform ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
                                   : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    modified[i].pNext = &state;
    ++changed;
  }
  VkResult result = d.createPipelines(device, cache, count, modified.data(), alloc, out);
  if(out) {
    std::lock_guard<std::mutex> guard(mu);
    for(uint32_t i=0;i<count;++i)if(out[i] && infos[i].renderPass)
      m87Pipelines[out[i]]={device,infos[i].renderPass,infos[i].subpass};
  }
  if(result==VK_SUCCESS && out)for(uint32_t i=0;i<count;++i)
    m830CreateCropVariants(device,cache,out[i],modified[i],infos[i].pViewportState,alloc,d);
  m89CreateVariants(result);
  m838CreateVariants(result);
  if (cropChanged) {
    char message[160];
    sprintf_s(message,"M6.2 static-scissor pipelines cropped: %u/%u",cropChanged,count);
    log(message);
  }
  static unsigned logCount = 0;
  if (logCount++ < 32) {
    char message[180];
    sprintf_s(message, "M5 pipelines: %s for %u/%u, VkResult=%d",
              uniform ? "uniform 4x4" : "attachment combiner REPLACE (image NOT BOUND)",
              changed, count, static_cast<int>(result));
    log(message);
  }
  return result;
}


// M4 diagnostic hooks. No render-pass, image, or command data is modified.
static DeviceDispatch lookupDevice(VkDevice device) {
 std::lock_guard<std::mutex> guard(mu);
 auto it = devices.find(device);
 return it == devices.end() ? DeviceDispatch{} : it->second;
}
static DeviceDispatch lookupCommand(VkCommandBuffer cmd) {
 std::lock_guard<std::mutex> guard(mu);
 auto it = commandDevices.find(cmd);
 if (it == commandDevices.end()) return {};
 auto d = devices.find(it->second);
 return d == devices.end() ? DeviceDispatch{} : d->second;
}
#include "early_vrs_hooks.inc"

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
 VkDevice device, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* alloc, VkRenderPass* out) {
 auto d = lookupDevice(device);
 if (!d.createRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
 if (ci) {
  char b[240];
  sprintf_s(b, "M4 renderpass1: attachments=%u subpasses=%u dependencies=%u",
   ci->attachmentCount, ci->subpassCount, ci->dependencyCount);
  if (ci->subpassCount > 1) diagnosticLog(b);
 }
 VkResult result = d.createRenderPass(device, ci, alloc, out);
 if (result == VK_SUCCESS && ci && out) {
  if(m835Enabled())m835Profiler.pass(device,*out,*ci);
  PassInfo p{ci->attachmentCount, ci->subpassCount, 0, 0, false, false};
  if (ci->subpassCount == 1 && ci->pSubpasses) {
   const auto& sp = ci->pSubpasses[0];
   p.colors = sp.colorAttachmentCount;
   p.hasDepth = sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;
   bool singleSample = true;
   for (uint32_t j = 0; j < sp.colorAttachmentCount; ++j) {
    uint32_t a = sp.pColorAttachments[j].attachment;
    if (a != VK_ATTACHMENT_UNUSED && a < ci->attachmentCount && ci->pAttachments[a].samples != VK_SAMPLE_COUNT_1_BIT) singleSample = false;
   }
   p.samples = singleSample ? 1 : 0;
   p.eligible = sp.colorAttachmentCount > 0 && singleSample;
  }
  if(m838Enabled() && !ci->pNext && ci->subpassCount==1 && ci->pSubpasses && ci->pAttachments){
    const auto& sp=ci->pSubpasses[0];
    std::lock_guard<std::mutex> guard(mu);
    m838PassDevices[*out]=device;
    if(sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment<ci->attachmentCount)
      m838DepthSlots[*out]=sp.pDepthStencilAttachment->attachment;
    if(sp.colorAttachmentCount==1 && sp.pColorAttachments && sp.pColorAttachments[0].attachment<ci->attachmentCount &&
       sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment<ci->attachmentCount &&
       !sp.inputAttachmentCount && !sp.preserveAttachmentCount && !sp.pResolveAttachments &&
       ci->pAttachments[sp.pColorAttachments[0].attachment].format==VK_FORMAT_R16G16B16A16_UNORM &&
       ci->pAttachments[sp.pColorAttachments[0].attachment].samples==VK_SAMPLE_COUNT_1_BIT &&
       ci->pAttachments[sp.pDepthStencilAttachment->attachment].samples==VK_SAMPLE_COUNT_1_BIT){
      m838UnormPasses.insert(*out);m833ColorSlots[*out]=sp.pColorAttachments[0].attachment;
    }
  }
  // Shadow creation is opt-in and never changes BeamNG's original handles.
  // Vulkan render-pass VRS requires VkRenderPassFragmentDensityMapCreateInfoEXT? No:
  // VkFragmentShadingRateAttachmentInfoKHR is chained to VkSubpassDescription2,
  // so use render-pass 2 with a fully translated single-subpass description.
  // M5.6 rejection bitmask: 1=disabled/unsupported, 2=not eligible,
  // 4=multi/missing subpass, 8=input, 16=preserve, 32=resolve,
  // 64=unsupported pNext, 128=missing attachments, 256=invalid dependencies.
  uint32_t reject = 0;
  if (!(m54ShadowEnabled() || m8Enabled()) || !d.vrsEnabled || !d.createRenderPass2 || !d.destroyRenderPass) reject |= 1;
  if (!p.eligible) reject |= 2;
  if (ci->subpassCount != 1 || !ci->pSubpasses) reject |= 4;
  if (!ci->pAttachments) reject |= 128;
  if (ci->pNext) reject |= 64;
  if (ci->subpassCount == 1 && ci->pSubpasses) {
    const auto& sub = ci->pSubpasses[0];
    if (sub.inputAttachmentCount) reject |= 8;
    if (sub.preserveAttachmentCount) reject |= 16;
    if (sub.pResolveAttachments) reject |= 32;
  }
  if (ci->dependencyCount && !ci->pDependencies) reject |= 256;
  if (reject == 0) {
    const auto& old = ci->pSubpasses[0];
    std::vector<VkAttachmentDescription2> attachments(ci->attachmentCount + 1);
    for (uint32_t j=0;j<ci->attachmentCount;++j) {
      const auto& a=ci->pAttachments[j];
      attachments[j]={VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,nullptr,a.flags,a.format,a.samples,
        a.loadOp,a.storeOp,a.stencilLoadOp,a.stencilStoreOp,a.initialLayout,a.finalLayout};
    }
    attachments.back()={VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,nullptr,0,VK_FORMAT_R8_UINT,
      VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_DONT_CARE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
      VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
      VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR};
    std::vector<VkAttachmentReference2> colors(old.colorAttachmentCount);
    for (uint32_t j=0;j<old.colorAttachmentCount;++j)
      colors[j]={VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,nullptr,old.pColorAttachments[j].attachment,
        old.pColorAttachments[j].layout,VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    if (old.pDepthStencilAttachment) {
      depth.attachment=old.pDepthStencilAttachment->attachment;
      depth.layout=old.pDepthStencilAttachment->layout;
      depth.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkAttachmentReference2 shadingRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,nullptr,
      ci->attachmentCount,VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,0};
    VkFragmentShadingRateAttachmentInfoKHR shading{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
      nullptr,&shadingRef,{16,16}};
    VkSubpassDescription2 sub{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,&shading,old.flags,
      old.pipelineBindPoint,0,0,nullptr,old.colorAttachmentCount,colors.data(),nullptr,
      old.pDepthStencilAttachment ? &depth : nullptr,0,nullptr};
    // Translate legacy VkSubpassDependency to VkSubpassDependency2 without
    // discarding synchronization. Shadow passes remain unbound in M5.6.
    std::vector<VkSubpassDependency2> dependencies(ci->dependencyCount);
    for (uint32_t j=0; j<ci->dependencyCount; ++j) {
      const auto& dep = ci->pDependencies[j];
      dependencies[j] = {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr,
        dep.srcSubpass, dep.dstSubpass, dep.srcStageMask, dep.dstStageMask,
        dep.srcAccessMask, dep.dstAccessMask, dep.dependencyFlags, 0};
    }
    VkRenderPassCreateInfo2 create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,nullptr,ci->flags,
      static_cast<uint32_t>(attachments.size()),attachments.data(),1,&sub,
      static_cast<uint32_t>(dependencies.size()),dependencies.empty() ? nullptr : dependencies.data(),0,nullptr};
    VkRenderPass shadow=VK_NULL_HANDLE;
    VkResult sr=d.createRenderPass2(device,&create,alloc,&shadow);
    static std::atomic<unsigned> shadowPassReports{0};
    if(sr!=VK_SUCCESS || shadowPassReports.fetch_add(1,std::memory_order_relaxed)<12){
      char msg[240]{};sprintf_s(msg,"M5.4 shadow pass creation result=%d attachments=%u",int(sr),create.attachmentCount);
      log(msg);
    }
    if (sr==VK_SUCCESS && shadow) {
      std::lock_guard<std::mutex> guard(mu);
      shadowPasses[*out]={device,shadow};
      m832ShadowSignatures[shadow]=m88Signature(*ci);
      if(m837Tracking() && old.colorAttachmentCount==1 && old.pColorAttachments &&
         old.pColorAttachments[0].attachment!=VK_ATTACHMENT_UNUSED)
        m833ColorSlots[*out]=old.pColorAttachments[0].attachment;
      if(m837Enabled() && old.colorAttachmentCount==1 && old.pColorAttachments &&
         old.pColorAttachments[0].attachment<ci->attachmentCount && old.pDepthStencilAttachment &&
         old.pDepthStencilAttachment->attachment<ci->attachmentCount &&
         ci->pAttachments[old.pColorAttachments[0].attachment].format==VK_FORMAT_R16G16B16A16_SFLOAT)
        m837ScenePasses.insert(*out);
    }
  }
  std::lock_guard<std::mutex> guard(mu);
  if(m818Enabled() && ci->subpassCount==1 && ci->pSubpasses && ci->pAttachments){
   const auto& sub=ci->pSubpasses[0];
   if(sub.pDepthStencilAttachment){
    const auto ix=sub.pDepthStencilAttachment->attachment;
    if(ix!=VK_ATTACHMENT_UNUSED && ix<ci->attachmentCount){
     const auto& att=ci->pAttachments[ix];
     const bool stencil=att.format==VK_FORMAT_S8_UINT ||
       att.format==VK_FORMAT_D16_UNORM_S8_UINT || att.format==VK_FORMAT_D24_UNORM_S8_UINT ||
       att.format==VK_FORMAT_D32_SFLOAT_S8_UINT;
     const bool writable=sub.pDepthStencilAttachment->layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
       sub.pDepthStencilAttachment->layout==VK_IMAGE_LAYOUT_GENERAL;
     const bool disposable=att.stencilLoadOp!=VK_ATTACHMENT_LOAD_OP_LOAD;
     m818StencilCapable[*out]=stencil&&writable&&disposable;
     m819DepthFormatByPass[*out]=att.format;
     if(stencil)m819StencilPasses.fetch_add(1,std::memory_order_relaxed);
     else m819DepthOnlyPasses.fetch_add(1,std::memory_order_relaxed);
    }
   }
  }
  if(m818Enabled() && !m819DepthFormatByPass.count(*out))
   m819NoDepthPasses.fetch_add(1,std::memory_order_relaxed);
  passInfos[*out] = p;
  if(ci->pAttachments && ci->pSubpasses && !ci->pNext) m88PassSignatures[*out]=m88Signature(*ci);
  shadowRejectFlags[*out] = reject;
 }
 return result;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
 VkDevice device, VkRenderPass pass, const VkAllocationCallbacks* alloc) {
 if(m835Enabled())m835Profiler.forgetPass(device,pass);
 std::lock_guard<std::mutex> reuseGuard(m823CreateMutex);
 auto d=lookupDevice(device);
 VkRenderPass shadow=VK_NULL_HANDLE;
 {
   std::lock_guard<std::mutex> guard(mu);
   auto it=shadowPasses.find(pass);
   if (it!=shadowPasses.end()) { shadow=it->second.shadow; m832ShadowSignatures.erase(shadow); shadowPasses.erase(it); }
   passInfos.erase(pass);
   m833ColorSlots.erase(pass);m837ScenePasses.erase(pass);m838UnormPasses.erase(pass);m838DepthSlots.erase(pass);m838PassDevices.erase(pass);
   if(m833Enabled())for(auto& [owner,tracker]:m833Trackers)tracker.erasePass(m833Id(pass));
   if(m837Enabled())for(auto& [owner,tracker]:m837Trackers)tracker.erasePass(m833Id(pass));
   m818StencilCapable.erase(pass);
   m819DepthFormatByPass.erase(pass);
   m818MaskedVariantsByPass.erase(pass);
   m88PassSignatures.erase(pass);
   shadowRejectFlags.erase(pass);
 }
 for(auto id:m834FramebufferCache.invalidate([&](const auto& k){return k.device==m833Id(device) && k.pass==m833Id(shadow);}))
  if(d.destroyFramebuffer)d.destroyFramebuffer(device,reinterpret_cast<VkFramebuffer>(uintptr_t(id)),nullptr);
 if (shadow && d.destroyRenderPass) d.destroyRenderPass(device,shadow,alloc);
 if (d.destroyRenderPass) d.destroyRenderPass(device,pass,alloc);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2(
 VkDevice device, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* alloc, VkRenderPass* out) {
 auto d = lookupDevice(device);
 if (!d.createRenderPass2) return VK_ERROR_INITIALIZATION_FAILED;
 if (ci) {
  char b[240];
  sprintf_s(b, "M4 renderpass2: attachments=%u subpasses=%u correlatedMasks=%u",
   ci->attachmentCount, ci->subpassCount, ci->correlatedViewMaskCount);
  diagnosticLog(b);
  for (uint32_t i = 0; i < ci->subpassCount && i < 8; ++i) {
   sprintf_s(b, "M4 subpass2[%u]: viewMask=0x%08X colorAttachments=%u",
    i, ci->pSubpasses[i].viewMask, ci->pSubpasses[i].colorAttachmentCount);
   diagnosticLog(b);
  }
 }
 const auto result=d.createRenderPass2(device,ci,alloc,out);
 if(m835Enabled() && result==VK_SUCCESS && ci && out)m835Profiler.pass(device,*out,*ci);
 return result;
}
// M7.4g diagnostic: track image views without changing Vulkan behavior.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
 VkDevice device, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* alloc, VkImageView* out) {
 auto d = lookupDevice(device);
 if (!d.createImageView) return VK_ERROR_INITIALIZATION_FAILED;
 VkResult r = d.createImageView(device, ci, alloc, out);
 if(m835Enabled() && r==VK_SUCCESS && ci && out)m835Profiler.resources([&](auto& t){t.createView(device,*out,*ci);});
 if (r == VK_SUCCESS && ci && out) {
   bool report = false;
   {
     std::lock_guard<std::mutex> guard(mu);
     m74gViews[*out] = {device, ci->image};
     if(m837Tracking() && !ci->pNext && ci->viewType==VK_IMAGE_VIEW_TYPE_2D &&
        ci->subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT &&
        ci->subresourceRange.baseMipLevel==0 && ci->subresourceRange.levelCount==1 &&
        ci->subresourceRange.baseArrayLayer==0 && ci->subresourceRange.layerCount==1)
       m833ColorViews.insert(*out);
     if(m838Enabled() && !ci->pNext && ci->viewType==VK_IMAGE_VIEW_TYPE_2D &&
        (ci->subresourceRange.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT) &&
        !(ci->subresourceRange.aspectMask&~(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)) &&
        ci->subresourceRange.baseMipLevel==0 && ci->subresourceRange.levelCount==1 &&
        ci->subresourceRange.baseArrayLayer==0 && ci->subresourceRange.layerCount==1)
       m838DepthViews.insert(*out);
     report = m817Verbose() && m74gLogCount++ < 2500;
   }
   if (report) {
     char msg[256]{};
     sprintf_s(msg, "M7.4g imageView=%p VkImage=%p format=%u viewType=%u layers=%u baseLayer=%u",
       (void*)*out, (void*)ci->image, (unsigned)ci->format,
       (unsigned)ci->viewType, ci->subresourceRange.layerCount,
       ci->subresourceRange.baseArrayLayer);
     log(msg);
   }
 }
 return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
 VkDevice device, VkImageView view, const VkAllocationCallbacks* alloc) {
 if(m835Enabled())m835Profiler.resources([&](auto& t){t.destroyView(device,view);});
 std::lock_guard<std::mutex> reuseGuard(m823CreateMutex);
 auto d = lookupDevice(device);
 for(auto id:m834FramebufferCache.invalidate([&](const auto& k){return k.device==m833Id(device) &&
  (k.rateView==m833Id(view) || std::find(k.views.begin(),k.views.end(),m833Id(view))!=k.views.end());}))
  if(d.destroyFramebuffer)d.destroyFramebuffer(device,reinterpret_cast<VkFramebuffer>(uintptr_t(id)),nullptr);
 {
   std::lock_guard<std::mutex> guard(mu);
   m74gViews.erase(view);
   m833ColorViews.erase(view);m838DepthViews.erase(view);
 }
 if (d.destroyImageView) d.destroyImageView(device, view, alloc);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
 VkDevice device, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
 auto d = lookupDevice(device);
 if (!d.createFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
 m831RefreshCropTarget();
 if(ci && m8Enabled()){
  uint32_t xrW=0,xrH=0;
  if(m814Target(xrW,xrH) && ci->width==xrW && ci->height==xrH && ci->layers==1 &&
     !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
    m814EnsureResources(device,xrW,xrH);
 }
 VkResult r = d.createFramebuffer(device, ci, alloc, out);
 if(m835Enabled() && r==VK_SUCCESS && ci && out)m835Profiler.resources([&](auto& t){t.framebuffer(device,*out,*ci);});
 if (r == VK_SUCCESS && ci && out) {
  m823Stats.originals.fetch_add(1,std::memory_order_relaxed);
  std::lock_guard<std::mutex> guard(mu);
  framebuffers[*out] = {ci->width, ci->height, ci->layers, ci->attachmentCount};
  if (ci->width == m813Width() && ci->height == m813Height() && ci->pAttachments &&
      !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
    M74dAttachmentSet entry{};
    entry.pass = ci->renderPass;
    entry.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    m74dAttachments[*out] = std::move(entry);
  }

 }
 // M5.5: create but NEVER bind shadow framebuffers; diagnostic only.
 // The VRS image view is already in the correct layout after the upload fence.
 if (r == VK_SUCCESS && ci && out && (m55FramebufferEnabled() || m8Enabled()) &&
     ci->width == m813Width() && ci->height == m813Height() && ci->layers == 1 &&
     !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments) {
   VkRenderPass shadowPass = VK_NULL_HANDLE;
   VkImageView rateView = VK_NULL_HANDLE;
   bool passFound = false, passDeviceMatch = false, imageFound = false;
   bool imageViewPresent = false;
   size_t shadowPassCount = 0, uploadedImageCount = 0;
   {
     std::lock_guard<std::mutex> guard(mu);
     auto passIt = shadowPasses.find(ci->renderPass);
     auto imageIt = uploadedPatterns.find(device);
     shadowPassCount = shadowPasses.size();
     uploadedImageCount = uploadedPatterns.size();
     passFound = passIt != shadowPasses.end();
     passDeviceMatch = passFound && passIt->second.device == device;
     imageFound = imageIt != uploadedPatterns.end();
     imageViewPresent = imageFound && imageIt->second.view != VK_NULL_HANDLE;
     if (passDeviceMatch) shadowPass = passIt->second.shadow;
     if (imageViewPresent) rateView = imageIt->second.view;
   }
   {
     static std::atomic<unsigned> reports{0};
     if (m817Verbose() && reports.fetch_add(1, std::memory_order_relaxed) < 24) {
       char diag[384];
       sprintf_s(diag,
         "M5.5.1 lookup size=%ux%u attachments=%u passFound=%d passDeviceMatch=%d imageFound=%d imageView=%d shadowPassCount=%llu uploadedImageCount=%llu passHash=%llu",
         ci->width, ci->height, ci->attachmentCount,
         int(passFound), int(passDeviceMatch), int(imageFound), int(imageViewPresent),
         static_cast<unsigned long long>(shadowPassCount),
         static_cast<unsigned long long>(uploadedImageCount),
         static_cast<unsigned long long>(std::hash<VkRenderPass>{}(ci->renderPass)));
       log(diag);
     }
   }
   if (!passFound) {
     uint32_t rejectFlags = 0xFFFFFFFFu;
     bool hasRejectRecord = false;
     {
       std::lock_guard<std::mutex> guard(mu);
       auto it = shadowRejectFlags.find(ci->renderPass);
       hasRejectRecord = it != shadowRejectFlags.end();
       if (hasRejectRecord) rejectFlags = it->second;
     }
     static std::atomic<unsigned> reasonReports{0};
     if (m817Verbose() && reasonReports.fetch_add(1, std::memory_order_relaxed) < 40) {
       char msg[300];
       sprintf_s(msg, "M5.6 VR pass missing shadow: passHash=%llu recorded=%d rejectMask=0x%X (1=feature 2=eligible 4=subpass 8=input 10=preserve 20=resolve 40=pNext 80=attachments 100=dependencies)",
         static_cast<unsigned long long>(std::hash<VkRenderPass>{}(ci->renderPass)),
         int(hasRejectRecord), rejectFlags);
       log(msg);
     }
   }
   if (shadowPass && rateView) {
     std::vector<VkImageView> views(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
     views.push_back(rateView);
     VkFramebufferCreateInfo variant = *ci;
     variant.renderPass = shadowPass;
     variant.attachmentCount = static_cast<uint32_t>(views.size());
     variant.pAttachments = views.data();
     // M8.16: the gaze-following path ONLY selects eyeFbs[eye] from
     // m83EyeFramebuffers. The old base framebuffer was used as a presence
     // marker, while four spare variants were allocated but never bound.
     // Stop creating all five unused VkFramebuffers for the eye-tracked path.
     if (m8Enabled()) {
       m823Stats.eligible.fetch_add(1,std::memory_order_relaxed);
       // Only safe to copy a legacy VkFramebufferCreateInfo without pNext.
       // For all other cases, retain the proven eager M8.19 path.
       if(m823LazyEnabled() && !ci->pNext){
        M823LazyFramebuffer plan{};
        plan.device=device;plan.shadowPass=shadowPass;
        plan.flags=ci->flags;plan.width=ci->width;
        plan.height=ci->height;plan.layers=ci->layers;
        plan.views.assign(ci->pAttachments,ci->pAttachments+ci->attachmentCount);
        if(alloc){plan.hasAllocator=true;plan.allocator=*alloc;}
        {
         std::lock_guard<std::mutex> guard(mu);
         m823LazyPlans[*out]=std::move(plan);
         m83EyeFramebuffers[*out]={device,{VK_NULL_HANDLE,VK_NULL_HANDLE}};
         shadowFramebuffers[*out]={device,VK_NULL_HANDLE};
        }
        m823Stats.lazyPlans.fetch_add(1,std::memory_order_relaxed);
       }else{
       std::array<VkImageView,2> eyeViews{};
       {std::lock_guard<std::mutex> guard(mu);
        auto it=m83EyePatterns.find(device);
        if(it!=m83EyePatterns.end())for(int eye=0;eye<2;++eye)eyeViews[eye]=it->second[eye].view;
       }
       std::array<VkFramebuffer,3> eyeFbs{};
       for(int eye=0;eye<2;++eye) {
         if(!eyeViews[eye])continue;
         views.back()=eyeViews[eye];
         const VkResult result=d.createFramebuffer(device,&variant,alloc,&eyeFbs[eye]);
         if(result!=VK_SUCCESS)eyeFbs[eye]=VK_NULL_HANDLE;
       }
       if(eyeFbs[0] && eyeFbs[1]) {
         std::lock_guard<std::mutex> guard(mu);
         m83EyeFramebuffers[*out]={device,eyeFbs};
         // Existing m8TryShadowBegin uses this map as a gate, then overwrites
         // its framebuffer with the appropriate per-eye framebuffer. Null is
         // an intentional sentinel, NOT a Vulkan object to destroy.
         shadowFramebuffers[*out]={device,VK_NULL_HANDLE};
       } else {
         for(auto f:eyeFbs)if(f)d.destroyFramebuffer(device,f,alloc);
         static std::atomic<unsigned> failures{0};
         if(failures.fetch_add(1,std::memory_order_relaxed)<12)
           log("M8.16 EYE_FRAMEBUFFER_CREATE_FAILED; original render pass preserved");
       }
       if(eyeFbs[0] && eyeFbs[1])m823Stats.eagerVariants.fetch_add(2,std::memory_order_relaxed);
       } // eager fallback
       const uint64_t count=m823Stats.eligible.load(std::memory_order_relaxed);
       if(count==1 || count%8192==0){
         char report[320]{};
         sprintf_s(report,"M8.23 FB_STATS eligible=%llu created=%llu destroyed=%llu lazyPlans=%llu eagerVariants=%llu lazyVariants=%llu selectedPasses=%llu lazyFailures=%llu",
           (unsigned long long)count,
           (unsigned long long)m823Stats.originals.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.destroyed.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.lazyPlans.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.eagerVariants.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.lazyVariants.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.shadowSelections.load(std::memory_order_relaxed),
           (unsigned long long)m823Stats.lazyFailures.load(std::memory_order_relaxed));
         log(report);
         m834Report();
       }
     } else {
       // Preserve the legacy non-eye shadow framebuffer mode unchanged.
       VkFramebuffer shadowFb=VK_NULL_HANDLE;
       const VkResult shadowResult=d.createFramebuffer(device,&variant,alloc,&shadowFb);
       static std::atomic<unsigned> reports{0};
       if(shadowResult!=VK_SUCCESS || reports.fetch_add(1,std::memory_order_relaxed)<12){
         char msg[256]{};
         sprintf_s(msg,"M5.5 shadow framebuffer result=%d size=%ux%u attachments=%u",
           int(shadowResult),ci->width,ci->height,variant.attachmentCount);
         log(msg);
       }
       if(shadowResult==VK_SUCCESS && shadowFb){
         std::lock_guard<std::mutex> guard(mu);
         shadowFramebuffers[*out]={device,shadowFb};
       }
     }
   } else {
     static std::atomic<unsigned> skipped{0};
     if (skipped.fetch_add(1, std::memory_order_relaxed) < 8)
       log("M5.5 shadow framebuffer skipped: no matching shadow render pass or uploaded image view");
   }
 }
 if (r == VK_SUCCESS && ci && (ci->width == m813Width() && ci->height == m813Height() || ci->width == 1680 && ci->height == 1050)) {
  const auto passHash = static_cast<unsigned long long>(std::hash<VkRenderPass>{}(ci->renderPass));
  const auto pairKey = passHash ^ (static_cast<unsigned long long>(ci->width) << 32) ^ ci->height;
  bool first = false;
  PassInfo pass{}; bool known = false;
  {
    std::lock_guard<std::mutex> guard(mu);
    first = m53FramebufferPairs.size() < 256 && m53FramebufferPairs.insert(pairKey).second;
    auto it = passInfos.find(ci->renderPass);
    if (it != passInfos.end()) { pass = it->second; known = true; }
  }
  if (first && m817Verbose()) {
    char msg[320];
    sprintf_s(msg, "M5.3 FRAMEBUFFER renderPassHash=%llu size=%ux%u attachments=%u passKnown=%u eligible=%u",
      passHash, ci->width, ci->height, ci->attachmentCount, known ? 1u : 0u, pass.eligible ? 1u : 0u);
    log(msg);
  }
 }
 if (r == VK_SUCCESS && ci && (ci->width >= 3000 || ci->height >= 3000)) {
  char b[240];
  sprintf_s(b, "M4.1 LARGE framebuffer: %ux%u layers=%u attachments=%u",
   ci->width, ci->height, ci->layers, ci->attachmentCount);
  diagnosticLog(b);
 }
 return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
 VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* alloc) {
 if(m835Enabled())m835Profiler.resources([&](auto& t){t.forgetFramebuffer(device,framebuffer);});
 std::lock_guard<std::mutex> lazyGuard(m823CreateMutex);
 auto d = lookupDevice(device);
 VkFramebuffer shadow = VK_NULL_HANDLE;
 std::array<VkFramebuffer,3> eyeFbs{};
 M812FramebufferPool spareFbs{};
 { std::lock_guard<std::mutex> guard(mu);
   auto poolIt=m812FramebufferPools.find(framebuffer);
   if(poolIt!=m812FramebufferPools.end()){spareFbs=poolIt->second;m812FramebufferPools.erase(poolIt);}
   auto eyeIt=m83EyeFramebuffers.find(framebuffer);
   if(eyeIt!=m83EyeFramebuffers.end()){eyeFbs=eyeIt->second.eyes;m83EyeFramebuffers.erase(eyeIt);}
   if(framebuffers.erase(framebuffer))m823Stats.destroyed.fetch_add(1,std::memory_order_relaxed);
   m823LazyPlans.erase(framebuffer);
   m74dAttachments.erase(framebuffer);
   m74dReported.erase(framebuffer);
   auto it=shadowFramebuffers.find(framebuffer);
   if (it!=shadowFramebuffers.end()) { shadow=it->second.shadow; shadowFramebuffers.erase(it); }
 }
 for(auto f:eyeFbs)if(f && !m834FramebufferCache.release(m833Id(f)) && d.destroyFramebuffer)d.destroyFramebuffer(device,f,alloc);
 for(auto& eye:spareFbs.spare)for(auto f:eye)if(f && d.destroyFramebuffer)d.destroyFramebuffer(device,f,alloc);
 if (shadow && d.destroyFramebuffer) d.destroyFramebuffer(device, shadow, alloc);
 if (d.destroyFramebuffer) d.destroyFramebuffer(device, framebuffer, alloc);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
 VkDevice device, const VkImageCreateInfo* ci, const VkAllocationCallbacks* alloc, VkImage* out) {
 auto d = lookupDevice(device);
 if (!d.createImage) return VK_ERROR_INITIALIZATION_FAILED;
 if (ci && (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
     (ci->extent.width >= 3000 || ci->extent.height >= 3000 || ci->arrayLayers > 1)) {
  unsigned long long key = (static_cast<unsigned long long>(ci->extent.width) << 32) ^
    (static_cast<unsigned long long>(ci->extent.height) << 12) ^ ci->arrayLayers;
  bool first;
  { std::lock_guard<std::mutex> guard(mu); first = seenImageDimensions.insert(key).second; }
  if (first) {
   char b[280];
   sprintf_s(b, "M4.1 large/layered color image: %ux%u layers=%u samples=%u format=%d usage=0x%X",
    ci->extent.width, ci->extent.height, ci->arrayLayers, unsigned(ci->samples),
    int(ci->format), unsigned(ci->usage));
   diagnosticLog(b);
  }
 }
 const auto result=d.createImage(device,ci,alloc,out);
 if(m835Enabled() && result==VK_SUCCESS && ci && out)m835Profiler.resources([&](auto& t){t.createImage(device,*out,*ci);});
 if(m837Tracking() && result==VK_SUCCESS && ci && out && !ci->pNext &&
    ci->imageType==VK_IMAGE_TYPE_2D && ci->extent.depth==1 && ci->arrayLayers==1 &&
    ci->samples==VK_SAMPLE_COUNT_1_BIT && ci->mipLevels==1 &&
    (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
    !(ci->usage & VK_IMAGE_USAGE_STORAGE_BIT)){
  const auto f=ci->format;
  if(f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB ||
     f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB ||
     f==VK_FORMAT_R16G16B16A16_SFLOAT || f==VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
     (m838Enabled() && f==VK_FORMAT_R16G16B16A16_UNORM)){
   std::lock_guard<std::mutex> guard(mu);
   if(m833Enabled())m833Trackers[device].image(m833Id(*out),ci->extent.width,ci->extent.height);
   if(m837Enabled()){
    const bool scene=f==VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool unorm=m838Enabled() && f==VK_FORMAT_R16G16B16A16_UNORM;
    m837Trackers[device].image(m833Id(*out),ci->extent.width,ci->extent.height,scene,unorm);
    if(unorm)m838UnormImages.insert(*out);
    if(scene)m837SceneImages.insert(*out);
   }
  }
 }
 if(m838Enabled() && result==VK_SUCCESS && ci && out && !ci->pNext &&
    ci->imageType==VK_IMAGE_TYPE_2D && ci->extent.depth==1 && ci->arrayLayers==1 &&
    ci->mipLevels==1 && ci->samples==VK_SAMPLE_COUNT_1_BIT && (ci->usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)){
  std::lock_guard<std::mutex> guard(mu);
  m838DepthImages[*out]={device,ci->extent.width,ci->extent.height};m837Trackers[device].depthImage(m833Id(*out));
 }
 return result;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
 VkDevice device, const VkCommandBufferAllocateInfo* ci, VkCommandBuffer* out) {
 auto d = lookupDevice(device);
 if (!d.allocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
 VkResult r = d.allocateCommandBuffers(device, ci, out);
 if (r == VK_SUCCESS && ci && out) {
  std::lock_guard<std::mutex> guard(mu);
  for (uint32_t i=0; i<ci->commandBufferCount; ++i){
   commandDevices[out[i]]=device;
   if(m835Enabled())m835Profiler.allocate(device,ci->commandPool,out[i],ci->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY);
   m837Each(device,[&](auto& t){t.allocateCommand(m833Id(out[i]),m833Id(ci->commandPool),ci->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY);});
  }
 }
 return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
 VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
 auto d = lookupDevice(device);
 if (buffers) {
  std::lock_guard<std::mutex> guard(mu);
  for (uint32_t i=0; i<count; ++i){if(m835Enabled())m835Profiler.reset(buffers[i],true);commandDevices.erase(buffers[i]);m838Scopes.erase(buffers[i]);m838OriginalBindings.erase(buffers[i]);m838BoundCopies.erase(buffers[i]);m833ActiveEarlier.erase(buffers[i]);m837ActiveShared.erase(buffers[i]);
   m837Each(device,[&](auto& t){t.commands.erase(m833Id(buffers[i]));});
  }
 }
 if (d.freeCommandBuffers) d.freeCommandBuffers(device, pool, count, buffers);
}
static void logDynamic(const VkRenderingInfo* info, const char* label) {
 if (!info) return;
 char b[280];
 sprintf_s(b, "M4 %s: extent=%ux%u offset=%d,%d layers=%u viewMask=0x%X colors=%u",
  label, info->renderArea.extent.width, info->renderArea.extent.height,
  info->renderArea.offset.x, info->renderArea.offset.y, info->layerCount,
  info->viewMask, info->colorAttachmentCount);
 diagnosticLog(b);
}
namespace { void m74iTraceDynamic(VkCommandBuffer, const VkRenderingInfo*, const char*); }
// M5.1: actual shading-rate attachment binding for Vulkan dynamic rendering.
// Only exact-size, single-layer VR render areas are eligible; legacy renderpasses
// remain untouched. The texture is persistent and uploaded before use.
static void beginDynamicWithFFR(VkCommandBuffer cmd, const VkRenderingInfo* info,
                                PFN_vkCmdBeginRendering next) {
 if (!next) return;
 if (!info || !attachmentCombinerOptIn() || uniformVrsEnabled() ||
     info->renderArea.offset.x != 0 || info->renderArea.offset.y != 0 ||
     info->renderArea.extent.width != m813Width() || info->renderArea.extent.height != m813Height() ||
     info->layerCount != 1 || info->viewMask != 0 || info->colorAttachmentCount == 0 ||
     findChain(info->pNext, VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR)) {
   next(cmd, info);
   return;
 }
 VkImageView view = VK_NULL_HANDLE;
 bool enabled = false;
 {
   std::lock_guard<std::mutex> guard(mu);
   auto ci = commandDevices.find(cmd);
   if (ci != commandDevices.end()) {
     auto di = devices.find(ci->second);
     auto pi = uploadedPatterns.find(ci->second);
     if (di != devices.end() && di->second.vrsEnabled &&
         pi != uploadedPatterns.end()) {
       view = pi->second.view;
       enabled = view != VK_NULL_HANDLE;
     }
   }
 }
 if (!enabled) { next(cmd, info); return; }
 VkRenderingFragmentShadingRateAttachmentInfoKHR shading{
     VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
 shading.pNext = info->pNext;
 shading.imageView = view;
 shading.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
 shading.shadingRateAttachmentTexelSize = {16, 16};
 VkRenderingInfo modified = *info;
 modified.pNext = &shading;
 static std::atomic<unsigned> logs{0};
 if (logs.fetch_add(1) < 8) log("M5.1 ACTIVE dynamic-rendering FFR attachment bound (5190x4168)");
 next(cmd, &modified);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
 m833Invalidate(cmd);
 if(m835Enabled())m835Profiler.dynamic(cmd);
 auto d = lookupCommand(cmd);
 if(m817Verbose()){ logDynamic(info, "dynamicRendering"); m74iTraceDynamic(cmd, info, "BeginRendering"); }
 beginDynamicWithFFR(cmd, info, d.cmdBeginRendering);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer cmd, const VkRenderingInfo* info) {
 m833Invalidate(cmd);
 if(m835Enabled())m835Profiler.dynamic(cmd);
 auto d = lookupCommand(cmd);
 if(m817Verbose()){ logDynamic(info, "dynamicRenderingKHR"); m74iTraceDynamic(cmd, info, "BeginRenderingKHR"); }
 beginDynamicWithFFR(cmd, info, reinterpret_cast<PFN_vkCmdBeginRendering>(d.cmdBeginRenderingKHR));
}

static void logRenderPassBegin(const VkRenderPassBeginInfo* info, const char* tag) {
 if (!diagnosticEnabled() || !info) return;
 FramebufferInfo fb{};
 bool known = false;
 bool first = false;
 {
  std::lock_guard<std::mutex> guard(mu);
  auto it = framebuffers.find(info->framebuffer);
  if (it != framebuffers.end()) { fb = it->second; known = true; }
  // Deduplicate by framebuffer size, render area, and pass handle; bounded to avoid log flooding.
  unsigned long long key = (static_cast<unsigned long long>(fb.width) << 44) ^
    (static_cast<unsigned long long>(fb.height) << 28) ^
    (static_cast<unsigned long long>(info->renderArea.extent.width) << 12) ^
    static_cast<unsigned long long>(info->renderArea.extent.height) ^
    static_cast<unsigned long long>(std::hash<VkRenderPass>{}(info->renderPass));
  first = seenBeginDimensions.size() < 200 && seenBeginDimensions.insert(key).second;
 }
 if (first) {
  char b[320];
  sprintf_s(b, "M4.1 %s: fb=%ux%u layers=%u attachments=%u known=%u area=%ux%u offset=%d,%d",
   tag, fb.width, fb.height, fb.layers, fb.attachments, known ? 1 : 0,
   info->renderArea.extent.width, info->renderArea.extent.height,
   info->renderArea.offset.x, info->renderArea.offset.y);
  diagnosticLog(b);
  if (fb.width >= 3000 && fb.height >= 3000) {
   PassInfo p{}; bool found = false;
   { std::lock_guard<std::mutex> guard(mu);
     auto it = passInfos.find(info->renderPass);
     if (it != passInfos.end()) { p = it->second; found = true; }
   }
   sprintf_s(b, "M4.3 VR pass candidate: known=%u attachments=%u subpasses=%u colors=%u depth=%u singleSample=%u compatibleCandidate=%u",
    found ? 1u : 0u, p.attachments, p.subpasses, p.colors, p.hasDepth ? 1u : 0u,
    p.samples, (found && p.eligible && fb.layers == 1) ? 1u : 0u);
   diagnosticLog(b);
  }
 }
}
namespace { int m8ClassifiedEye(VkImage image); }
// M8.23: allocate just the classified eye's shadow framebuffer on first use.
// Serialize with resolution changes, destruction, and other first-use calls.
// No framebuffer or VkImageView is ever shared across original object lifetimes.
static bool m823EnsureEyeFramebuffer(VkDevice device,VkFramebuffer original,int eye){
 if(eye<0 || eye>2 || (eye==2 && !m837Enabled()))return false;
 std::lock_guard<std::mutex> resolutionGuard(m814ResourceMutex);
 std::lock_guard<std::mutex> createGuard(m823CreateMutex);
 M823LazyFramebuffer plan{};
 beamng_framebuffer::Key reuseKey{};bool reusable=false;
 DeviceDispatch d{};
 VkImageView rateView=VK_NULL_HANDLE;
 {
  std::lock_guard<std::mutex> guard(mu);
  auto existing=m83EyeFramebuffers.find(original);
  if(existing==m83EyeFramebuffers.end() || existing->second.device!=device)return false;
  if(existing->second.eyes[eye])return true;
  auto it=m823LazyPlans.find(original);
  auto di=devices.find(device);
  auto vrs=m83EyePatterns.find(device);
  if(it==m823LazyPlans.end() || di==devices.end() || vrs==m83EyePatterns.end())return false;
  if(it->second.device!=device || it->second.width!=m813Width() ||
     it->second.height!=m813Height())return false;
  auto base=uploadedPatterns.find(device);
  rateView=eye==2?(base==uploadedPatterns.end()?VK_NULL_HANDLE:base->second.view):vrs->second[eye].view;
  if(it->second.views.empty() || !rateView)return false;
  // A destroyed shadow render pass must not be used to construct a framebuffer.
  bool passAlive=false;
  for(const auto& sp:shadowPasses)if(sp.second.device==device &&
    sp.second.shadow==it->second.shadowPass){passAlive=true;break;}
  if(!passAlive)return false;
  plan=it->second;d=di->second;
  reusable=!plan.hasAllocator;
  for(auto view:plan.views){auto v=m74gViews.find(view);if(v==m74gViews.end() || v->second.device!=device)reusable=false;}
  if(reusable){
   reuseKey={m833Id(device),m833Id(plan.shadowPass),m833Id(rateView),plan.flags,plan.width,plan.height,plan.layers,{}};
   for(auto view:plan.views)reuseKey.views.push_back(m833Id(view));
   if(auto cached=m834FramebufferCache.take(reuseKey)){
    existing->second.eyes[eye]=reinterpret_cast<VkFramebuffer>(uintptr_t(cached));return true;
   }
  }
 }
 plan.views.push_back(rateView);
 VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
 ci.flags=plan.flags;
 ci.renderPass=plan.shadowPass;
 ci.attachmentCount=static_cast<uint32_t>(plan.views.size());
 ci.pAttachments=plan.views.data();
 ci.width=plan.width;ci.height=plan.height;ci.layers=plan.layers;
 const VkAllocationCallbacks* alloc=plan.hasAllocator?&plan.allocator:nullptr;
 VkFramebuffer created=VK_NULL_HANDLE;
 const auto createStart=std::chrono::steady_clock::now();
 const VkResult result=d.createFramebuffer(device,&ci,alloc,&created);
 m834FbCreateNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-createStart).count()),std::memory_order_relaxed);
 m834FbCreateCalls.fetch_add(1,std::memory_order_relaxed);
 if(result!=VK_SUCCESS || !created){
  m823Stats.lazyFailures.fetch_add(1,std::memory_order_relaxed);
  static std::atomic<unsigned> failed{0};
  if(failed.fetch_add(1,std::memory_order_relaxed)<8){
   char msg[160]{};sprintf_s(msg,"M8.23 LAZY_CREATE_FAILED eye=%d result=%d",eye,int(result));log(msg);
  }
  return false;
 }
 bool keep=false;
 {
  std::lock_guard<std::mutex> guard(mu);
  auto it=m83EyeFramebuffers.find(original);
  auto planIt=m823LazyPlans.find(original);
  if(it!=m83EyeFramebuffers.end() && planIt!=m823LazyPlans.end() &&
     it->second.device==device && !it->second.eyes[eye]){
   it->second.eyes[eye]=created;keep=true;
  }
 }
 if(!keep){d.destroyFramebuffer(device,created,alloc);return false;}
 if(reusable)m834FramebufferCache.created(m833Id(created),std::move(reuseKey));
 m823Stats.lazyVariants.fetch_add(1,std::memory_order_relaxed);
 return true;
}
// M5.7: only substitute when the original pass and framebuffer have matching
// shadow counterparts on the same device. This is experimental: pipeline
// render-pass compatibility and secondary command buffers require validation.
// "Shadow" here is our compatible VRS render-pass/framebuffer copy, not a
// lighting shadow map. Preserve the app objects and substitute only a known eye.
static bool m57SelectShadow(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info,
                             VkRenderPassBeginInfo& modified) {
 // M8.11: announce test mode once, and bypass the shadow substitution
 // in baseline mode while keeping every other pipeline/resource path identical.
 m811ReportMode();
 if (m811BaselineEnabled()) return false;
 // M8.5: prevent accidental use of incompatible graphics pipelines.
 // This override is deliberately named UNSAFE; it is not a functioning VRS mode.
 // M8.9 static experiment is explicitly opt-in; legacy unsafe flag remains supported.
 char m89Flag[8]{};
 const bool m89Static = beamng_config::read("BEAMNG_VRS_M89_STATIC_TEST",m89Flag,sizeof(m89Flag)) && strcmp(m89Flag,"1")==0;
 if (m8Enabled() && !m85UnsafeBindEnabled() && !m89Static) return false;
 if (!(m57BindEnabled() || m8Enabled()) || !info || info->pNext ||
     !(m54ShadowEnabled() || m8Enabled()) || !(m55FramebufferEnabled() || m8Enabled())) return false;
 VkDevice device = VK_NULL_HANDLE;
 VkRenderPass pass = VK_NULL_HANDLE;
 VkFramebuffer fb = VK_NULL_HANDLE;
 int selectedEye = -1; // M8.10: preserve eye classification outside mutex scope.
 bool needLazyFramebuffer=false,earlySelection=false,sharedSelection=false;
 {
   std::lock_guard<std::mutex> guard(mu);
   auto cd = commandDevices.find(cmd);
   if (cd == commandDevices.end()) return false;
   device = cd->second;
   if(m838Enabled() && m838UnormPasses.count(info->renderPass))return false;
   auto pi = shadowPasses.find(info->renderPass);
   auto fi = shadowFramebuffers.find(info->framebuffer);
   auto originalFb = framebuffers.find(info->framebuffer);
   // M8: never redirect an unclassified render pass. This is a conservative
   // safety gate; classification is learned from copies to OpenXR eye images.
   bool eyeFound[2]{};
   auto attachments = m74dAttachments.find(info->framebuffer);
   if (attachments != m74dAttachments.end()) {
     for (VkImageView view : attachments->second.views) {
       auto vi = m74gViews.find(view);
       if (vi == m74gViews.end()) continue;
       // Classification map is defined below, so use a helper declaration.
       int eye = m8ClassifiedEye(vi->second.image);
       if (eye >= 0 && eye < 2) eyeFound[eye] = true;
     }
   }
   if(m8Enabled() && !eyeFound[0] && !eyeFound[1]){
    const int earlyEye=m833Eye(device,cmd,info);
    if(earlyEye>=0){eyeFound[earlyEye]=true;earlySelection=true;}
    else sharedSelection=m837Select(device,cmd,info);
   }
   if (m8Enabled() && !sharedSelection && (eyeFound[0] == eyeFound[1])) return false;
   if (pi == shadowPasses.end() || fi == shadowFramebuffers.end() ||
       originalFb == framebuffers.end() ||
       originalFb->second.width != m813Width() || originalFb->second.height != m813Height() ||
       pi->second.device != device || fi->second.device != device) return false;
   pass = pi->second.shadow;
   fb = fi->second.shadow;
   if(m8Enabled()) {
     const int eye=sharedSelection?2:(eyeFound[0]?0:1);
     selectedEye = eye;
     auto ef=m83EyeFramebuffers.find(info->framebuffer);
     if(ef==m83EyeFramebuffers.end() || ef->second.device!=device)return false;
     if(!ef->second.eyes[eye]){
      if(m823LazyPlans.find(info->framebuffer)==m823LazyPlans.end())return false;
      needLazyFramebuffer=true;
     }else fb=ef->second.eyes[eye];
   }
 }
 if(needLazyFramebuffer){
  if(!m823EnsureEyeFramebuffer(device,info->framebuffer,selectedEye))return false;
  std::lock_guard<std::mutex> guard(mu);
  auto it=m83EyeFramebuffers.find(info->framebuffer);
  if(it==m83EyeFramebuffers.end() || it->second.device!=device ||
     !it->second.eyes[selectedEye])return false;
  fb=it->second.eyes[selectedEye];
 }
 m823Stats.shadowSelections.fetch_add(1,std::memory_order_relaxed);
 modified = *info;
 modified.renderPass = pass;
 modified.framebuffer = fb;
 {std::lock_guard<std::mutex> guard(mu);
  if(earlySelection){++m833Trackers[device].stats.selected;m833ActiveEarlier.insert(cmd);}
  m89ActiveShadows[cmd]=pass;
  if(sharedSelection){++m837Trackers[device].stats.selected;m837ActiveShared.insert(cmd);}
  if(selectedEye>=0 && selectedEye<2)m818SelectedEye[cmd]=selectedEye;
 }
 if (m810ShadowReports.fetch_add(1,std::memory_order_relaxed)<64) {
   char msg[256]{};
   sprintf_s(msg,"M8.10 SHADOW_SELECTED eye=%d passHash=%llu framebufferHash=%llu cmdHash=%llu",
     selectedEye,
     static_cast<unsigned long long>(std::hash<VkRenderPass>{}(pass)),
     static_cast<unsigned long long>(std::hash<VkFramebuffer>{}(fb)),
     static_cast<unsigned long long>(std::hash<VkCommandBuffer>{}(cmd)));
   log(msg);
 }
 static std::atomic<unsigned> reported{0};
 if (reported.fetch_add(1, std::memory_order_relaxed) < 32)
   log("M8 EXPERIMENTAL eye-classified VRS render pass + framebuffer substitution (pipeline compatibility unverified)");
 return true;
}
// M8.18: initialize and stamp the VISIBLE part of the runtime mask in stencil.
// This uses 2 vkCmdClearAttachments calls (one whole-pass 0, one batched strips 1).
// Depth values are not touched. Only existing stencil-capable eye passes qualify.
static void m818BeginStencil(VkCommandBuffer cmd,const VkRenderPassBeginInfo* original,
                              bool useShadow,const DeviceDispatch& d){
 if(!m818Enabled() || !useShadow || !original || !d.cmdClearAttachments)return;
 // vkCmdClearAttachments must be recorded within an INLINE subpass.
 // And the raster strips are derived from a full-size eye viewport.
 if(original->renderArea.offset.x || original->renderArea.offset.y ||
    original->renderArea.extent.width!=m813Width() ||
    original->renderArea.extent.height!=m813Height())return;
 int eye=-1;bool hasStencil=false,hasPipeline=false;
 {
  std::lock_guard<std::mutex> guard(mu);
  const auto e=m818SelectedEye.find(cmd);
  if(e!=m818SelectedEye.end())eye=e->second;
  const auto p=m818StencilCapable.find(original->renderPass);
  const auto v=m818MaskedVariantsByPass.find(original->renderPass);
  hasStencil=p!=m818StencilCapable.end()&&p->second;
  hasPipeline=v!=m818MaskedVariantsByPass.end() && v->second>0;
 }
 if(eye<0)return;
 if(!hasStencil || !hasPipeline){
  if(m818NoStencilCount.fetch_add(1,std::memory_order_relaxed)==0){
   VkFormat fmt=VK_FORMAT_UNDEFINED;
   unsigned depthOnly=m819DepthOnlyPasses.load(std::memory_order_relaxed);
   unsigned stencil=m819StencilPasses.load(std::memory_order_relaxed);
   unsigned noDepth=m819NoDepthPasses.load(std::memory_order_relaxed);
   {std::lock_guard<std::mutex> guard(mu);
    const auto f=m819DepthFormatByPass.find(original->renderPass);
    if(f!=m819DepthFormatByPass.end())fmt=f->second;
   }
   char message[256]{};
   sprintf_s(message,"M8.19 MASK_SKIPPED reason=%s eyePassDepthFormat=%u knownPassesDepthOnly=%u stencil=%u noDepth=%u (VRS unchanged)",
    hasStencil?"NO_COMPATIBLE_STENCIL_PIPELINE":"NO_WRITABLE_STENCIL_ATTACHMENT",
    unsigned(fmt),depthOnly,stencil,noDepth);
   log(message);
  }
  return;
 }
 std::vector<VkClearRect> rectangles;
 if(!m818VisibilityRects(eye,m813Width(),m813Height(),rectangles)){
  if(m818NoMeshCount.fetch_add(1,std::memory_order_relaxed)==0)
   log("M8.18 MASK_SKIPPED reason=OPENXR_MESH_UNAVAILABLE (VRS unchanged)");
  return;
 }
 VkClearAttachment clear{};
 clear.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
 clear.clearValue.depthStencil={1.f,0};
 VkClearRect full{original->renderArea,0,1};
 d.cmdClearAttachments(cmd,1,&clear,1,&full);
 clear.clearValue.depthStencil.stencil=1;
 d.cmdClearAttachments(cmd,1,&clear,(uint32_t)rectangles.size(),rectangles.data());
 {
  std::lock_guard<std::mutex> guard(mu);
  m818MaskedCommands.insert(cmd);
 }
 const auto count=m818MaskedCount.fetch_add(1,std::memory_order_relaxed)+1;
 if(count==1 || count%4096==0){
  char msg[200]{};sprintf_s(msg,"M8.18 MASK_STENCIL_APPLIED eye=%d strips=%u totalPasses=%llu",
   eye,(unsigned)rectangles.size(),(unsigned long long)count);log(msg);
 }
}

// M8.12f-prepass: CPU gaze staging must precede vkCmdBeginRenderPass.
// No Vulkan transfers are recorded here; the GPU image remains unchanged.
namespace { void m84PrepareEyePatterns(); }
namespace { void m75TracePass(const VkRenderPassBeginInfo* info); }
// M7.4i: capture distinct 5190x4168 legacy render-pass starts and the actual
// VkImages attached to their framebuffer. Diagnostic only; no Vulkan mutations.
namespace {
std::unordered_set<VkFramebuffer> m74iReportedFramebuffers;
std::atomic<unsigned> m74iDynamicCount{0};
void m74iTraceLegacy(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const char* api) {
 if(!m817Verbose())return;
 if (!info) return;
 std::vector<VkImage> images;
 std::vector<VkImageView> views;
 FramebufferInfo fb{};
 bool report=false;
 {
  std::lock_guard<std::mutex> guard(mu);
  auto fi=framebuffers.find(info->framebuffer);
  if (fi==framebuffers.end() || fi->second.width!=m813Width() || fi->second.height!=m813Height()) return;
  if (m74iReportedFramebuffers.size()>=512 || !m74iReportedFramebuffers.insert(info->framebuffer).second) return;
  fb=fi->second;
  auto ai=m74dAttachments.find(info->framebuffer);
  if (ai!=m74dAttachments.end()) {
   views=ai->second.views;
   for (auto view:views) {
    auto vi=m74gViews.find(view);
    images.push_back(vi==m74gViews.end()?VK_NULL_HANDLE:vi->second.image);
   }
  }
  report=true;
 }
 if (!report) return;
 char b[360]{};
 sprintf_s(b,"M7.4i pass api=%s cmd=%p fb=%p pass=%p size=%ux%u attachmentCount=%u area=%d,%d %ux%u",
   api,(void*)cmd,(void*)info->framebuffer,(void*)info->renderPass,fb.width,fb.height,
   (unsigned)images.size(),info->renderArea.offset.x,info->renderArea.offset.y,
   info->renderArea.extent.width,info->renderArea.extent.height);
 log(b);
 for (size_t i=0;i<images.size() && i<16;i++) {
  char a[256]{};
  sprintf_s(a,"M7.4i passAttachment fb=%p slot=%u view=%p VkImage=%p",
   (void*)info->framebuffer,(unsigned)i,(void*)views[i],(void*)images[i]);
  log(a);
 }
}
void m74iTraceDynamic(VkCommandBuffer cmd,const VkRenderingInfo* info,const char* api) {
 if(!m817Verbose())return;
 if (!info || info->renderArea.extent.width!=m813Width() || info->renderArea.extent.height!=m813Height()) return;
 unsigned seq=m74iDynamicCount.fetch_add(1,std::memory_order_relaxed);
 if (seq>=64) return;
 char b[256]{};
 sprintf_s(b,"M7.4i dynamic api=%s cmd=%p seq=%u colors=%u layers=%u",
  api,(void*)cmd,seq,info->colorAttachmentCount,info->layerCount);
 log(b);
 for (uint32_t i=0;i<info->colorAttachmentCount && i<8;i++) {
  VkImageView view=info->pColorAttachments?info->pColorAttachments[i].imageView:VK_NULL_HANDLE;
  VkImage image=VK_NULL_HANDLE;
  { std::lock_guard<std::mutex> guard(mu); auto it=m74gViews.find(view); if(it!=m74gViews.end())image=it->second.image; }
  char a[256]{};
  sprintf_s(a,"M7.4i dynamicAttachment seq=%u slot=%u view=%p VkImage=%p",seq,i,(void*)view,(void*)image);
  log(a);
 }
}
}
// M8.1 visual debug: track the active classified eye for end-of-pass overlay.
namespace {
struct M81OutlinePass { int eye; VkRect2D area; uint32_t width, height; };
std::unordered_map<VkCommandBuffer, M81OutlinePass> m81OutlinePasses;
void m81StartOutline(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info);
void m81DrawOutline(VkCommandBuffer cmd, const DeviceDispatch& d);
}
namespace {
// Caller holds mu. Mixed-eye and unknown framebuffers keep the common crop.
int m830FramebufferEye(VkFramebuffer framebuffer) {
 auto attachments=m74dAttachments.find(framebuffer);
 if(attachments==m74dAttachments.end())return -1;
 int eye=-1;
 for(auto view:attachments->second.views){
  auto v=m74gViews.find(view);if(v==m74gViews.end())continue;
  const int candidate=m8ClassifiedEye(v->second.image);
  if(candidate<0 || candidate>1)continue;
  if(eye>=0 && eye!=candidate)return -1;
  eye=candidate;
 }
 return eye;
}
void m830StartCrop(VkCommandBuffer cmd,const VkRenderPassBeginInfo* info,bool inlinePass) {
 std::lock_guard<std::mutex> guard(mu);
 cropStates.erase(cmd);
 if(!cropEnabled() || !info || !inlinePass)return;
 auto f=framebuffers.find(info->framebuffer);auto p=passInfos.find(info->renderPass);
 if(f==framebuffers.end() || p==passInfos.end() || f->second.layers!=1 ||
    f->second.width!=m813Width() || f->second.height!=m813Height() ||
    !p->second.colors || p->second.subpasses!=1)return;
 cropStates[cmd]={true,f->second.width,f->second.height,info->renderArea,0,m830FramebufferEye(info->framebuffer)};
}
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
 VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
 auto d = lookupCommand(cmd);
 if(m817Verbose()){
  m73ReadGaze();
  m74LogEyePass(cmd, info);
  m74iTraceLegacy(cmd, info, "BeginRenderPass");
  m75TracePass(info);
  m74LogEyeCandidate();
  logRenderPassBegin(info, "beginRenderPass");
 }
 VkRenderPassBeginInfo modified{};
 if(info){std::lock_guard<std::mutex> guard(mu);m87ActivePasses[cmd]=info->renderPass;
  m87PassBindCounts[cmd]=0;m89ActiveShadows.erase(cmd);m833ActiveEarlier.erase(cmd);m837ActiveShared.erase(cmd);
  m818SelectedEye.erase(cmd);m818MaskedCommands.erase(cmd);
 }
 // Prepare gaze while outside the render pass. This is CPU-only until
 // submission lifetime tracking is available; do NOT consume staged buffers on GPU.
 m84PrepareEyePatterns();
 m833Begin(cmd,info,contents==VK_SUBPASS_CONTENTS_INLINE);
 const bool useShadow = m57SelectShadow(cmd, info, modified);
 if(m835Enabled())m835BeginPass(cmd,info,contents!=VK_SUBPASS_CONTENTS_INLINE,useShadow);
 if (d.cmdBeginRenderPass) d.cmdBeginRenderPass(cmd, useShadow ? &modified : info, contents);
 if(contents==VK_SUBPASS_CONTENTS_INLINE)m818BeginStencil(cmd,info,useShadow,d);
 m81StartOutline(cmd, info);
 m830StartCrop(cmd,info,contents==VK_SUBPASS_CONTENTS_INLINE);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(
 VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* begin) {
 auto d = lookupCommand(cmd);
 if(m817Verbose()){
  m74LogEyePass(cmd, info);
  m74iTraceLegacy(cmd, info, "BeginRenderPass2");
  m75TracePass(info);
  m74LogEyeCandidate();
  logRenderPassBegin(info, "beginRenderPass2");
 }
 VkRenderPassBeginInfo modified{};
 if(info){std::lock_guard<std::mutex> guard(mu);m87ActivePasses[cmd]=info->renderPass;
  m87PassBindCounts[cmd]=0;m89ActiveShadows.erase(cmd);m833ActiveEarlier.erase(cmd);m837ActiveShared.erase(cmd);
  m818SelectedEye.erase(cmd);m818MaskedCommands.erase(cmd);
 }
 // Prepare gaze while outside the render pass. This is CPU-only until
 // submission lifetime tracking is available; do NOT consume staged buffers on GPU.
 m84PrepareEyePatterns();
 m833Begin(cmd,info,begin && begin->contents==VK_SUBPASS_CONTENTS_INLINE);
 const bool useShadow = m57SelectShadow(cmd, info, modified);
 if(m835Enabled())m835BeginPass(cmd,info,!begin || begin->contents!=VK_SUBPASS_CONTENTS_INLINE,useShadow);
 if (d.cmdBeginRenderPass2) d.cmdBeginRenderPass2(cmd, useShadow ? &modified : info, begin);
 if(begin && begin->contents==VK_SUBPASS_CONTENTS_INLINE)
  m818BeginStencil(cmd,info,useShadow,d);
 m81StartOutline(cmd, info);
 m830StartCrop(cmd,info,begin && begin->contents==VK_SUBPASS_CONTENTS_INLINE);
}


// M6.0/M8.29: dynamic scissor + final black borders, legacy VR render passes.
// Matching static scissors are handled at pipeline creation; dynamic rendering
// is not cropped by this path.
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cmd,uint32_t first,uint32_t count,const VkRect2D* rects) {
 auto d=lookupCommand(cmd);
 if(!cropEnabled()){if(d.cmdSetScissor)d.cmdSetScissor(cmd,first,count,rects);return;}
 if (!d.cmdSetScissor || !rects || !count) { if(d.cmdSetScissor) d.cmdSetScissor(cmd,first,count,rects); return; }
 CropState c{};
 { std::lock_guard<std::mutex> guard(mu); auto it=cropStates.find(cmd); if(it!=cropStates.end()) c=it->second; }
 if (!c.active) { d.cmdSetScissor(cmd,first,count,rects); return; }
 const auto area=intersectRect(cropRect(c.width,c.height,c.eye),c.renderArea);
 // M8.17: avoid heap allocation in the common single-viewport scissor path.
 if(count==1){
  const VkRect2D clipped=intersectRect(rects[0],area);
  d.cmdSetScissor(cmd,first,1,&clipped);
  return;
 }
 if(count<=16){
  std::array<VkRect2D,16> clipped{};
  for(uint32_t i=0;i<count;++i)clipped[i]=intersectRect(rects[i],area);
  d.cmdSetScissor(cmd,first,count,clipped.data());return;
 }
 std::vector<VkRect2D> clipped(rects,rects+count);
 for(auto& r:clipped) r=intersectRect(r,area);
 d.cmdSetScissor(cmd,first,count,clipped.data());
}
static void cropBlackBars(VkCommandBuffer cmd,const DeviceDispatch& d) {
 if(!cropEnabled())return;
 CropState c{};
 { std::lock_guard<std::mutex> guard(mu); auto it=cropStates.find(cmd); if(it!=cropStates.end()) {c=it->second; cropStates.erase(it);} }
 if (!c.active || !d.cmdClearAttachments || c.subpass!=0) return;
 // vkCmdClearAttachments is not affected by pipeline scissors.
 VkClearAttachment color{}; color.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; color.colorAttachment=0;
 color.clearValue.color.float32[0]=0; color.clearValue.color.float32[1]=0;
 color.clearValue.color.float32[2]=0; color.clearValue.color.float32[3]=1;
 const auto crop=cropGeometry(c.width,c.height,c.eye);
 VkClearRect bars[4]={};
 uint32_t count=0;
 for(const auto& outside:crop.outside){
  const auto rect=intersectRect(cropVkRect(outside),c.renderArea);
  if(rect.extent.width && rect.extent.height)bars[count++]={rect,0,1};
 }
 if(count)d.cmdClearAttachments(cmd,1,&color,count,bars);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd,VkPipelineBindPoint point,VkPipeline pipeline) {
 const auto originalPipeline=pipeline;bool profileVrs=false;
 DeviceDispatch d=lookupCommand(cmd);
 if(point==VK_PIPELINE_BIND_POINT_GRAPHICS && m817Verbose()) {
  bool report=false;unsigned binds=0;VkRenderPass actual=VK_NULL_HANDLE,expected=VK_NULL_HANDLE;
  {
   std::lock_guard<std::mutex> guard(mu);
   auto active=m87ActivePasses.find(cmd);
   auto pipe=m87Pipelines.find(pipeline);
   if(active!=m87ActivePasses.end()) {
    actual=active->second;
    binds=++m87PassBindCounts[cmd];
    if(pipe!=m87Pipelines.end())expected=pipe->second.renderPass;
    report=(binds==1 && m87Reports.fetch_add(1,std::memory_order_relaxed)<96);
   }
  }
  if(report){char msg[240]{};
   sprintf_s(msg,"M8.7 BIND passHash=%llu pipelinePassHash=%llu matched=%u firstBind=%u",
     static_cast<unsigned long long>(std::hash<VkRenderPass>{}(actual)),
     static_cast<unsigned long long>(std::hash<VkRenderPass>{}(expected)),
     actual==expected?1u:0u,binds);log(msg);
  }
  if(binds==1 && actual && expected) {
   // Status 1: same handle; 2: different handles with matching legacy
   // attachment/subpass signature; 3: known mismatch; 0: unknown.
   int status=0;
   bool newPair=false;
   {
    std::lock_guard<std::mutex> guard(mu);
    if(actual==expected)status=1;
    else {
     auto a=m88PassSignatures.find(actual), b=m88PassSignatures.find(expected);
     if(a!=m88PassSignatures.end() && b!=m88PassSignatures.end())
      status=m832Compatible(a->second,b->second)?2:3;
    }
    // Use actual pair of handles as key, not a hash of pass descriptions.
    const uint64_t ah=uint64_t(std::hash<VkRenderPass>{}(actual));
    const uint64_t bh=uint64_t(std::hash<VkRenderPass>{}(expected));
    const uint64_t key=ah^(bh+0x9e3779b97f4a7c15ULL+(ah<<6)+(ah>>2));
    if(m88Reports.load(std::memory_order_relaxed)<128 && m88ReportedPairs.insert(key).second){
     m88Reports.fetch_add(1,std::memory_order_relaxed);newPair=true;
    }
   }
   if(newPair){char msg[256]{};
    sprintf_s(msg,"M8.8 COMPAT passHash=%llu pipelinePassHash=%llu status=%d (0=unknown 1=same 2=signature-match 3=mismatch)",
      static_cast<unsigned long long>(std::hash<VkRenderPass>{}(actual)),
      static_cast<unsigned long long>(std::hash<VkRenderPass>{}(expected)),status);log(msg);
   }
  }
 }
 if(point==VK_PIPELINE_BIND_POINT_GRAPHICS && m838Enabled()){
   std::lock_guard<std::mutex> guard(mu);
   const auto dev=commandDevices.find(cmd);
   if(dev!=commandDevices.end()){
     auto& tracker=m837Trackers[dev->second];tracker.pipeline(m833Id(cmd),m833Id(originalPipeline));
     m838OriginalBindings[cmd]=originalPipeline;m838BoundCopies.erase(cmd);
     auto scope=m838Scopes.find(cmd);auto variant=m838Variants.find(originalPipeline);
     auto pass=m87ActivePasses.find(cmd);
     auto signature=pass==m87ActivePasses.end()?m88PassSignatures.end():m88PassSignatures.find(pass->second);
     if(scope!=m838Scopes.end() && scope->second.candidate && scope->second.on && !m811BaselineEnabled() &&
        variant!=m838Variants.end() && variant->second.device==dev->second &&
        signature!=m88PassSignatures.end() && m832Compatible(variant->second.compatibility,signature->second) &&
        !m89ActiveShadows.count(cmd) && tracker.allowUnormPipeline(m833Id(cmd),m833Id(originalPipeline),GetTickCount64())){
       pipeline=variant->second.pipeline;profileVrs=true;m838BoundCopies.insert(cmd);++tracker.stats.unormBinds;
       if(!scope->second.selected){scope->second.selected=true;++tracker.stats.unormSelected;}
     }
   }
 }
 if(point==VK_PIPELINE_BIND_POINT_GRAPHICS) {
   bool shadowActive=false, substituted=false, hasVariant=false, matchingPass=false,usedStencil=false,compatibleReuse=false;
   VkRenderPass activePass=VK_NULL_HANDLE;
   {
     std::lock_guard<std::mutex> guard(mu);
     auto active=m89ActiveShadows.find(cmd);
     auto variant=m89Variants.find(pipeline);
     auto dev=commandDevices.find(cmd);
     shadowActive=(active!=m89ActiveShadows.end());
     hasVariant=(variant!=m89Variants.end());
     if(shadowActive) {
       activePass=active->second;
       // BeamNG reuses pipelines across compatible render-pass objects. Handle
       // equality alone skipped these binds even when a VRS copy was ready.
       // Compare owned descriptions: no allocation, pipeline compilation or
       // reliance on the lifetime of the pipeline's original render pass.
       const auto key=m832ShadowSignatures.find(activePass);
       matchingPass=hasVariant && key!=m832ShadowSignatures.end() &&
         m832Compatible(variant->second.compatibility,key->second);
       if(matchingPass && dev!=commandDevices.end() && variant->second.device==dev->second) {
         usedStencil=m818MaskedCommands.count(cmd) && variant->second.maskedPipeline;
         pipeline=usedStencil ? variant->second.maskedPipeline : variant->second.pipeline;
         substituted=true;profileVrs=true;
         if(m833Enabled() && m833ActiveEarlier.count(cmd))++m833Trackers[dev->second].stats.binds;
         if(m837Enabled() && m837ActiveShared.count(cmd))++m837Trackers[dev->second].stats.binds;
         compatibleReuse=variant->second.shadowPass!=activePass;
       }
     }
   }
   if(usedStencil){
    const auto n=m818StencilPipelineBinds.fetch_add(1,std::memory_order_relaxed)+1;
    if(n==1 || n%8192==0){
     char msg[160]{};sprintf_s(msg,"M8.18 STENCIL_PIPELINE_BOUND count=%llu (recorded)",
       (unsigned long long)n);log(msg);
    }
   }
   if(shadowActive) {
     // Counts command recording, NOT executed GPU work or measured VRS savings.
     static std::atomic<unsigned long long> m811Substituted{0};
     static std::atomic<unsigned long long> m811NotSubstituted{0};
     const auto total = (substituted ? m811Substituted : m811NotSubstituted)
       .fetch_add(1,std::memory_order_relaxed)+1;
     if(total==1 || total==1000 || total==10000 || total==100000) {
       char msg[192]{};
       sprintf_s(msg,"M8.11 BIND_COUNT substituted=%u count=%llu (recorded, not executed)",
         substituted?1u:0u,static_cast<unsigned long long>(total));
       log(msg);
     }
     unsigned n=(substituted?m810BindReports:m810MissingReports).fetch_add(1,std::memory_order_relaxed);
     if(n<96) {
       char msg[256]{};
       sprintf_s(msg,"M8.10 SHADOW_BIND substituted=%u hasVariant=%u matchingPass=%u compatibleReuse=%u passHash=%llu",
         substituted?1u:0u,hasVariant?1u:0u,matchingPass?1u:0u,compatibleReuse?1u:0u,
         static_cast<unsigned long long>(std::hash<VkRenderPass>{}(activePass)));
       log(msg);
     }
   }
 }
 if(point==VK_PIPELINE_BIND_POINT_GRAPHICS && cropEnabled() && cropAsymmetric()){
  std::lock_guard<std::mutex> guard(mu);
  const auto state=cropStates.find(cmd);const auto variants=m830CropVariants.find(pipeline);
  const auto device=commandDevices.find(cmd);
  if(state!=cropStates.end() && state->second.active && state->second.eye>=0 && state->second.eye<2 &&
     variants!=m830CropVariants.end() && device!=commandDevices.end() && variants->second.device==device->second)
   pipeline=variants->second.eyes[state->second.eye];
 }
 if(m835Enabled())m835Profiler.bind(cmd,point,originalPipeline,profileVrs);
 if(d.cmdBindPipeline)d.cmdBindPipeline(cmd,point,pipeline);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device,VkPipeline pipeline,const VkAllocationCallbacks* alloc) {
 if(m835Enabled())m835Profiler.forgetPipeline(device,pipeline);
 PFN_vkDestroyPipeline next=nullptr;
 {std::lock_guard<std::mutex> guard(mu);
  auto pipeInfo=m87Pipelines.find(pipeline);
  if(pipeInfo!=m87Pipelines.end()){
   const auto originalPass=pipeInfo->second.renderPass;
   auto n=m818MaskedVariantsByPass.find(originalPass);
   auto vi=m89Variants.find(pipeline);
   if(n!=m818MaskedVariantsByPass.end() && vi!=m89Variants.end() &&
      vi->second.maskedPipeline && n->second>0)--n->second;
   m87Pipelines.erase(pipeInfo);
  }
  if(m838Enabled()){
   auto t=m837Trackers.find(device);if(t!=m837Trackers.end())t->second.erasePipeline(m833Id(pipeline));
   auto uniform=m838Variants.find(pipeline);
   if(uniform!=m838Variants.end()){
    auto d=devices.find(device);if(d!=devices.end() && d->second.destroyPipeline)d->second.destroyPipeline(device,uniform->second.pipeline,nullptr);
    m838Variants.erase(uniform);
   }
   for(auto i=m838OriginalBindings.begin();i!=m838OriginalBindings.end();)if(i->second==pipeline){m838BoundCopies.erase(i->first);i=m838OriginalBindings.erase(i);}else ++i;
  }
  auto variant=m89Variants.find(pipeline);
  auto destroyCrops=[&](VkPipeline base){
   auto crops=m830CropVariants.find(base);if(crops==m830CropVariants.end())return;
   auto dit=devices.find(device);
   if(dit!=devices.end() && dit->second.destroyPipeline)
    for(auto copy:crops->second.eyes)if(copy)dit->second.destroyPipeline(device,copy,alloc);
   m830CropVariants.erase(crops);
  };
  destroyCrops(pipeline);
  if(variant!=m89Variants.end()) {
    destroyCrops(variant->second.pipeline);destroyCrops(variant->second.maskedPipeline);
    auto dit=devices.find(device);
    if(dit!=devices.end() && dit->second.destroyPipeline)
      {
       if(variant->second.maskedPipeline)
         dit->second.destroyPipeline(device,variant->second.maskedPipeline,alloc);
       dit->second.destroyPipeline(device,variant->second.pipeline,alloc);
      }
    m89Variants.erase(variant);
  }
  auto it=devices.find(device);if(it!=devices.end())next=it->second.destroyPipeline;
 }
 if(next)next(device,pipeline,alloc);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer cmd) {
 m833End(cmd);
 {std::lock_guard<std::mutex> guard(mu);m87ActivePasses.erase(cmd);m87PassBindCounts.erase(cmd);m89ActiveShadows.erase(cmd);m833ActiveEarlier.erase(cmd);m837ActiveShared.erase(cmd);
  m818MaskedCommands.erase(cmd);m818SelectedEye.erase(cmd);}
 auto d=lookupCommand(cmd); m81DrawOutline(cmd,d); cropBlackBars(cmd,d); if(d.cmdEndRenderPass) d.cmdEndRenderPass(cmd);
 if(m835Enabled())m835Profiler.end(cmd);
 m838Restore(cmd,d);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2(VkCommandBuffer cmd,const VkSubpassEndInfo* end) {
 m833End(cmd);
 {std::lock_guard<std::mutex> guard(mu);m87ActivePasses.erase(cmd);m87PassBindCounts.erase(cmd);m89ActiveShadows.erase(cmd);m833ActiveEarlier.erase(cmd);m837ActiveShared.erase(cmd);
  m818MaskedCommands.erase(cmd);m818SelectedEye.erase(cmd);}
 auto d=lookupCommand(cmd); m81DrawOutline(cmd,d); cropBlackBars(cmd,d); if(d.cmdEndRenderPass2) d.cmdEndRenderPass2(cmd,end);
 if(m835Enabled())m835Profiler.end(cmd);
 m838Restore(cmd,d);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer cmd,VkSubpassContents contents) {
 m833Invalidate(cmd);
 auto d=lookupCommand(cmd); {std::lock_guard<std::mutex> guard(mu); cropStates.erase(cmd);m81OutlinePasses.erase(cmd);}
 if(d.cmdNextSubpass) d.cmdNextSubpass(cmd,contents);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2(VkCommandBuffer cmd,const VkSubpassBeginInfo* begin,const VkSubpassEndInfo* end) {
 m833Invalidate(cmd);
 auto d=lookupCommand(cmd); {std::lock_guard<std::mutex> guard(mu); cropStates.erase(cmd);m81OutlinePasses.erase(cmd);}
 if(d.cmdNextSubpass2) d.cmdNextSubpass2(cmd,begin,end);
}




// M7.5a: classify render-target images from copy destinations to OpenXR eye images.
namespace {
std::unordered_map<VkImage,int> m75Sources;
void m833ForgetImage(VkImage image){m75Sources.erase(image);}
int m8ClassifiedEye(VkImage image) {
 auto it = m75Sources.find(image);
 return it == m75Sources.end() ? -1 : it->second;
}
std::atomic<unsigned> m75MatchLogs{0};
bool m75ReadEyeImages(uint64_t out[2][3]) {
 static HANDLE mapping=nullptr;
 static const beamng_eye_images::Data* shared=nullptr;
 if(!shared){if(!mapping)mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_eye_images::mappingName);
  if(mapping)shared=static_cast<const beamng_eye_images::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_eye_images::Data)));}
 if(!shared)return false;
 for(int attempt=0;attempt<3;++attempt){
  LONG a=shared->sequence;MemoryBarrier();if(a&1)continue;
  uint32_t magic=shared->magic,pid=shared->processId;
  for(int eye=0;eye<2;++eye)for(int i=0;i<3;++i)out[eye][i]=shared->image[eye][i];
  MemoryBarrier();LONG b=shared->sequence;
  if(a==b && !(b&1) && magic==beamng_eye_images::magicValue && pid==GetCurrentProcessId() && out[0][0] && out[1][0] && out[0][0]!=out[1][0])return true;
 }
 return false;
}
int m75TraceTransfer(VkImage src,VkImage dst) {
 uint64_t images[2][3]{};if(!m75ReadEyeImages(images))return -1;
 int eye=-1;
 for(int e=0;e<2;++e)for(int i=0;i<3;++i)if(images[e][i] && (uint64_t)(uintptr_t)dst==images[e][i])eye=e;
 if(eye<0)return -1;
 {std::lock_guard<std::mutex> guard(mu);m75Sources[src]=eye;}
 unsigned n=m75MatchLogs.fetch_add(1,std::memory_order_relaxed);
 if(n<32){char b[256]{};sprintf_s(b,"M7.5a transferEye eye=%d source=%p destination=%p",eye,(void*)src,(void*)dst);log(b);}
 return eye;
}
// M7.5b: per-eye attachment readiness assessment. No GPU resources are created
// or bound here; this is a deliberately non-invasive prerequisite audit.
struct M75bGaze { bool valid=false; float u=0.f,v=0.f; unsigned long long age=0; uint64_t sourceTickMs=0; LONG sourceSequence=0; };
M75bGaze m75bReadGaze(int eye) {
 M75bGaze result{};
 static HANDLE mapping=nullptr;
 static const beamng_projected_gaze::Data* shared=nullptr;
 if(!shared){
  if(!mapping)mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,beamng_projected_gaze::mappingName);
  if(mapping)shared=static_cast<const beamng_projected_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(beamng_projected_gaze::Data)));
 }
 if(!shared || eye<0 || eye>1)return result;
 for(int i=0;i<3;++i){
  LONG before=shared->sequence;MemoryBarrier();if(before&1)continue;
  const uint32_t magic=shared->magic,pid=shared->processId,mask=shared->eyeValidMask;
  const uint64_t tick=shared->projectionTickMs,poseTick=shared->poseTickMs;
  const float u=shared->eyeU[eye],v=shared->eyeV[eye];
  MemoryBarrier();LONG after=shared->sequence;
  if(before!=after || (after&1) || magic!=beamng_projected_gaze::magicValue || pid!=GetCurrentProcessId())continue;
  uint64_t now=GetTickCount64();
  if(!beamng_gaze_freshness::validProjection(now,poseTick,tick,mask,eye,u,v))return result;
  result.valid=true;result.u=u;result.v=v;result.age=static_cast<unsigned long long>(now-tick);
  result.sourceTickMs=tick;result.sourceSequence=after;return result;
 }
 return result;
}

// M8.25: the SAME grace decision is used by live pattern staging and the
// debug-preview fallback. Store each eye independently and avoid holding gaze
// indefinitely if tracking disappears.
std::mutex m825GazeMutex;
std::array<beamng_gaze_hold::State,2> m825GazeStates{};
struct M825EffectiveGaze { M75bGaze gaze{}; beamng_gaze_hold::Phase phase=beamng_gaze_hold::Phase::Center; };
M825EffectiveGaze m825EffectiveGaze(int eye){
 const auto raw=m75bReadGaze(eye);
 beamng_gaze_hold::Sample sample{};
 sample.valid=raw.valid;sample.u=raw.u;sample.v=raw.v;
 sample.sourceTickMs=raw.sourceTickMs;sample.sequence=raw.sourceSequence;
 const auto now=GetTickCount64();
 beamng_gaze_hold::Decision decision{};
 {
  std::lock_guard<std::mutex> lock(m825GazeMutex);
  decision=beamng_gaze_hold::choose(m825GazeStates[eye],sample,now,m825GraceMs());
 }
 if(decision.changed){
  char message[220]{};
  const char* phase=decision.phase==beamng_gaze_hold::Phase::Live?"LIVE":
    decision.phase==beamng_gaze_hold::Phase::Grace?"GRACE":"CENTER";
  sprintf_s(message,"M8.25 GAZE_STATE eye=%d phase=%s graceMs=%u uv=(%.3f,%.3f)",
    eye,phase,m825GraceMs(),decision.sample.u,decision.sample.v);
  log(message);
 }
 M825EffectiveGaze out{};
 out.phase=decision.phase;
 out.gaze.valid=decision.phase!=beamng_gaze_hold::Phase::Center;
 out.gaze.u=decision.sample.u;out.gaze.v=decision.sample.v;
 out.gaze.sourceSequence=decision.sample.sequence;
 out.gaze.sourceTickMs=decision.sample.sourceTickMs;
 out.gaze.age=out.gaze.sourceTickMs && now>=out.gaze.sourceTickMs ?
    now-out.gaze.sourceTickMs:0;
 return out;
}

// M8.15: prepare CPU gaze and write only to staging slots NOT pinned by an
// in-flight transfer fence; the queue interceptor handles the GPU copies.
// CPU producer: choose effective gaze, generate the tile map, and publish only
// to a free staging slot. Queue-submit hooks are the GPU consumer; they pin each
// consumed slot until its fence signals. Never overwrite a busy mapped buffer.
void m84PrepareEyePatterns() {
 static const bool enabled=settingFlag("BEAMNG_VRS_M84_CPU_GAZE");
 if(!enabled)return;
 static std::atomic<uint64_t> lastTick{0};
 static std::array<std::atomic<LONG>,2> lastStagedSequence{};
 static std::array<std::atomic<uint64_t>,2> lastStagedGeneration{};
 const uint64_t now=GetTickCount64();
 uint64_t previous=lastTick.load(std::memory_order_relaxed);
 if(!beamng_gaze_timing::shouldPrepare(now,previous,m821UpdateIntervalMs()))return;
 if(!lastTick.compare_exchange_strong(previous,now,std::memory_order_relaxed))return;
 uint32_t width=0,height=0;
 {std::lock_guard<std::mutex> guard(mu);width=m813Width();height=m813Height();}
 const uint64_t generation=m814Key(width,height);
 for(int eye=0;eye<2;++eye){
  const auto decision=m825EffectiveGaze(eye);
  auto gaze=decision.gaze;
  // No tracking at boot: the GPU pattern is already centered. On dropout,
  // retain the last tracked region briefly, then upload ONE centered pattern.
  // A retained pose or center fallback is excluded from source-age statistics.
  const bool centeredFallback=decision.phase==beamng_gaze_hold::Phase::Center;
  if(centeredFallback){
   m821GazeStats.stale.fetch_add(1,std::memory_order_relaxed);
   const auto lastGeneration=lastStagedGeneration[eye].load(std::memory_order_relaxed);
   if(lastStagedSequence[eye].load(std::memory_order_relaxed)==0 &&
      (!lastGeneration || lastGeneration==generation))continue;
   gaze.u=.5f;gaze.v=.5f;gaze.sourceSequence=0;gaze.sourceTickMs=0;
  } else if(gaze.sourceSequence==lastStagedSequence[eye].load(std::memory_order_relaxed) &&
            lastStagedGeneration[eye].load(std::memory_order_relaxed)==generation){
   m821GazeStats.unchanged.fetch_add(1,std::memory_order_relaxed);continue;
  }
  const int mode=m58PatternMode();
  const float center=mode==1?.10f:mode==2?1.f:m59Radius("BEAMNG_VRS_CENTER_RADIUS",.30f);
  float middle=mode==1?.18f:mode==2?1.f:m59Radius("BEAMNG_VRS_MID_RADIUS",.65f);
  middle=std::max(center,middle);
  auto pattern=beamng_ffr::makeGaze(width,height,16,16,gaze.u,gaze.v,center,middle,
     m59Rate("BEAMNG_VRS_MID_RATE",beamng_ffr::rate(2,2)),
     m59Rate("BEAMNG_VRS_OUTER_RATE",beamng_ffr::rate(4,4)),m824Shape());
  if(mode==2)std::fill(pattern.texels.begin(),pattern.texels.end(),beamng_ffr::rate(1,1));
  // Stage freshest valid gaze only into a source buffer that is not pinned
  // by an in-flight GPU upload.
  if (m8Enabled()) {
   static std::atomic<unsigned> stagedReports[2]{};
   bool didStage=false;
   bool acceptedIdentical=false;
   std::lock_guard<std::mutex> guard(mu);
   // Another render thread can reach preparation while this pattern is built.
   if(width!=m813Width() || height!=m813Height())continue; // Discard work for a replaced generation.
   if(gaze.sourceSequence==lastStagedSequence[eye].load(std::memory_order_relaxed) &&
      lastStagedGeneration[eye].load(std::memory_order_relaxed)==generation)continue;
   for (auto& entry : m812Staging) {
    auto& slots=m815SlotStates[entry.first][generation];
    // Compare against the latest accepted payload, including a pending upload.
    // Both CPU comparison and GPU transfer only READ this coherent buffer.
    // No gaze threshold/smoothing: skip only a byte-for-byte identical image.
    const int latest=slots.latestSlot[eye];
    if(latest>=0 && latest<3){
     const auto& prior=entry.second.slots[eye][latest];
     if(prior.mapped && prior.capacity>=pattern.texels.size() &&
        !std::memcmp(prior.mapped,pattern.texels.data(),pattern.texels.size())){
      acceptedIdentical=true;
      m821GazeStats.identicalPattern.fetch_add(1,std::memory_order_relaxed);
      continue;
     }
    }
    // Do not rewrite staging memory while an outstanding transfer reads it.
    int selected=-1;
    for(int trial=0;trial<3;++trial){
     const int candidate=int((m812bPreparedCount[eye]+trial)%3);
     if(!slots.busy[eye][candidate]){selected=candidate;break;}
    }
    if(selected<0){m821GazeStats.noFreeSlot.fetch_add(1,std::memory_order_relaxed);continue;}
    auto& slot=entry.second.slots[eye][selected];
    if (!slot.mapped || slot.capacity<pattern.texels.size()) continue;
    std::memcpy(slot.mapped,pattern.texels.data(),pattern.texels.size());
    ++m812bPreparedCount[eye];
    slots.latestSlot[eye]=selected;
    slots.latestSeq[eye]=m812bPreparedCount[eye];
    slots.sourceTickMs[eye][selected]=gaze.sourceTickMs;
    slots.preparedTickMs[eye][selected]=now;
    m825StagedCenters[eye]={gaze.u,gaze.v};
    didStage=true;
    m821GazeStats.staged.fetch_add(1,std::memory_order_relaxed);
    if(!centeredFallback)m821GazeStats.prepareAgeMsSum.fetch_add(gaze.age,std::memory_order_relaxed);
    if (stagedReports[eye].fetch_add(1,std::memory_order_relaxed)<16) {
     char msg[240]{};
     sprintf_s(msg,"M8.15 STAGED eye=%d slot=%u uv=(%.3f,%.3f) bytes=%zu sequence=%llu pinned=0",
       eye,(unsigned)selected,gaze.u,gaze.v,pattern.texels.size(),
       static_cast<unsigned long long>(m812bPreparedCount[eye]));
     log(msg);
    }
   }
   if(didStage || acceptedIdentical){
    lastStagedSequence[eye].store(gaze.sourceSequence,std::memory_order_relaxed);
    lastStagedGeneration[eye].store(generation,std::memory_order_relaxed);
    if(centeredFallback){
     char msg[140]{};
     sprintf_s(msg,"M8.24 GAZE_FALLBACK eye=%d centerUV=(0.500,0.500) reason=NO_VALID_GAZE",eye);
     log(msg);
    }
   }
  }
  static std::atomic<unsigned> reports[2]{};
  if(m817Verbose() && reports[eye].fetch_add(1,std::memory_order_relaxed)<10){
   char msg[220]{};
   sprintf_s(msg,"M8.4 CPU gaze pattern eye=%d uv=(%.3f,%.3f) texels=%zu GPU_UPLOAD=LIVE_OPT_IN",
     eye,gaze.u,gaze.v,pattern.texels.size());
   log(msg);
  }
 }
}
// Debug overlay is independent of VRS enablement. It draws into color attachment 0
// immediately before the classified render pass ends. Intended only for single-subpass
// color passes; a later compositor may overwrite it.
void m81StartOutline(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info) {
 if(!outlineEnabled())return;
 std::lock_guard<std::mutex> guard(mu);
 m81OutlinePasses.erase(cmd);
 if(!info || info->renderArea.extent.width==0) return;
 auto fb=framebuffers.find(info->framebuffer);
 auto pass=passInfos.find(info->renderPass);
 if(fb==framebuffers.end() || pass==passInfos.end() || fb->second.width!=m813Width() || fb->second.height!=m813Height() ||
    pass->second.subpasses!=1 || pass->second.colors==0 || fb->second.layers!=1) return;
 auto a=m74dAttachments.find(info->framebuffer);
 if(a==m74dAttachments.end())return;
 int eye=-1;
 for(auto view:a->second.views) {
  auto v=m74gViews.find(view);
  if(v==m74gViews.end())continue;
  int candidate=m8ClassifiedEye(v->second.image);
  if(candidate<0)continue;
  if(eye>=0 && eye!=candidate)return;
  eye=candidate;
 }
 if(eye<0)return;
 // M8.25: only mark the eye render pass that truly received VRS substitution.
 // Other eye-sized intermediate passes could feed temporal history and leave
 // what appears to be a second/ghost red outline after resolution changes.
 if(m8Enabled() && m812liveEnabled()){
  const auto selected=m818SelectedEye.find(cmd);
  if(selected==m818SelectedEye.end() || selected->second!=eye)return;
 }
 m81OutlinePasses[cmd]={eye,info->renderArea,fb->second.width,fb->second.height};
}
void m81DrawOutline(VkCommandBuffer cmd, const DeviceDispatch& d) {
 if(!outlineEnabled())return;
 M81OutlinePass state{};
 {
  std::lock_guard<std::mutex> guard(mu);
  auto it=m81OutlinePasses.find(cmd);
  if(it==m81OutlinePasses.end())return;
  state=it->second;
  m81OutlinePasses.erase(it);
 }
 if(!d.cmdClearAttachments)return;
 // M8.25: prevent previous-resolution red borders being written into temporal
 // history during swapchain recreation. VRS itself continues uninterrupted.
 if(beamng_gaze_hold::suppressOutline(GetTickCount64(),
      m825LastResizeMs.load(std::memory_order_acquire),m825OutlinePauseMs()))return;
 if(state.width!=m813Width() || state.height!=m813Height())return;
 // PREVIEW mode is intentionally separate from real VRS binding.
 // Never claim the gaze-following preview corresponds to a GPU rate image.
 char preview[8]{};
 const bool gazePreview=beamng_config::read("BEAMNG_VRS_M82_GAZE_PREVIEW",preview,sizeof(preview))
                       && strcmp(preview,"1")==0;
 // Live VRS now uploads gaze-following rate images; the preview boundary is useful.
 float centerU=.5f,centerV=.5f;
 if(m8Enabled() && m812liveEnabled()){
  // Draw the exact center used for the most recently staged GPU VRS pattern,
  // not a newer raw gaze sample obtained after command-buffer recording.
  std::lock_guard<std::mutex> guard(mu);
  centerU=m825StagedCenters[state.eye].u;
  centerV=m825StagedCenters[state.eye].v;
 }else if(gazePreview){
  const auto gaze=m825EffectiveGaze(state.eye);
  if(gaze.gaze.valid){centerU=gaze.gaze.u;centerV=gaze.gaze.v;}
 }
 const int mode=m58PatternMode();
 if(mode==2)return; // Uniform 1x1 diagnostic has no internal boundary.
 const float centerSize=mode==1?.10f:m59Radius("BEAMNG_VRS_CENTER_RADIUS",.30f);
 float middleSize=mode==1?.18f:m59Radius("BEAMNG_VRS_MID_RADIUS",.65f);
 if(middleSize<centerSize)middleSize=centerSize;
 const auto shape=m824Shape();
 const uint8_t midRate=m59Rate("BEAMNG_VRS_MID_RATE",beamng_ffr::rate(2,2));
 const uint8_t outerRate=m59Rate("BEAMNG_VRS_OUTER_RATE",beamng_ffr::rate(4,4));
 const int tile=16,thickness=3;
 const int columns=int((state.width+tile-1)/tile);
 const int rows=int((state.height+tile-1)/tile);
 const auto drawBoundary=[&](float size,float r,float g,float b) {
  if(size<=0.f)return;
  // M8.26: use the exact same tile-center region classification as the
  // static and live VRS maps. This supports circle, square and ellipse.
  const auto inside=[&](int x,int y)->bool {
   if(x<0||y<0||x>=columns||y>=rows)return false;
   const float u=beamng_ffr::tileUV(uint32_t(x),uint32_t(tile),state.width);
   const float v=beamng_ffr::tileUV(uint32_t(y),uint32_t(tile),state.height);
   return beamng_ffr::inside(u,v,centerU,centerV,state.width,state.height,size,shape);
  };
  VkClearAttachment color{};
  color.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
  color.colorAttachment=0;
  color.clearValue.color.float32[0]=r;
  color.clearValue.color.float32[1]=g;
  color.clearValue.color.float32[2]=b;
  color.clearValue.color.float32[3]=1.f;
  const auto draw=[&](int x,int y,int w,int h) {
   const int x0=std::max(x,std::max(0,state.area.offset.x));
   const int y0=std::max(y,std::max(0,state.area.offset.y));
   const int x1=std::min(x+w,std::min(int(state.width),int(int64_t(state.area.offset.x)+state.area.extent.width)));
   const int y1=std::min(y+h,std::min(int(state.height),int(int64_t(state.area.offset.y)+state.area.extent.height)));
   if(x1<=x0||y1<=y0)return;
   VkClearRect rect{};
   rect.rect={{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}};
   rect.layerCount=1;
   d.cmdClearAttachments(cmd,1,&color,1,&rect);
  };
  // Circle/square: size is eye-height-relative diameter/side length.
  // Ellipse: size is a fraction of both viewport dimensions.
  const float halfUVY=.5f*size;
  const float halfUVX=shape==beamng_ffr::Shape::Ellipse ? .5f*size :
                       (.5f*size*float(state.height)/float(state.width));
  const int minX=std::max(0,int((centerU-halfUVX)*columns)-2);
  const int maxX=std::min(columns,int((centerU+halfUVX)*columns)+3);
  const int minY=std::max(0,int((centerV-halfUVY)*rows)-2);
  const int maxY=std::min(rows,int((centerV+halfUVY)*rows)+3);
  for(int y=minY;y<maxY;++y)for(int x=minX;x<maxX;++x) {
   if(!inside(x,y))continue;
   const int px=x*tile,py=y*tile;
   const int tw=std::min(tile,int(state.width)-px);
   const int th=std::min(tile,int(state.height)-py);
   if(!inside(x-1,y))draw(px,py,thickness,th);
   if(!inside(x+1,y))draw(px+tw-thickness,py,thickness,th);
   if(!inside(x,y-1))draw(px,py,tw,thickness);
   if(!inside(x,y+1))draw(px,py+th-thickness,tw,thickness);
  }
 };
 // Orange = outside edge of actual 2x2 middle region (into 4x4).
 // Don't display an orange '2x2' edge if the rates are configured otherwise.
 const bool orange2x2=middleSize>centerSize &&
   midRate==beamng_ffr::rate(2,2) && outerRate==beamng_ffr::rate(4,4);
 if(orange2x2)drawBoundary(middleSize,1.f,.55f,.05f);
 // Red = outside edge of the full-quality 1x1 region.
 if(centerSize>0.f)drawBoundary(centerSize,1.f,.016f,.016f);
 static std::atomic<unsigned> reports[2]{};
 if(reports[state.eye].fetch_add(1,std::memory_order_relaxed)<8) {
  char msg[320]{};
  sprintf_s(msg,"M8.26 outline eye=%d mode=%s uv=(%.3f,%.3f) center=%.3f middle=%.3f shape=%s liveGpuVrs=%u orange2x2=%u",
    state.eye,gazePreview?"GAZE_PREVIEW":"FIXED",centerU,centerV,centerSize,middleSize,
    beamng_ffr::shapeName(shape),m812liveEnabled()?1u:0u,orange2x2?1u:0u);
  log(msg);
 }

}

void m75TracePass(const VkRenderPassBeginInfo* info) {
 if(!m817Verbose())return;
 if(!info)return;
 bool found[2]{}; VkImage images[2]{VK_NULL_HANDLE,VK_NULL_HANDLE};
 {std::lock_guard<std::mutex> guard(mu);
  auto f=m74dAttachments.find(info->framebuffer);
  if(f!=m74dAttachments.end())for(auto view:f->second.views){
   auto v=m74gViews.find(view);if(v==m74gViews.end())continue;
   auto e=m75Sources.find(v->second.image);
   if(e!=m75Sources.end() && e->second>=0 && e->second<2){found[e->second]=true;images[e->second]=v->second.image;}
  }
 }
 static std::atomic<unsigned> counts[3]{};
 for(int eye=0;eye<2;++eye)if(found[eye]){
  unsigned n=counts[eye].fetch_add(1,std::memory_order_relaxed)+1;
  if(n<=8 || n%120==0){char b[256]{};sprintf_s(b,"M7.5a1 classifiedPass eye=%d count=%u fb=%p pass=%p image=%p",eye,n,(void*)info->framebuffer,(void*)info->renderPass,(void*)images[eye]);log(b);}
 }
 if(!found[0]&&!found[1]){
  unsigned n=counts[2].fetch_add(1,std::memory_order_relaxed)+1;
  if(n<=3 || n%2000==0){char b[192]{};sprintf_s(b,"M7.5a1 unclassifiedPass count=%u fb=%p",n,(void*)info->framebuffer);log(b);}
 }
 // M7.5b: independent, bounded readiness reporting for each classified eye.
 // The pass is only a candidate: pipeline compatibility, fragment-shading-rate
 // attachment layout, and GPU lifetime/synchronization are NOT yet validated.
 for(int eye=0;eye<2;++eye)if(found[eye] && !found[1-eye]){
  static std::atomic<unsigned> audited[2]{};
  unsigned count=audited[eye].fetch_add(1,std::memory_order_relaxed)+1;
  if(count<=6 || count%240==0){
   FramebufferInfo fb{};PassInfo pass{};bool fbKnown=false,passKnown=false;
   {
    std::lock_guard<std::mutex> guard(mu);
    auto fi=framebuffers.find(info->framebuffer);
    if(fi!=framebuffers.end()){fb=fi->second;fbKnown=true;}
    auto pi=passInfos.find(info->renderPass);
    if(pi!=passInfos.end()){pass=pi->second;passKnown=true;}
   }
   const M75bGaze gaze=m75bReadGaze(eye);
   const bool sizeOK=fbKnown && fb.width==m813Width() && fb.height==m813Height() && fb.layers==1;
   char b[512]{};
   sprintf_s(b,"M7.5b candidate eye=%d count=%u fb=%p pass=%p src=%p sizeOK=%u fbAttachments=%u passKnown=%u colors=%u subpasses=%u samples=%u gazeValid=%u UV=(%.3f,%.3f) gazeAge=%llums VRS=NOT_BOUND",
    eye,count,(void*)info->framebuffer,(void*)info->renderPass,(void*)images[eye],
    sizeOK?1u:0u,fbKnown?fb.attachments:0u,passKnown?1u:0u,
    passKnown?pass.colors:0u,passKnown?pass.subpasses:0u,passKnown?pass.samples:0u,
    gaze.valid?1u:0u,gaze.u,gaze.v,gaze.age);
   log(b);
  }
 }
 if(found[0]&&found[1])log("M7.5a1 WARNING: framebuffer matches BOTH eyes");
}
}
// M7.4h: diagnostic-only image transfer tracking. No GPU state is changed.
namespace {
std::atomic<unsigned> m74hTransferCount{0};
void m74hTrace(const char* op, VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t regions) {
 // Shared-scene learning records participation, not last-writer dependencies.
 // Copies may update the shared target between its heavy scene pass and display.
 // Keep the old eye-specific learner's strict transfer barrier unchanged.
 if(m833Enabled()){std::lock_guard<std::mutex> guard(mu);auto t=m833Command(cmd);if(t)t->invalidate(m833Id(cmd));}
 if(m838Enabled()){
  std::lock_guard<std::mutex> guard(mu);
  if(m838DepthImages.count(dst)){
   auto c=commandDevices.find(cmd);if(c!=commandDevices.end())m837Trackers[c->second].invalidate(m833Id(cmd));
  }
 }
 const int copyEye=m75TraceTransfer(src,dst);
 if(m835Enabled()){
  VkDevice device{};
  {std::lock_guard<std::mutex> guard(mu);auto c=commandDevices.find(cmd);if(c!=commandDevices.end())device=c->second;}
  // Stamp only a direct transfer to a currently published OpenXR image. Do not
  // seed new image generations from historical source-handle classifications.
  if(device)m835Profiler.resources([&](auto& t){t.observeEye(device,src,copyEye);t.observeEye(device,dst,copyEye);t.transfer(device,src,dst,op,regions);});
 }
 if(!m817Verbose())return;
 unsigned n = m74hTransferCount.fetch_add(1, std::memory_order_relaxed);
 if (n >= 12000) return;
 char b[256]{};
 sprintf_s(b, "M7.4h transfer=%s cmd=%p srcVkImage=%p dstVkImage=%p regions=%u seq=%u",
   op, (void*)cmd, (void*)src, (void*)dst, regions, n);
 log(b);
 if (n == 11999) log("M7.4h transfer logging cap reached (12000)");
}
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* r) {
 auto x=lookupCommand(c); m74hTrace("CopyImage",c,s,d,n); if(x.cmdCopyImage)x.cmdCopyImage(c,s,sl,d,dl,n,r);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit* r,VkFilter f) {
 auto x=lookupCommand(c); m74hTrace("BlitImage",c,s,d,n); if(x.cmdBlitImage)x.cmdBlitImage(c,s,sl,d,dl,n,r,f);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve* r) {
 auto x=lookupCommand(c); m74hTrace("ResolveImage",c,s,d,n); if(x.cmdResolveImage)x.cmdResolveImage(c,s,sl,d,dl,n,r);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer c,const VkCopyImageInfo2* i) {
 auto x=lookupCommand(c); if(i)m74hTrace("CopyImage2",c,i->srcImage,i->dstImage,i->regionCount); if(x.cmdCopyImage2)x.cmdCopyImage2(c,i);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2(VkCommandBuffer c,const VkBlitImageInfo2* i) {
 auto x=lookupCommand(c); if(i)m74hTrace("BlitImage2",c,i->srcImage,i->dstImage,i->regionCount); if(x.cmdBlitImage2)x.cmdBlitImage2(c,i);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2(VkCommandBuffer c,const VkResolveImageInfo2* i) {
 auto x=lookupCommand(c); if(i)m74hTrace("ResolveImage2",c,i->srcImage,i->dstImage,i->regionCount); if(x.cmdResolveImage2)x.cmdResolveImage2(c,i);
}
