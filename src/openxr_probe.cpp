#include "windows_config.hpp"
#include "openxr_turbo.hpp"
#include "d3d12_probe.hpp"
#include "lens_mask.hpp"
#include <memory>
// OpenXR side of BeamNG-VRS: identify eye swapchains, publish resolution and
// projected gaze, and optionally overlap frame waiting with BeamNG's CPU work.
// GPU shading/cropping live in layer.cpp. See docs/SOURCE-GUIDE.md for navigation.
// Never call a blocking runtime function while holding stateMutex.
#include <windows.h>
#include "beamng_gaze_shared.hpp"
#include "m819_visibility_geometry.hpp"
#include "vr_far_scan.hpp"
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#include <utility>
#include <string>
#include <algorithm>
#include <cmath>
namespace {
std::mutex stateMutex;
// Implicit layers sit above this explicit layer and may query global functions
// through it with XR_NULL_HANDLE; forward those to the chain below.
std::atomic<PFN_xrGetInstanceProcAddr> downstreamGetInstanceProcAddr{};
// One downstream function table per OpenXR instance. Each interceptor resolves
// ownership first, copies the needed entries, releases stateMutex, then forwards.
struct Dispatch {
 PFN_xrGetInstanceProcAddr next{};
 PFN_xrGetSystemProperties getSystemProperties{};
 PFN_xrDestroyInstance destroy{};
 PFN_xrCreateSession createSession{};
 PFN_xrAttachSessionActionSets attachSets{};
 PFN_xrSyncActions syncActions{};
 PFN_xrWaitFrame waitFrame{};
 PFN_xrBeginFrame beginFrame{};
 PFN_xrBeginSession beginSession{};
 PFN_xrEndSession endSession{};
 PFN_xrDestroySession destroySession{};
 PFN_xrDestroySwapchain destroySwapchain{};
 PFN_xrCreateReferenceSpace createReferenceSpace{};
 PFN_xrStringToPath stringToPath{};
 PFN_xrCreateActionSet createActionSet{};
 PFN_xrCreateAction createAction{};
 PFN_xrSuggestInteractionProfileBindings suggestBindings{};
 PFN_xrCreateActionSpace createActionSpace{};
 PFN_xrGetActionStatePose getActionStatePose{};
 PFN_xrLocateSpace locateSpace{};
 PFN_xrLocateViews locateViews{};
 PFN_xrEndFrame endFrame{};
 PFN_xrCreateSwapchain createSwapchain{};
 PFN_xrEnumerateSwapchainImages enumerateSwapchainImages{};
 PFN_xrAcquireSwapchainImage acquireSwapchainImage{};
 PFN_xrEnumerateViewConfigurationViews enumerateViews{};
 PFN_xrReleaseSwapchainImage releaseSwapchainImage{};
 PFN_xrGetVisibilityMaskKHR getVisibilityMask{};
 bool visibilityMaskEnabled{};
 PFN_xrDestroySpace destroySpace{};
 PFN_xrDestroyActionSet destroyActionSet{};
 bool gazeExtension{};
};
std::unordered_map<XrInstance, Dispatch> instances;
// Session ownership exists even with VRS/eye sampling off: scissor still needs
// eye-image identification, and turbo is independent of gaze support.
struct GazeSession {
 XrInstance instance{XR_NULL_HANDLE};
 XrActionSet set{XR_NULL_HANDLE};
 XrAction action{XR_NULL_HANDLE};
 XrSpace gazeSpace{XR_NULL_HANDLE};
 XrSpace localSpace{XR_NULL_HANDLE};
 XrTime displayTime{};
 unsigned samples{};
 XrPosef lastGaze{};
 XrTime lastGazeTime{};
 bool ready{};
 std::shared_ptr<beamng_turbo::Scheduler> turbo;
};
std::unordered_map<XrSession, GazeSession> sessions;
// M8.40: BeamNG 0.40 defaults to Direct3D 12. Mirror the two D3D12 OpenXR
// structs so this layer needs no d3d12.h; only the COM pointers are recorded.
struct D3D12Binding { XrStructureType type; const void* next; void* device; void* queue; };
struct D3D12SwapchainImage { XrStructureType type; void* next; void* texture; };
// M7.5a: OpenXR swapchain -> VkImage (or ID3D12Resource), then projection view -> eye.
std::unordered_map<XrSwapchain, std::vector<uint64_t>> m75SwapchainImage;
std::unordered_map<XrSwapchain,XrSession> swapchainSessions;
// M8.43: D3D12 eye images are finished just before release. Acquired indices
// are released oldest first; the eye comes from the last projection layer.
struct M843Swapchain { int64_t format=0; int eye=-1; std::vector<uint32_t> acquired; };
std::unordered_map<XrSwapchain,M843Swapchain> m843Swapchains;
// M8.44: on D3D12 the [Scissor] margins are removed from the field of view and
// the recommended image size, so BeamNG renders only the kept part in every
// pass. The compositor places the smaller views by their reported FOV.
std::atomic<bool> m844Active{false};
XrSession m844Session{XR_NULL_HANDLE}; // Guarded by stateMutex.
std::array<beamng_scissor::Margins,2> m844Margins{};
// Resolution is published as soon as the second matching swapchain exists;
// waiting for xrEndFrame would be too late for initial Vulkan framebuffer creation.
struct M814Dimensions { uint32_t width=0,height=0; };
struct M814PairState {uint64_t lastKey=0;unsigned streak=0;};
std::unordered_map<XrSession,M814PairState> m814PairStates;
HANDLE m814Mapping=nullptr;
beamng_eye_resolution::Data* m814Shared=nullptr;
void m814Publish(uint32_t width,uint32_t height) {
 if(!m814Shared){
  m814Mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
      sizeof(beamng_eye_resolution::Data),beamng_eye_resolution::mappingName);
  if(m814Mapping)m814Shared=static_cast<beamng_eye_resolution::Data*>(
      MapViewOfFile(m814Mapping,FILE_MAP_WRITE,0,0,sizeof(beamng_eye_resolution::Data)));
 }
 if(!m814Shared)return;
 auto* d=m814Shared;
 InterlockedIncrement(&d->sequence);MemoryBarrier();
 d->magic=beamng_eye_resolution::magicValue;
 d->processId=GetCurrentProcessId();d->width=width;d->height=height;
 d->changedTickMs=GetTickCount64();
 MemoryBarrier();InterlockedIncrement(&d->sequence);
}

void logLine(const char* msg);
// M8.19: query all three standardized visibility-mask representations.
// LINE_LOOP is preferred; VISIBLE_TRIANGLE_MESH falls back to a conservative
// convex hull. HIDDEN_TRIANGLE_MESH is probed but never inverted heuristically.
// A zero-count XR_SUCCESS means "mask unavailable" per OpenXR, even when the
// extension is advertised and enabled; the Vulkan layer must fail open.
struct M818SessionGeometry {
 unsigned polls=0,attempts=0;
 bool published=false,invalidated=false;
 std::array<std::vector<XrVector2f>,2> loop{};
};
std::unordered_map<XrSession,M818SessionGeometry> m818Sessions;
HANDLE m818Mapping=nullptr;
beamng_visibility_mesh::Data* m818Shared=nullptr;
bool m818Enabled() { char v[8]{};
 return beamng_config::read("BEAMNG_VRS_M818_STENCIL",v,sizeof(v)) && strcmp(v,"1")==0;
}
bool m819EnsureShared(){
 if(m818Shared)return true;
 m818Mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
        sizeof(beamng_visibility_mesh::Data),beamng_visibility_mesh::mappingName);
 if(m818Mapping)m818Shared=static_cast<beamng_visibility_mesh::Data*>(
     MapViewOfFile(m818Mapping,FILE_MAP_WRITE,0,0,sizeof(beamng_visibility_mesh::Data)));
 return m818Shared!=nullptr;
}
void m819InvalidateShared(){
 if(!m819EnsureShared())return;
 auto* d=m818Shared;
 InterlockedIncrement(&d->sequence);MemoryBarrier();
 d->magic=beamng_visibility_mesh::magicValue;
 d->processId=GetCurrentProcessId();d->validMask=0;
 MemoryBarrier();InterlockedIncrement(&d->sequence);
}
struct M819QueriedMask {
 XrResult result=XR_SUCCESS;
 uint32_t vertexCount=0,indexCount=0;
 std::vector<XrVector2f> points;
 std::vector<uint32_t> indices;
 bool valid=false;
};
M819QueriedMask m819Query(const Dispatch& d,XrSession session,uint32_t eye,XrVisibilityMaskTypeKHR type) {
 M819QueriedMask output{};
 XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
 output.result=d.getVisibilityMask(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,type,&mask);
 output.vertexCount=mask.vertexCountOutput;output.indexCount=mask.indexCountOutput;
 // OpenXR: XR_SUCCESS + zero vertices/indices explicitly means unavailable.
 if(XR_FAILED(output.result)||mask.vertexCountOutput<3||mask.indexCountOutput<3||
    mask.vertexCountOutput>8192 || mask.indexCountOutput>32768)return output;
 output.points.resize(mask.vertexCountOutput);
 output.indices.resize(mask.indexCountOutput);
 mask.vertexCapacityInput=static_cast<uint32_t>(output.points.size());
 mask.indexCapacityInput=static_cast<uint32_t>(output.indices.size());
 mask.vertices=output.points.data();mask.indices=output.indices.data();
 output.result=d.getVisibilityMask(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,type,&mask);
 output.vertexCount=mask.vertexCountOutput;output.indexCount=mask.indexCountOutput;
 if(XR_FAILED(output.result)||mask.vertexCountOutput<3||mask.indexCountOutput<3||
    mask.vertexCountOutput>output.points.size()||mask.indexCountOutput>output.indices.size())return output;
 output.points.resize(mask.vertexCountOutput);output.indices.resize(mask.indexCountOutput);
 for(uint32_t i:output.indices)if(i>=output.points.size())return output;
 for(const auto& p:output.points)if(!std::isfinite(p.x)||!std::isfinite(p.y))return output;
 output.valid=true;return output;
}
void m818Publish(XrSession session,const Dispatch& d,const XrView* views) {
 if(!m818Enabled() || !d.visibilityMaskEnabled || !d.getVisibilityMask || !views)return;
 auto& geo=m818Sessions[session]; // App serializes xrLocateViews per session.
 if(!geo.invalidated){m819InvalidateShared();geo.invalidated=true;}
 if(!geo.published && geo.attempts<3){
  ++geo.polls;
  // Give a runtime time to initialize masks; avoid per-frame queries.
  if(geo.polls==1 || geo.polls==10 || geo.polls==40){
   ++geo.attempts;
   for(uint32_t eye=0;eye<2;++eye){
    if(geo.loop[eye].size()>=3)continue;
    const auto outline=m819Query(d,session,eye,XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR);
    const auto visible=m819Query(d,session,eye,XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR);
    const auto hidden=m819Query(d,session,eye,XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR);
    for(const auto& entry:std::array<std::pair<const char*,const M819QueriedMask*>,3>{{
      {"LINE_LOOP",&outline},{"VISIBLE_TRIANGLES",&visible},{"HIDDEN_TRIANGLES",&hidden}}}){
     char msg[256]{};
     sprintf_s(msg,"M8.19 XR_MASK_QUERY attempt=%u eye=%u type=%s result=%d vertices=%u indices=%u usable=%u",
      geo.attempts,eye,entry.first,int(entry.second->result),entry.second->vertexCount,
      entry.second->indexCount,entry.second->valid?1u:0u);
     logLine(msg);
    }
    if(outline.valid && outline.indices.size()>=3 &&
       outline.indices.size()<=beamng_visibility_mesh::maxVertices){
     // LINE_LOOP indices are ordered around the boundary, per the spec.
     for(uint32_t idx:outline.indices)geo.loop[eye].push_back(outline.points[idx]);
     char msg[160]{};sprintf_s(msg,"M8.19 XR_MASK_SELECTED eye=%u representation=LINE_LOOP vertices=%u",
      eye,(unsigned)geo.loop[eye].size());logLine(msg);
    }else if(visible.valid && visible.indices.size()%3==0){
     // Convex hull of vertices referenced by visible triangles: intentionally
     // leaves extra pixels *visible*, never intentionally discards visible mesh.
     std::vector<beamng_visibility_geometry::Point> verts;
     verts.reserve(visible.points.size());
     // Every supplied vertex belongs to the visible mesh; including even
     // unreferenced ones can only enlarge the conservative hull.
     for(const auto& p:visible.points)verts.push_back({p.x,p.y});
     std::vector<beamng_visibility_geometry::Point> hull;
     if(beamng_visibility_geometry::convexHull(verts,hull,beamng_visibility_mesh::maxVertices)){
      for(const auto& p:hull)geo.loop[eye].push_back({p.x,p.y});
      char msg[160]{};sprintf_s(msg,"M8.19 XR_MASK_SELECTED eye=%u representation=VISIBLE_CONVEX_HULL vertices=%u",
       eye,(unsigned)hull.size());logLine(msg);
     }
    }
   }
   if(geo.attempts==3 && (geo.loop[0].size()<3||geo.loop[1].size()<3))
    logLine("M8.19 XR_MASK_UNAVAILABLE_ALL_ATTEMPTS failOpen=1 lensStencilDisabled=1");
  }
 }
 if(geo.published || geo.loop[0].size()<3 || geo.loop[1].size()<3)return;
 if(!m819EnsureShared())return;
 auto* shared=m818Shared;
 InterlockedIncrement(&shared->sequence);MemoryBarrier();
 shared->magic=beamng_visibility_mesh::magicValue;
 shared->processId=GetCurrentProcessId();shared->validMask=0;
 for(unsigned eye=0;eye<2;++eye){
  const auto& f=views[eye].fov;
  auto& dst=shared->eyes[eye];
  dst.tanLeft=tanf(f.angleLeft);dst.tanRight=tanf(f.angleRight);
  dst.tanDown=tanf(f.angleDown);dst.tanUp=tanf(f.angleUp);
  dst.count=static_cast<uint32_t>(geo.loop[eye].size());
  for(uint32_t i=0;i<dst.count;++i){dst.vertices[i].x=geo.loop[eye][i].x;dst.vertices[i].y=geo.loop[eye][i].y;}
  shared->validMask|=(1u<<eye);
 }
 MemoryBarrier();InterlockedIncrement(&shared->sequence);
 geo.published=true;
 logLine("M8.19 XR_MASK_PUBLISHED eyes=2 geometry=LINE_LOOP_OR_VISIBLE_HULL projection=PER_EYE_FOV");
}

HANDLE m75Mapping = nullptr;
beamng_eye_images::Data* m75Shared = nullptr;
void m75Publish(const uint64_t images[2][3]) {
 if(!m75Shared) {
  m75Mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(beamng_eye_images::Data),beamng_eye_images::mappingName);
  if(m75Mapping)m75Shared=static_cast<beamng_eye_images::Data*>(MapViewOfFile(m75Mapping,FILE_MAP_WRITE,0,0,sizeof(beamng_eye_images::Data)));
 }
 if(!m75Shared)return;
 auto* d=m75Shared;
 InterlockedIncrement(&d->sequence);MemoryBarrier();
 d->magic=beamng_eye_images::magicValue;d->processId=GetCurrentProcessId();
 for(int eye=0;eye<2;++eye)for(int i=0;i<3;++i)d->image[eye][i]=images[eye][i];d->updatedTickMs=GetTickCount64();
 MemoryBarrier();InterlockedIncrement(&d->sequence);
}
HANDLE gazeMapping = nullptr;
beamng_gaze::Data* gazeShared = nullptr;
HANDLE m827GazeMapping=nullptr;
beamng_projected_gaze::Data* m827GazeShared=nullptr;
std::mutex m827GazeWriteMutex;
void publishProjectedGaze(const float u[2],const float v[2],uint32_t mask,
                         uint64_t poseTick,XrTime time){
 std::lock_guard<std::mutex> guard(m827GazeWriteMutex);
 if(!m827GazeShared){
  m827GazeMapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
    sizeof(beamng_projected_gaze::Data),beamng_projected_gaze::mappingName);
  if(m827GazeMapping)m827GazeShared=static_cast<beamng_projected_gaze::Data*>(
    MapViewOfFile(m827GazeMapping,FILE_MAP_WRITE,0,0,sizeof(beamng_projected_gaze::Data)));
 }
 if(!m827GazeShared)return;
 auto* s=m827GazeShared;InterlockedIncrement(&s->sequence);MemoryBarrier();
 s->magic=beamng_projected_gaze::magicValue;s->processId=GetCurrentProcessId();
 s->poseTickMs=poseTick;s->projectionTickMs=GetTickCount64();
 s->poseTime=time;s->projectionTime=time;s->eyeValidMask=mask;
 for(int eye=0;eye<2;++eye){s->eyeU[eye]=u[eye];s->eyeV[eye]=v[eye];}
 MemoryBarrier();InterlockedIncrement(&s->sequence);
}
void publishGaze(const XrSpaceLocation& loc, XrTime predictedTime) {
 if (!gazeShared) {
  gazeMapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(beamng_gaze::Data),beamng_gaze::mappingName);
  if (gazeMapping) gazeShared=static_cast<beamng_gaze::Data*>(MapViewOfFile(gazeMapping,FILE_MAP_WRITE,0,0,sizeof(beamng_gaze::Data)));
  if (!gazeShared) return;
 }
 auto* d=gazeShared;
 InterlockedIncrement(&d->sequence); // odd: writing (single OpenXR session writer)
 MemoryBarrier();
 d->magic=beamng_gaze::magicValue;
 d->processId=GetCurrentProcessId();
 d->tickMs=GetTickCount64();
 d->predictedTime=static_cast<int64_t>(predictedTime);
 d->qx=loc.pose.orientation.x;d->qy=loc.pose.orientation.y;
 d->qz=loc.pose.orientation.z;d->qw=loc.pose.orientation.w;
 d->px=loc.pose.position.x;d->py=loc.pose.position.y;d->pz=loc.pose.position.z;
 d->flags=loc.locationFlags;
 MemoryBarrier();
 InterlockedIncrement(&d->sequence); // even: complete
}

// M8.17: logging level is shared by the Vulkan and OpenXR DLLs.
int m817LogLevel() {
 static const int level=[](){
  char v[8]{};
  const DWORD n=beamng_config::read("BEAMNG_VRS_LOG_LEVEL",v,sizeof(v));
  if(n && n<sizeof(v) && strcmp(v,"-1")==0)return -1;
  return n && n<sizeof(v) && v[0]=='0' && !v[1] ? 0 :
         n && n<sizeof(v) && v[0]=='2' && !v[1] ? 2 : 1;
 }();
 return level;
}
bool m817Verbose(){return m817LogLevel()==2;}
int m817MessageLevel(const char* msg){
 if(strstr(msg,"FAILED") || strstr(msg,"ERROR") || strstr(msg,"WARNING") ||
    strstr(msg,"FATAL") || strstr(msg,"result=-"))return 0;
 if(strstr(msg,"M8.14 XR_EYE_RESOLUTION") || strstr(msg,"M8.17") || strstr(msg,"M8.18") ||
    strstr(msg,"Runtime advertises") || strstr(msg,"System supports") ||
    strstr(msg,"M8.31 TURBO") || strstr(msg,"Next xrCreateApiLayerInstance") || strstr(msg,"Opt-in:") ||
    // D3D12 setup and first-use lines are flushed at once, so a crash right after still leaves them.
    strstr(msg,"M8.40 graphics") || strstr(msg,"hooks=") || strstr(msg,"foveation active") ||
    strstr(msg,"M8.42 shading-rate image") || strstr(msg,"foveation frames") || strstr(msg,"M8.43") || strstr(msg,"M8.44") || strstr(msg,"M8.47") || strstr(msg,"M8.50") || strstr(msg,"M8.51"))return 1;
 return 2;
}
void logLine(const char* msg) {
 if(m817LogLevel()<0)return;
 const int severity=m817MessageLevel(msg);
 if(severity>m817LogLevel())return;
 static std::mutex fileMutex;
 struct LogStream {
  FILE* file=nullptr;
  char buffer[16*1024]{};
  ~LogStream(){if(file)fclose(file);}
 };
 static LogStream stream;
 static bool attempted=false;
 static unsigned writes=0;
 std::lock_guard<std::mutex> lock(fileMutex);
 if(!attempted){
  attempted=true;
  char dir[MAX_PATH]{};
  const DWORD n=beamng_config::read("TEMP",dir,MAX_PATH);
  if(n && n<MAX_PATH && strcat_s(dir,"\\BeamNG_VRS_eye_probe.log")==0){
   if(fopen_s(&stream.file,dir,"a")==0 && stream.file)
    setvbuf(stream.file,stream.buffer,_IOFBF,sizeof(stream.buffer));
  }
 }
 if(!stream.file)return;
 fprintf(stream.file,"%s\n",msg);
 if(severity<=1 || (++writes%128)==0)fflush(stream.file);
}

void logResult(const char* what,XrResult r){char s[256]{};sprintf_s(s,sizeof(s),"%s result=%d",what,(int)r);logLine(s);}
bool enabled(const char* name){char s[16]{};return beamng_config::read(name,s,sizeof(s)) && strcmp(s,"1")==0;}
}
namespace {
struct FrameContext {Dispatch dispatch{};std::shared_ptr<beamng_turbo::Scheduler> turbo;};
FrameContext frameContext(XrSession session){
 std::lock_guard<std::mutex> lock(stateMutex);FrameContext out;
 auto s=sessions.find(session);if(s==sessions.end())return out;
 auto i=instances.find(s->second.instance);if(i!=instances.end())out.dispatch=i->second;
 out.turbo=s->second.turbo;return out;
}
void turboReport(const std::shared_ptr<beamng_turbo::Scheduler>& turbo){
 if(!turbo)return;const auto s=turbo->stats();char msg[384]{};
 sprintf_s(msg,"M8.31 TURBO normalWaits=%llu predictedWaits=%llu readyWaits=%llu blockedWaits=%llu failures=%llu runtimeWaitMs=%.3f appWaitMs=%.3f endWaitMs=%.3f (cumulative CPU waits, not GPU time)",
  (unsigned long long)s.normal,(unsigned long long)s.predicted,(unsigned long long)s.ready,
  (unsigned long long)s.blocked,(unsigned long long)s.failures,s.runtimeWaitNs/1e6,s.appWaitNs/1e6,s.endWaitNs/1e6);
 logLine(msg);
}
void eraseSessionState(XrSession session){ // Caller holds stateMutex.
 sessions.erase(session);m818Sessions.erase(session);m814PairStates.erase(session);
 if(session==m844Session){m844Session=XR_NULL_HANDLE;m844Active=false;}
 for(auto it=swapchainSessions.begin();it!=swapchainSessions.end();){
  if(it->second==session){m75SwapchainImage.erase(it->first);m843Swapchains.erase(it->first);it=swapchainSessions.erase(it);}else ++it;
 }
}
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeBeginSession(XrSession session,const XrSessionBeginInfo* info){
 auto c=frameContext(session);if(!c.dispatch.beginSession)return XR_ERROR_FUNCTION_UNSUPPORTED;
 const auto result=c.dispatch.beginSession(session,info);
 if(result==XR_SUCCESS && c.turbo && info && info->primaryViewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO){
  c.turbo->start();logLine("M8.31 TURBO enabled: single-frame async wait; experimental, motion reprojection may be incompatible");
 }
 return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeEndSession(XrSession session){
 auto c=frameContext(session);if(!c.dispatch.endSession)return XR_ERROR_FUNCTION_UNSUPPORTED;
 if(c.turbo){c.turbo->stop();turboReport(c.turbo);}
 return c.dispatch.endSession(session);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeDestroySession(XrSession session){
 auto c=frameContext(session);if(!c.dispatch.destroySession)return XR_ERROR_FUNCTION_UNSUPPORTED;
 if(c.turbo){c.turbo->stop();turboReport(c.turbo);}
 const auto result=c.dispatch.destroySession(session);
 if(XR_SUCCEEDED(result)){std::lock_guard<std::mutex> lock(stateMutex);eraseSessionState(session);}
 return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeBeginFrame(XrSession session,const XrFrameBeginInfo* info){
 auto c=frameContext(session);if(!c.dispatch.beginFrame)return XR_ERROR_FUNCTION_UNSUPPORTED;
 return c.turbo?c.turbo->begin(info):c.dispatch.beginFrame(session,info);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeDestroySwapchain(XrSwapchain swapchain){
 FrameContext c{};
 {std::lock_guard<std::mutex> lock(stateMutex);auto owner=swapchainSessions.find(swapchain);
  if(owner!=swapchainSessions.end()){
   auto s=sessions.find(owner->second);if(s!=sessions.end()){
    c.turbo=s->second.turbo;auto i=instances.find(s->second.instance);if(i!=instances.end())c.dispatch=i->second;
   }
  }
  if(!c.dispatch.destroySwapchain && !instances.empty())c.dispatch=instances.begin()->second;
 }
 if(!c.dispatch.destroySwapchain)return XR_ERROR_FUNCTION_UNSUPPORTED;
 if(c.turbo)c.turbo->drain();
 const auto result=c.dispatch.destroySwapchain(swapchain);
 if(XR_SUCCEEDED(result)){std::lock_guard<std::mutex> lock(stateMutex);swapchainSessions.erase(swapchain);m75SwapchainImage.erase(swapchain);m843Swapchains.erase(swapchain);}
 return result;
}
// M7.4e: read-only OpenXR stereo projection diagnostics.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeCreateSwapchain(XrSession session,const XrSwapchainCreateInfo* ci,XrSwapchain* out){
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex); auto s=sessions.find(session);if(s!=sessions.end()){auto i=instances.find(s->second.instance);if(i!=instances.end())d=i->second;}}
 if(!d.createSwapchain)return XR_ERROR_FUNCTION_UNSUPPORTED;
 XrResult r=d.createSwapchain(session,ci,out);
 if(XR_SUCCEEDED(r)&&ci&&out){
  {std::lock_guard<std::mutex> lock(stateMutex);swapchainSessions[*out]=session;m843Swapchains[*out].format=ci->format;}
  char msg[384]{};
  sprintf_s(msg,sizeof(msg),"M7.4e swapchain=%p dims=%ux%u array=%u faces=%u samples=%u usage=0x%llX format=%lld",
    (void*)*out,ci->width,ci->height,ci->arraySize,ci->faceCount,ci->sampleCount,
    (unsigned long long)ci->usageFlags,(long long)ci->format);logLine(msg);
  // Require a PAIR of identically sized render swapchains. Ignore single UI
  // swapchains, cubemaps and array layers. No hardcoded headset resolution.
  if(ci->width>=640 && ci->height>=640 && ci->arraySize==1 && ci->faceCount==1 && ci->sampleCount==1){
   const uint64_t key=(uint64_t(ci->width)<<32)|ci->height;
   bool publish=false;
   {std::lock_guard<std::mutex> g(stateMutex);
    auto& pair=m814PairStates[session];
    if(pair.lastKey!=key){pair.lastKey=key;pair.streak=1;}
    else ++pair.streak;
    publish=(pair.streak % 2u)==0;
   }
   if(publish){m814Publish(ci->width,ci->height);
    char m[200]{};sprintf_s(m,"M8.14 XR_EYE_RESOLUTION width=%u height=%u source=SWAPCHAIN_PAIR",ci->width,ci->height);logLine(m);
   }
  }
 }
 return r;
}
// M7.4f: log actual VkImage handles from each OpenXR swapchain.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeEnumerateSwapchainImages(
 XrSwapchain swapchain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images){
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex);
  // Swapchain ownership is not exposed in this callback; use the resolved
  // runtime function from the live OpenXR instance (BeamNG has one instance).
  for(const auto& kv:instances){if(kv.second.enumerateSwapchainImages){d=kv.second;break;}}
 }
 if(!d.enumerateSwapchainImages)return XR_ERROR_FUNCTION_UNSUPPORTED;
 XrResult r=d.enumerateSwapchainImages(swapchain,capacity,count,images);
 static unsigned reported=0;
 if(XR_SUCCEEDED(r)&&count&&images&&capacity>=*count&&reported<160){
  if(*count && images[0].type==XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR){
   for(uint32_t i=0;i<*count&&reported<160;++i){
    const auto* vi=&reinterpret_cast<const XrSwapchainImageVulkanKHR*>(images)[i];
    if(vi->type!=XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)break;
    char msg[256]{};
    sprintf_s(msg,sizeof(msg),"M7.4f swapchain=%p imageIndex=%u VkImage=%p",
      (void*)swapchain,i,(void*)vi->image);
    logLine(msg);
    {std::lock_guard<std::mutex> guard(stateMutex); {auto& entries=m75SwapchainImage[swapchain];if(entries.size()<=i)entries.resize(i+1);entries[i]=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vi->image));}}
    ++reported;
   }
  }else if(*count && images[0].type==XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR){
   for(uint32_t i=0;i<*count&&reported<160;++i){
    const auto* di=&reinterpret_cast<const D3D12SwapchainImage*>(images)[i];
    if(di->type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)break;
    char msg[256]{};
    sprintf_s(msg,sizeof(msg),"M8.40 swapchain=%p imageIndex=%u ID3D12Resource=%p",
      (void*)swapchain,i,di->texture);
    logLine(msg);
    {std::lock_guard<std::mutex> guard(stateMutex); {auto& entries=m75SwapchainImage[swapchain];if(entries.size()<=i)entries.resize(i+1);entries[i]=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(di->texture));}}
    ++reported;
   }
  }else{logLine("M7.4f swapchain images are neither Vulkan nor D3D12 KHR type");++reported;}
 }
 return r;
}
Dispatch swapchainDispatch(XrSwapchain swapchain){ // Caller holds stateMutex.
 auto owner=swapchainSessions.find(swapchain);
 if(owner!=swapchainSessions.end()){auto s=sessions.find(owner->second);
  if(s!=sessions.end()){auto i=instances.find(s->second.instance);if(i!=instances.end())return i->second;}}
 return instances.empty()?Dispatch{}:instances.begin()->second;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeAcquireSwapchainImage(XrSwapchain swapchain,const XrSwapchainImageAcquireInfo* info,uint32_t* index){
 Dispatch d{};{std::lock_guard<std::mutex> g(stateMutex);d=swapchainDispatch(swapchain);}
 if(!d.acquireSwapchainImage)return XR_ERROR_FUNCTION_UNSUPPORTED;
 const auto r=d.acquireSwapchainImage(swapchain,info,index);
 if(XR_SUCCEEDED(r)&&index){std::lock_guard<std::mutex> g(stateMutex);auto it=m843Swapchains.find(swapchain);
  if(it!=m843Swapchains.end()&&it->second.acquired.size()<8)it->second.acquired.push_back(*index);}
 return r;
}
// M8.43: crop/lens/outline on D3D12 eye images; the game has submitted its
// rendering, and the runtime reads the image only after this release.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeReleaseSwapchainImage(XrSwapchain swapchain,const XrSwapchainImageReleaseInfo* info){
 Dispatch d{};uint64_t texture=0;int eye=-1;int64_t format=0;
 {std::lock_guard<std::mutex> g(stateMutex);d=swapchainDispatch(swapchain);
  auto it=m843Swapchains.find(swapchain);
  if(it!=m843Swapchains.end()&&!it->second.acquired.empty()){
   const uint32_t index=it->second.acquired.front();it->second.acquired.erase(it->second.acquired.begin());
   eye=it->second.eye;format=it->second.format;
   auto images=m75SwapchainImage.find(swapchain);
   if(images!=m75SwapchainImage.end()&&index<images->second.size())texture=images->second[index];
  }
 }
 if(!d.releaseSwapchainImage)return XR_ERROR_FUNCTION_UNSUPPORTED;
 if(texture&&eye>=0)beamng_d3d12::finishEyeImage(texture,eye,uint32_t(format));
 return d.releaseSwapchainImage(swapchain,info);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeEndFrame(XrSession session,const XrFrameEndInfo* info){
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex);auto s=sessions.find(session);if(s!=sessions.end()){auto i=instances.find(s->second.instance);if(i!=instances.end())d=i->second;}}
 if(!d.endFrame)return XR_ERROR_FUNCTION_UNSUPPORTED;
 static unsigned reported=0;
 if(info && reported<64){
  for(uint32_t li=0;li<info->layerCount && reported<64;++li){
   const XrCompositionLayerBaseHeader* layer=info->layers?info->layers[li]:nullptr;
   if(!layer || layer->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
   const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(layer);
   for(uint32_t vi=0;vi<projection->viewCount && reported<64;++vi){
    const auto& v=projection->views[vi];const auto& sub=v.subImage;
    char msg[400]{};
    sprintf_s(msg,sizeof(msg),"M7.4e projection layer=%u eyeView=%u viewCount=%u swapchain=%p arrayIndex=%u rect=%d,%d %dx%d",
      li,vi,projection->viewCount,(void*)sub.swapchain,sub.imageArrayIndex,
      sub.imageRect.offset.x,sub.imageRect.offset.y,sub.imageRect.extent.width,sub.imageRect.extent.height);
    logLine(msg);++reported;
   }
  }
 }
 // Publish only projection layer 0; eye 0/1 must both have known images.
 if(info && info->layers) {
  uint64_t images[2][3]{};
  for(uint32_t li=0;li<info->layerCount;++li){
   const auto* layer=info->layers[li];
   if(!layer || layer->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
   const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(layer);
   if(projection->viewCount>=2 && projection->views){
    // The projection rect is the eye image the game renders, unlike UI swapchain pairs.
    const auto& extent=projection->views[0].subImage.imageRect.extent;
    if(extent.width>0 && extent.height>0)beamng_d3d12::setEyeSize(uint32_t(extent.width),uint32_t(extent.height));
    std::lock_guard<std::mutex> guard(stateMutex);
    for(int eye=0;eye<2;++eye){auto sc=m843Swapchains.find(projection->views[eye].subImage.swapchain);if(sc!=m843Swapchains.end())sc->second.eye=eye;}
    for(int eye=0;eye<2;++eye){auto it=m75SwapchainImage.find(projection->views[eye].subImage.swapchain);
     if(it!=m75SwapchainImage.end())for(size_t j=0;j<it->second.size()&&j<3;++j)images[eye][j]=it->second[j];}
   }
   break;
  }
  if(images[0][0] && images[1][0] && images[0][0]!=images[1][0]){
   m75Publish(images);
   beamng_d3d12::setEyeImages(&images[0][0],6);
   static unsigned n=0;if(m817Verbose() && (++n%180)==1){char b[256]{};sprintf_s(b,"M7.5a OpenXR eyeImages L=%p R=%p",(void*)(uintptr_t)images[0][0],(void*)(uintptr_t)images[1][0]);logLine(b);}
  }
 }
 beamng_d3d12::frame();
 const auto c=frameContext(session);
 return c.turbo?c.turbo->end(info):d.endFrame(session,info);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeGetInstanceProcAddr(XrInstance instance,const char* name,PFN_xrVoidFunction* function);
// Loader entrypoint: chain to the next API layer, enable supported optional
// extensions, and resolve only downstream pointers (never our own interceptors).
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeCreateApiLayerInstance(const XrInstanceCreateInfo* info,const XrApiLayerCreateInfo* layerInfo,XrInstance* instance);

// M8.42: D3D12 foveation uses the same [Foveation]/[Tracking] settings as the Vulkan layer.
float m842Size(const char* name,float fallback){
 char v[32]{};const DWORD n=beamng_config::read(name,v,sizeof(v));
 if(!n||n>=sizeof(v))return fallback;
 char* end=nullptr;const float value=strtof(v,&end);
 return end!=v && !*end && std::isfinite(value) && value>=0.f && value<=1.f ? value : fallback;
}
uint8_t m842Rate(const char* name,uint8_t fallback){
 char v[16]{};if(!beamng_config::read(name,v,sizeof(v)))return fallback;
 return !strcmp(v,"1x1")?beamng_ffr::rate(1,1):!strcmp(v,"2x2")?beamng_ffr::rate(2,2):!strcmp(v,"4x4")?beamng_ffr::rate(4,4):fallback;
}
// M8.43: [Scissor], [LensMask] and [Debug] on D3D12, same settings as the Vulkan layer.
double m843Fraction(const char* name,double fallback){
 char v[32]{};const DWORD n=beamng_config::read(name,v,sizeof(v));
 if(!n||n>=sizeof(v))return fallback;
 char* end=nullptr;const double value=strtod(v,&end);
 return end!=v && !*end && std::isfinite(value) ? std::clamp(value,0.0,0.49) : fallback;
}
// M8.50: [Renderer] VRDrawDistance replaces the 10000 m far distance of the game's VR views.
// Called once the game's OpenXR instance exists (the VR code is loaded by then).
// The launcher passes the saved in-game pick in the same variable when the INI
// value is unchanged; "off" leaves the variable unset and nothing is scanned.
void m850VrDrawDistance(){
 static std::once_flag once;
 std::call_once(once,[]{
  char v[32]{};const DWORD n=beamng_config::read("BEAMNG_VRS_VR_DRAW_DISTANCE",v,sizeof(v));
  if(!n||n>=sizeof(v))return;
  char* end=nullptr;const double distance=strtod(v,&end);
  if(end!=v && !*end && std::isfinite(distance) && distance>0)beamng_far::patchRunningGame(logLine,distance);
 });
}
// M8.51: the launcher names a file (BEAMNG_VRS_VR_DRAW_DISTANCE_FILE) that the game's
// Lua UI app and radial menu write as "distance=N". A new value is applied live.
// Called from xrWaitFrame every 45 frames (~0.5 s at 90 Hz): one file-time check
// per call, and the file is only read when its write time changes.
void m851LiveDrawDistance(){
 static const std::wstring path=[]{
  std::wstring p(32768,L'\0');const DWORD n=GetEnvironmentVariableW(L"BEAMNG_VRS_VR_DRAW_DISTANCE_FILE",p.data(),DWORD(p.size()));
  p.resize(n<p.size()?n:0);return p;
 }();
 if(path.empty())return;
 // xrWaitFrame may run on more than one thread; a busy poll is just skipped.
 static std::mutex pollMutex;
 std::unique_lock<std::mutex> lock(pollMutex,std::try_to_lock);
 if(!lock.owns_lock())return;
 static FILETIME seen{};
 static double applied=0;
 WIN32_FILE_ATTRIBUTE_DATA a{};
 if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a) || !CompareFileTime(&a.ftLastWriteTime,&seen))return;
 const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 char text[128]{};DWORD size=0;
 const BOOL ok=ReadFile(file,text,sizeof(text)-1,&size,nullptr);
 CloseHandle(file);
 if(!ok)return;
 text[size]='\0';
 const char* at=strstr(text,"distance=");
 if(!at)return; // Possibly read mid-write: retried at the next poll.
 char* end=nullptr;const double distance=strtod(at+9,&end);
 if(end==at+9 || !std::isfinite(distance) || distance<100 || distance>10000)return;
 // Only a complete, valid value marks this write time as handled.
 seen=a.ftLastWriteTime;
 if(distance==applied)return;
 applied=distance;
 char msg[128]{};
 if(beamng_far::setDistance(distance))sprintf_s(msg,sizeof(msg),"M8.51 VR draw distance live: %.0f m",distance);
 else sprintf_s(msg,sizeof(msg),"M8.51 VR draw distance WARNING: %.0f m not applied (VRDrawDistance off or nothing patched at start)",distance);
 logLine(msg);
}
beamng_d3d12::Effects m843Effects(){
 beamng_d3d12::Effects e{};
 e.crop=enabled("BEAMNG_VRS_SCISSOR_ENABLE");
 const double top=m843Fraction("BEAMNG_VRS_SCISSOR_TOP",.30),bottom=m843Fraction("BEAMNG_VRS_SCISSOR_BOTTOM",.30);
 const double left=m843Fraction("BEAMNG_VRS_SCISSOR_LEFT",0),right=m843Fraction("BEAMNG_VRS_SCISSOR_RIGHT",0);
 e.margins[0]={top,bottom,m843Fraction("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT",left),m843Fraction("BEAMNG_VRS_SCISSOR_LEFT_EYE_RIGHT",right)};
 e.margins[1]={top,bottom,m843Fraction("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT",left),m843Fraction("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT",right)};
 e.lensMask=enabled("BEAMNG_VRS_LENS_MASK");
 char v[32]{};const DWORD n=beamng_config::read("BEAMNG_VRS_LENS_MASK_SIZE",v,sizeof(v));
 if(n&&n<sizeof(v)){char* end=nullptr;const double size=strtod(v,&end);
  if(end!=v && !*end && std::isfinite(size) && size>=beamng_lens::minSize && size<=beamng_lens::maxSize)e.lensSize=size;}
 e.outline=enabled("BEAMNG_VRS_DEBUG_OUTLINE");
 return e;
}
beamng_d3d12::Foveation m842Foveation(bool gazeAvailable){
 beamng_d3d12::Foveation f{};
 f.enabled=enabled("BEAMNG_VRS_ENABLE_FEATURES");
 char mode[16]{};
 f.gaze=gazeAvailable && enabled("BEAMNG_EYE_SAMPLE") &&
   (!beamng_config::read("BEAMNG_VRS_FOVEATION_MODE",mode,sizeof(mode)) || !strcmp(mode,"eye"));
 f.center=m842Size("BEAMNG_VRS_CENTER_RADIUS",.25f);
 f.middle=std::max(f.center,m842Size("BEAMNG_VRS_MID_RADIUS",.55f));
 f.middleRate=m842Rate("BEAMNG_VRS_MID_RATE",beamng_ffr::rate(2,2));
 f.outerRate=m842Rate("BEAMNG_VRS_OUTER_RATE",beamng_ffr::rate(4,4));
 char shape[16]{};
 if(beamng_config::read("BEAMNG_VRS_SHAPE",shape,sizeof(shape)))
  f.shape=!_stricmp(shape,"square")?beamng_ffr::Shape::Square:!_stricmp(shape,"ellipse")?beamng_ffr::Shape::Ellipse:beamng_ffr::Shape::Circle;
 char grace[16]{};
 if(beamng_config::read("BEAMNG_VRS_GAZE_GRACE_MS",grace,sizeof(grace))){
  char* end=nullptr;const unsigned long ms=strtoul(grace,&end,10);
  if(end!=grace && !*end && ms<=1000)f.graceMs=uint32_t(ms);
 }
 return f;
}
// M7.2b diagnostic gaze action. Failures never block BeamNG's original calls.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeCreateSession(XrInstance inst,const XrSessionCreateInfo* ci,XrSession* session){
 Dispatch d{}; {std::lock_guard<std::mutex> g(stateMutex);auto it=instances.find(inst);if(it==instances.end())return XR_ERROR_HANDLE_INVALID;d=it->second;}
 if(!d.createSession)return XR_ERROR_FUNCTION_UNSUPPORTED;
 XrResult r=d.createSession(inst,ci,session);
 if(XR_SUCCEEDED(r) && session && m818Enabled()){
  // Invalidate on session creation, BEFORE any eye Vulkan work can start.
  m818Sessions[*session]=M818SessionGeometry{};
  m819InvalidateShared();
  m818Sessions[*session].invalidated=true;
 }
 logResult("M7.2b xrCreateSession",r);
 if(XR_SUCCEEDED(r) && session){
  GazeSession gs{};gs.instance=inst;
  bool vulkan=false,d3d12=false;
  if(ci)for(auto* next=reinterpret_cast<const XrBaseInStructure*>(ci->next);next;next=next->next){
   if(next->type==XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)vulkan=true;
   if(next->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR){
    d3d12=true;const auto* b=reinterpret_cast<const D3D12Binding*>(next);
    char msg[200]{};sprintf_s(msg,sizeof(msg),"M8.40 graphics=D3D12 device=%p queue=%p",b->device,b->queue);logLine(msg);
    // M8.42 foveation, M8.44 FOV crop and M8.43 lens mask/outline when enabled; M8.41 report in diagnostics runs.
    auto fx=m843Effects();
    if(fx.crop){
     fx.crop=false;fx.fovCrop=true;
     {std::lock_guard<std::mutex> g(stateMutex);m844Session=*session;m844Margins={fx.margins[0],fx.margins[1]};}
     m844Active=true;
     char m[240]{};sprintf_s(m,sizeof(m),"M8.44 FOV crop active: kept width %.0f%%/%.0f%% height %.0f%% of each eye",
       100*(1-fx.margins[0].left-fx.margins[0].right),100*(1-fx.margins[1].left-fx.margins[1].right),100*(1-fx.margins[0].top-fx.margins[0].bottom));
     logLine(m);
    }else m844Active=false;
    beamng_d3d12::attach(b->device,b->queue,logLine,m817Verbose(),m842Foveation(d.gazeExtension),fx);
   }
  }
  if(vulkan)logLine("M8.40 graphics=Vulkan");
  else if(!d3d12)logLine("M8.40 graphics=other (only gaze and eye resolution are available)");
  // Frame timing is graphics-API neutral; D3D12 still needs in-headset validation.
  if(enabled("BEAMNG_VRS_TURBO_MODE")){
   if((vulkan||d3d12) && d.waitFrame && d.beginFrame && d.endFrame && d.beginSession && d.endSession && d.destroySession && d.destroySwapchain){
    try {gs.turbo=std::make_shared<beamng_turbo::Scheduler>(*session,d.waitFrame,d.beginFrame,d.endFrame,true);}
    catch(...) {logLine("M8.31 TURBO WARNING: scheduler allocation failed; retaining normal timing");}
   }else logLine("M8.31 TURBO unavailable: requires Vulkan or D3D12 session and complete frame/lifecycle callbacks");
  }
  if(d.gazeExtension && enabled("BEAMNG_EYE_SAMPLE") && d.createReferenceSpace){
   XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
   si.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;si.poseInReferenceSpace.orientation.w=1.f;
   logResult("M7.2b local space",d.createReferenceSpace(*session,&si,&gs.localSpace));
  }
  {std::lock_guard<std::mutex> g(stateMutex);sessions[*session]=gs;}
 }
 return r;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeAttachSessionActionSets(XrSession session,const XrSessionActionSetsAttachInfo* info){
 Dispatch d{};GazeSession gs{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end()){gs=it->second;d=instances[it->second.instance];}
 else if(!instances.empty())d=instances.begin()->second;}
 if(!d.attachSets)return XR_ERROR_FUNCTION_UNSUPPORTED;
 char msg[160]{};sprintf_s(msg,sizeof(msg),"M7.2b xrAttachSessionActionSets appCount=%u",info?info->countActionSets:0);logLine(msg);
 bool add=false;
 if(info && gs.localSpace!=XR_NULL_HANDLE && d.createActionSet && d.createAction && d.stringToPath && d.suggestBindings){
  XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy_s(asci.actionSetName,"beamng_eye_probe");
  strcpy_s(asci.localizedActionSetName,"BeamNG Eye Probe");
  asci.priority=0;
  XrResult r=d.createActionSet(gs.instance,&asci,&gs.set);
  logResult("M7.2b xrCreateActionSet",r);
  if(XR_SUCCEEDED(r)){
   XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
   aci.actionType=XR_ACTION_TYPE_POSE_INPUT;
   strcpy_s(aci.actionName,"eye_gaze");
   strcpy_s(aci.localizedActionName,"Eye Gaze");
   r=d.createAction(gs.set,&aci,&gs.action);
   logResult("M7.2b xrCreateAction",r);
   if(XR_SUCCEEDED(r)){
    XrPath profile=XR_NULL_PATH,path=XR_NULL_PATH;
    XrResult rp=d.stringToPath(gs.instance,"/interaction_profiles/ext/eye_gaze_interaction",&profile);
    XrResult rb=d.stringToPath(gs.instance,"/user/eyes_ext/input/gaze_ext/pose",&path);
    logResult("M7.2b profile path",rp);logResult("M7.2b gaze path",rb);
    if(XR_SUCCEEDED(rp)&&XR_SUCCEEDED(rb)){
     XrActionSuggestedBinding binding{gs.action,path};
     XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
     sb.interactionProfile=profile;sb.countSuggestedBindings=1;sb.suggestedBindings=&binding;
     r=d.suggestBindings(gs.instance,&sb);
     logResult("M7.2b suggest gaze binding",r);
     add=XR_SUCCEEDED(r);
    }
   }
  }
 }
 std::vector<XrActionSet> sets;
 XrSessionActionSetsAttachInfo modified{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
 const XrSessionActionSetsAttachInfo* forwarded=info;
 if(add){
  sets.assign(info->actionSets,info->actionSets+info->countActionSets);
  sets.push_back(gs.set);
  modified=*info;modified.countActionSets=(uint32_t)sets.size();modified.actionSets=sets.data();
  forwarded=&modified;
 }
 XrResult result=d.attachSets(session,forwarded);
 logResult("M7.2b attach",result);
 if(XR_SUCCEEDED(result) && add && d.createActionSpace){
  XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  space.action=gs.action;space.poseInActionSpace.orientation.w=1.f;
  XrResult r=d.createActionSpace(session,&space,&gs.gazeSpace);
  logResult("M7.2b xrCreateActionSpace",r);
  gs.ready=XR_SUCCEEDED(r);
 }
 if(XR_FAILED(result) && add){
  logLine("M7.2b augmented attach failed; retrying original app attach");
  result=d.attachSets(session,info);
  logResult("M7.2b original attach retry",result);
  gs.ready=false;
 }
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end())it->second=gs;}
 return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeSyncActions(XrSession session,const XrActionsSyncInfo* info){
 Dispatch d{};GazeSession gs{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end()){gs=it->second;d=instances[it->second.instance];}
 else if(!instances.empty())d=instances.begin()->second;}
 if(!d.syncActions)return XR_ERROR_FUNCTION_UNSUPPORTED;
 std::vector<XrActiveActionSet> active;
 XrActionsSyncInfo modified{XR_TYPE_ACTIONS_SYNC_INFO};
 const XrActionsSyncInfo* forwarded=info;
 if(info && gs.ready){
  active.assign(info->activeActionSets,info->activeActionSets+info->countActiveActionSets);
  active.push_back({gs.set,XR_NULL_PATH});
  modified=*info;modified.countActiveActionSets=(uint32_t)active.size();modified.activeActionSets=active.data();
  forwarded=&modified;
 }
 XrResult r=d.syncActions(session,forwarded);
 if(gs.ready && XR_SUCCEEDED(r) && d.getActionStatePose && d.locateSpace && gs.displayTime){
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};gi.action=gs.action;
  XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
  XrResult pr=d.getActionStatePose(session,&gi,&pose);
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  XrResult lr=XR_ERROR_RUNTIME_FAILURE;
  if(XR_SUCCEEDED(pr)&&pose.isActive)lr=d.locateSpace(gs.gazeSpace,gs.localSpace,gs.displayTime,&loc);
  if(XR_SUCCEEDED(lr) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
   {publishGaze(loc,gs.displayTime); gs.lastGaze=loc.pose; gs.lastGazeTime=gs.displayTime;}
  if((++gs.samples%90)==1){
   char line[320]{};
   sprintf_s(line,sizeof(line),"M7.2b gaze state=%d active=%u locate=%d flags=0x%llX quat=(%.4f,%.4f,%.4f,%.4f)",
    (int)pr,(unsigned)pose.isActive,(int)lr,(unsigned long long)loc.locationFlags,
    loc.pose.orientation.x,loc.pose.orientation.y,loc.pose.orientation.z,loc.pose.orientation.w);
   logLine(line);
  }
  {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end()){it->second.samples=gs.samples;it->second.lastGaze=gs.lastGaze;it->second.lastGazeTime=gs.lastGazeTime;}}
 }
 return r;
}
// App-facing timing (real or extrapolated) is also the time used for gaze/view
// location. Raw worker results never independently refresh projected-gaze IPC.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeWaitFrame(XrSession session,const XrFrameWaitInfo* info,XrFrameState* state){
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end())d=instances[it->second.instance];
 else if(!instances.empty())d=instances.begin()->second;}
 if(!d.waitFrame)return XR_ERROR_FUNCTION_UNSUPPORTED;
 const auto c=frameContext(session);
 LARGE_INTEGER waitStart{},waitEnd{},freq{};QueryPerformanceCounter(&waitStart);
 XrResult r=c.turbo?c.turbo->wait(info,state):d.waitFrame(session,info,state);
 QueryPerformanceCounter(&waitEnd);QueryPerformanceFrequency(&freq);
 // M8.52: how long the runtime held the game here, for the M8.47 frame-pacing verdict.
 if(XR_SUCCEEDED(r))beamng_d3d12::frameWaited(double(waitEnd.QuadPart-waitStart.QuadPart)*1000.0/double(freq.QuadPart),
   state&&state->predictedDisplayPeriod>0?double(state->predictedDisplayPeriod)/1e6:0.0);
 if(XR_SUCCEEDED(r)&&state){
  std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);
  if(it!=sessions.end())it->second.displayTime=state->predictedDisplayTime;
 }
 static unsigned calls=0,drawDistancePolls=0;
 if(++drawDistancePolls%45==0)m851LiveDrawDistance();
 if(m817Verbose() && (++calls%300)==1){char msg[160]{};sprintf_s(msg,sizeof(msg),"M7.2b xrWaitFrame result=%d predictedDisplayTime=%lld",(int)r,(long long)(state?state->predictedDisplayTime:0));logLine(msg);}
 return r;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeCreateReferenceSpace(XrSession session,const XrReferenceSpaceCreateInfo* info,XrSpace* space){
 PFN_xrCreateReferenceSpace fn{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end())fn=instances[it->second.instance].createReferenceSpace;
 else if(!instances.empty())fn=instances.begin()->second.createReferenceSpace;}
 if(!fn)return XR_ERROR_FUNCTION_UNSUPPORTED;
 XrResult r=fn(session,info,space);
 char msg[128]{};sprintf_s(msg,sizeof(msg),"M7.2b xrCreateReferenceSpace type=%d result=%d",info?(int)info->referenceSpaceType:-1,(int)r);logLine(msg);
 return r;
}


// M7.3b: project gaze into each eye's actual OpenXR FOV. Diagnostic only.
struct V3 {float x,y,z;};
V3 rotateQ(const XrQuaternionf& q,V3 v){
 V3 u{q.x,q.y,q.z};
 V3 t{2.f*(u.y*v.z-u.z*v.y),2.f*(u.z*v.x-u.x*v.z),2.f*(u.x*v.y-u.y*v.x)};
 return {v.x+q.w*t.x+u.y*t.z-u.z*t.y,v.y+q.w*t.y+u.z*t.x-u.x*t.z,v.z+q.w*t.z+u.x*t.y-u.y*t.x};
}
// M8.44: the recommended size shrinks with the kept fraction of each eye.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeEnumerateViewConfigurationViews(XrInstance instance,XrSystemId system,
  XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* views){
 Dispatch d{};std::array<beamng_scissor::Margins,2> margins{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=instances.find(instance);if(it!=instances.end())d=it->second;margins=m844Margins;}
 if(!d.enumerateViews)return XR_ERROR_FUNCTION_UNSUPPORTED;
 const XrResult r=d.enumerateViews(instance,system,type,capacity,count,views);
 if(XR_SUCCEEDED(r)&&m844Active&&views&&count&&*count>=2&&capacity>=2&&type==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO){
  for(int i=0;i<2;++i){
   auto& v=views[i];const auto& m=margins[i];
   v.recommendedImageRectWidth=std::max<uint32_t>(16,uint32_t(std::lround(v.recommendedImageRectWidth*(1.0-m.left-m.right))));
   v.recommendedImageRectHeight=std::max<uint32_t>(16,uint32_t(std::lround(v.recommendedImageRectHeight*(1.0-m.top-m.bottom))));
  }
  char msg[200]{};sprintf_s(msg,sizeof(msg),"M8.44 FOV crop recommended size L=%ux%u R=%ux%u",
    views[0].recommendedImageRectWidth,views[0].recommendedImageRectHeight,views[1].recommendedImageRectWidth,views[1].recommendedImageRectHeight);
  logLine(msg);
 }
 return r;
}
// Image rows and columns are linear in the tangent of the view angle.
void m844CropFov(XrFovf& fov,const beamng_scissor::Margins& m){
 const double l=tan(fov.angleLeft),r=tan(fov.angleRight),u=tan(fov.angleUp),dn=tan(fov.angleDown);
 fov.angleLeft=float(atan(l+m.left*(r-l)));fov.angleRight=float(atan(r-m.right*(r-l)));
 fov.angleUp=float(atan(u-m.top*(u-dn)));fov.angleDown=float(atan(dn+m.bottom*(u-dn)));
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeLocateViews(XrSession session,const XrViewLocateInfo* info,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views){
 Dispatch d{};GazeSession gs{};bool crop=false;std::array<beamng_scissor::Margins,2> margins{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=sessions.find(session);if(it!=sessions.end()){gs=it->second;d=instances[it->second.instance];}else if(!instances.empty())d=instances.begin()->second;
  crop=m844Active&&session==m844Session;margins=m844Margins;}
 if(!d.locateViews)return XR_ERROR_FUNCTION_UNSUPPORTED;
 XrResult r=d.locateViews(session,info,state,capacity,count,views);
 // Everything below, the gaze projection included, then sees the kept views.
 if(crop&&XR_SUCCEEDED(r)&&info&&info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO&&views&&count&&*count>=2&&capacity>=2){
  for(int i=0;i<2;++i)m844CropFov(views[i].fov,margins[i]);
  static std::atomic<bool> logged{false};
  if(!logged.exchange(true)){char msg[240]{};sprintf_s(msg,sizeof(msg),"M8.44 FOV crop L=(%.3f,%.3f,%.3f,%.3f) rad",
    views[0].fov.angleLeft,views[0].fov.angleRight,views[0].fov.angleUp,views[0].fov.angleDown);logLine(msg);}
 }
 if(XR_SUCCEEDED(r)&&info&&views&&count&&*count>=2&&capacity>=2)
   m818Publish(session,d,views);
 // Capacity queries do not invalidate a previous sample. An actual failed
 // projection publishes invalidity promptly, allowing the existing grace.
 if(!info||!views||capacity<2||!gs.ready)return r;
 float u[2]{},v[2]{};
 auto invalid=[&](){publishProjectedGaze(u,v,0,0,info->displayTime);return r;};
 if(XR_FAILED(r)||!count||*count<2||!state)return invalid();
 // The eye view poses must be in the view space supplied by the application, using xrLocateSpace.
 if(!(state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))return invalid();
 if(!gs.gazeSpace||!d.locateSpace)return invalid();
 XrSpaceLocation gaze{XR_TYPE_SPACE_LOCATION};
 if(XR_FAILED(d.locateSpace(gs.gazeSpace,info->space,info->displayTime,&gaze)) || !(gaze.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))return invalid();
 const uint64_t poseTick=GetTickCount64();
 uint32_t mask=0;
 V3 forward=rotateQ(gaze.pose.orientation,{0.f,0.f,-1.f});
 for(int i=0;i<2;++i){
  const auto& eye=views[i];auto q=eye.pose.orientation;
  XrQuaternionf inverse{-q.x,-q.y,-q.z,q.w};
  V3 ray=rotateQ(inverse,forward);
  if(ray.z>=-0.0001f)continue;
  float slopeX=ray.x/-ray.z,slopeY=ray.y/-ray.z;
  float left=tanf(eye.fov.angleLeft),right=tanf(eye.fov.angleRight);
  float down=tanf(eye.fov.angleDown),up=tanf(eye.fov.angleUp);
  if(right<=left||up<=down)continue;
  u[i]=(slopeX-left)/(right-left);v[i]=(up-slopeY)/(up-down);
  if(std::isfinite(u[i])&&std::isfinite(v[i]))mask|=(1u<<i);
 }
 publishProjectedGaze(u,v,mask,poseTick,info->displayTime);
 beamng_d3d12::setGaze(u,v,mask);
 if(gazeShared){
  auto* s=gazeShared;InterlockedIncrement(&s->sequence);MemoryBarrier();
  for(int i=0;i<2;++i){s->eyeU[i]=u[i];s->eyeV[i]=v[i];}
  s->eyeValidMask=mask;MemoryBarrier();InterlockedIncrement(&s->sequence);
 }
 static unsigned logs=0;if(m817Verbose() && (++logs%180)==1){char msg[240]{};sprintf_s(msg,sizeof(msg),"M8.27 projected gaze mask=%u L=(%.3f,%.3f) R=(%.3f,%.3f)",mask,u[0],v[0],u[1],v[1]);logLine(msg);}
 return r;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeGetSystemProperties(XrInstance instance,XrSystemId system,XrSystemProperties* properties){
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=instances.find(instance);if(it==instances.end())return XR_ERROR_HANDLE_INVALID;d=it->second;}
 if(!d.getSystemProperties)return XR_ERROR_FUNCTION_UNSUPPORTED;
 if(!d.gazeExtension || !properties)return d.getSystemProperties(instance,system,properties);
 XrSystemEyeGazeInteractionPropertiesEXT gaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
 gaze.next=properties->next;properties->next=&gaze;
 XrResult r=d.getSystemProperties(instance,system,properties);
 properties->next=gaze.next;
 logResult("xrGetSystemProperties",r);
 if(XR_SUCCEEDED(r))logLine(gaze.supportsEyeGazeInteraction ? "System supports eye gaze interaction" : "System reports NO eye gaze interaction support");
 return r;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeDestroyInstance(XrInstance instance){
 Dispatch d{};std::vector<std::shared_ptr<beamng_turbo::Scheduler>> workers;
 {std::lock_guard<std::mutex> g(stateMutex);auto it=instances.find(instance);if(it==instances.end())return XR_ERROR_HANDLE_INVALID;d=it->second;
  for(const auto& pair:sessions)if(pair.second.instance==instance && pair.second.turbo)workers.push_back(pair.second.turbo);
 }
 for(auto& worker:workers){worker->stop();turboReport(worker);}
 const auto result=d.destroy?d.destroy(instance):XR_ERROR_FUNCTION_UNSUPPORTED;
 if(XR_SUCCEEDED(result)){
  std::lock_guard<std::mutex> g(stateMutex);instances.erase(instance);
  std::vector<XrSession> owned;
  for(const auto& pair:sessions)if(pair.second.instance==instance)owned.push_back(pair.first);
  for(auto session:owned)eraseSessionState(session);
 }
 return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeGetInstanceProcAddr(XrInstance instance,const char* name,PFN_xrVoidFunction* function){
 if(!name||!function)return XR_ERROR_VALIDATION_FAILURE;
 if(!strcmp(name,"xrGetInstanceProcAddr")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeGetInstanceProcAddr);return XR_SUCCESS;}
 if(!strcmp(name,"xrCreateApiLayerInstance")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeCreateApiLayerInstance);return XR_SUCCESS;}
 if(instance==XR_NULL_HANDLE){
  const auto next=downstreamGetInstanceProcAddr.load();
  if(next)return next(instance,name,function);
  *function=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
 }
 Dispatch d{};
 {std::lock_guard<std::mutex> g(stateMutex);auto it=instances.find(instance);if(it==instances.end())return XR_ERROR_HANDLE_INVALID;d=it->second;}
 if(!strcmp(name,"xrGetSystemProperties")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeGetSystemProperties);return XR_SUCCESS;}
 if(!strcmp(name,"xrCreateSession")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeCreateSession);return XR_SUCCESS;}
 if(!strcmp(name,"xrAttachSessionActionSets")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeAttachSessionActionSets);return XR_SUCCESS;}
 if(!strcmp(name,"xrSyncActions")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeSyncActions);return XR_SUCCESS;}
 if(!strcmp(name,"xrLocateViews")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeLocateViews);return XR_SUCCESS;}
 if(!strcmp(name,"xrEnumerateViewConfigurationViews")){if(!d.enumerateViews)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeEnumerateViewConfigurationViews);return XR_SUCCESS;}
 if(!strcmp(name,"xrBeginFrame")){if(!d.beginFrame)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeBeginFrame);return XR_SUCCESS;}
 if(!strcmp(name,"xrBeginSession")){if(!d.beginSession)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeBeginSession);return XR_SUCCESS;}
 if(!strcmp(name,"xrEndSession")){if(!d.endSession)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeEndSession);return XR_SUCCESS;}
 if(!strcmp(name,"xrDestroySession")){if(!d.destroySession)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeDestroySession);return XR_SUCCESS;}
 if(!strcmp(name,"xrDestroySwapchain")){if(!d.destroySwapchain)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeDestroySwapchain);return XR_SUCCESS;}
 if(!strcmp(name,"xrEndFrame")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeEndFrame);return XR_SUCCESS;}
 if(!strcmp(name,"xrCreateSwapchain")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeCreateSwapchain);return XR_SUCCESS;}
 if(!strcmp(name,"xrEnumerateSwapchainImages")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeEnumerateSwapchainImages);return XR_SUCCESS;}
 if(!strcmp(name,"xrAcquireSwapchainImage")){if(!d.acquireSwapchainImage)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeAcquireSwapchainImage);return XR_SUCCESS;}
 if(!strcmp(name,"xrReleaseSwapchainImage")){if(!d.releaseSwapchainImage)return d.next(instance,name,function);*function=reinterpret_cast<PFN_xrVoidFunction>(probeReleaseSwapchainImage);return XR_SUCCESS;}
 if(!strcmp(name,"xrWaitFrame")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeWaitFrame);return XR_SUCCESS;}
 if(!strcmp(name,"xrCreateReferenceSpace")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeCreateReferenceSpace);return XR_SUCCESS;}
 if(!strcmp(name,"xrDestroyInstance")){*function=reinterpret_cast<PFN_xrVoidFunction>(probeDestroyInstance);return XR_SUCCESS;}
 return d.next(instance,name,function);
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL probeCreateApiLayerInstance(const XrInstanceCreateInfo* info,const XrApiLayerCreateInfo* layerInfo,XrInstance* instance){
 if(!info||!layerInfo||!layerInfo->nextInfo||!instance)return XR_ERROR_VALIDATION_FAILURE;
 auto* next=layerInfo->nextInfo;
 downstreamGetInstanceProcAddr.store(next->nextGetInstanceProcAddr);
 XrApiLayerCreateInfo forwarded=*layerInfo;forwarded.nextInfo=next->next;
 bool appGaze=false;
 bool appVisibilityMask=false;
 for(uint32_t i=0;i<info->enabledExtensionCount;++i){
  if(!strcmp(info->enabledExtensionNames[i],XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))appGaze=true;
  if(!strcmp(info->enabledExtensionNames[i],"XR_KHR_visibility_mask"))appVisibilityMask=true;
 }
 logLine("M7.1 OpenXR probe: xrCreateInstance intercepted");
 logLine(appGaze?"App requested XR_EXT_eye_gaze_interaction":"App did not request XR_EXT_eye_gaze_interaction");
 PFN_xrVoidFunction raw{};
 XrResult lookup=next->nextGetInstanceProcAddr(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",&raw);
 logResult("Lookup xrEnumerateInstanceExtensionProperties",lookup);
 bool runtimeGaze=false;
 bool runtimeVisibilityMask=false;
 if(XR_SUCCEEDED(lookup)&&raw){
  auto enumerate=reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(raw);
  uint32_t count=0;
  XrResult r=enumerate(nullptr,0,&count,nullptr);
  logResult("Enumerate runtime extension count",r);
  if(XR_SUCCEEDED(r)){
   std::vector<XrExtensionProperties> props(count);
   for(auto& p:props){p.type=XR_TYPE_EXTENSION_PROPERTIES;p.next=nullptr;}
   r=enumerate(nullptr,count,&count,props.data());
   logResult("Enumerate runtime extensions",r);
   if(XR_SUCCEEDED(r))for(const auto& p:props){
    if(!strcmp(p.extensionName,XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))runtimeGaze=true;
    if(!strcmp(p.extensionName,"XR_KHR_visibility_mask"))runtimeVisibilityMask=true;
   }
  }
 }
 logLine(runtimeGaze?"Runtime advertises XR_EXT_eye_gaze_interaction":"Runtime gaze extension NOT found (or enumeration unavailable)");
 {char msg[200]{};
  sprintf_s(msg,"M8.17 VISIBILITY_MASK runtimeAdvertised=%u appEnabled=%u diagnosticOnly=%u",
   runtimeVisibilityMask?1u:0u,appVisibilityMask?1u:0u,m818Enabled()?0u:1u);
  logLine(msg);
 }
 bool force=enabled("BEAMNG_EYE_FORCE_EXTENSION");
 XrInstanceCreateInfo modified=*info;
 std::vector<const char*> extensionNames;
 bool injected=false;
 if(force && !appGaze && runtimeGaze){
  extensionNames.assign(info->enabledExtensionNames,info->enabledExtensionNames+info->enabledExtensionCount);
  extensionNames.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
  modified.enabledExtensionCount=static_cast<uint32_t>(extensionNames.size());
  modified.enabledExtensionNames=extensionNames.data();
  injected=true;
  logLine("Opt-in: injecting XR_EXT_eye_gaze_interaction into xrCreateInstance");
 }else if(force){logLine("Opt-in requested, but injection skipped (already enabled or unavailable)");}
 // Explicit opt-in permits use even if BeamNG itself did not enable the extension.
 bool injectedMask=false;
 if(m818Enabled() && runtimeVisibilityMask && !appVisibilityMask){
  if(!injected){extensionNames.assign(info->enabledExtensionNames,info->enabledExtensionNames+info->enabledExtensionCount);}
  extensionNames.push_back("XR_KHR_visibility_mask");
  modified.enabledExtensionCount=static_cast<uint32_t>(extensionNames.size());
  modified.enabledExtensionNames=extensionNames.data();
  injected=true;injectedMask=true;
  logLine("M8.18 XR_MASK_EXTENSION_INJECTED");
 }
 XrResult r=next->nextCreateApiLayerInstance(injected?&modified:info,&forwarded,instance);
 logResult("Next xrCreateApiLayerInstance",r);
 if(XR_FAILED(r)&&injected){
  // Another layer below may reject an extension BeamNG did not ask for: fail open.
  logLine("OpenXR WARNING: instance creation failed with injected extensions; retrying with the game's own request");
  injected=false;injectedMask=false;force=false;
  r=next->nextCreateApiLayerInstance(info,&forwarded,instance);
  logResult("Next xrCreateApiLayerInstance retry",r);
 }
 if(XR_FAILED(r))return r;
 m850VrDrawDistance();
 Dispatch d{};d.next=next->nextGetInstanceProcAddr;d.gazeExtension=appGaze||(force && runtimeGaze);d.visibilityMaskEnabled=appVisibilityMask||injectedMask;
 PFN_xrVoidFunction fn{};
 if(XR_SUCCEEDED(d.next(*instance,"xrGetSystemProperties",&fn)))d.getSystemProperties=reinterpret_cast<PFN_xrGetSystemProperties>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrDestroyInstance",&fn)))d.destroy=reinterpret_cast<PFN_xrDestroyInstance>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrCreateSession",&fn)))d.createSession=reinterpret_cast<PFN_xrCreateSession>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrAttachSessionActionSets",&fn)))d.attachSets=reinterpret_cast<PFN_xrAttachSessionActionSets>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrSyncActions",&fn)))d.syncActions=reinterpret_cast<PFN_xrSyncActions>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrWaitFrame",&fn)))d.waitFrame=reinterpret_cast<PFN_xrWaitFrame>(fn);
 fn=nullptr;if(XR_SUCCEEDED(d.next(*instance,"xrCreateReferenceSpace",&fn)))d.createReferenceSpace=reinterpret_cast<PFN_xrCreateReferenceSpace>(fn);
#define RESOLVE_XR(member, name, type) fn=nullptr; if(XR_SUCCEEDED(d.next(*instance,name,&fn))) d.member=reinterpret_cast<type>(fn);
 RESOLVE_XR(stringToPath,"xrStringToPath",PFN_xrStringToPath)
 RESOLVE_XR(createActionSet,"xrCreateActionSet",PFN_xrCreateActionSet)
 RESOLVE_XR(createAction,"xrCreateAction",PFN_xrCreateAction)
 RESOLVE_XR(suggestBindings,"xrSuggestInteractionProfileBindings",PFN_xrSuggestInteractionProfileBindings)
 RESOLVE_XR(createActionSpace,"xrCreateActionSpace",PFN_xrCreateActionSpace)
 RESOLVE_XR(getActionStatePose,"xrGetActionStatePose",PFN_xrGetActionStatePose)
 RESOLVE_XR(locateSpace,"xrLocateSpace",PFN_xrLocateSpace)
 RESOLVE_XR(locateViews,"xrLocateViews",PFN_xrLocateViews)
 if(d.visibilityMaskEnabled){ RESOLVE_XR(getVisibilityMask,"xrGetVisibilityMaskKHR",PFN_xrGetVisibilityMaskKHR) }
 RESOLVE_XR(endFrame,"xrEndFrame",PFN_xrEndFrame)
 RESOLVE_XR(beginFrame,"xrBeginFrame",PFN_xrBeginFrame)
 RESOLVE_XR(beginSession,"xrBeginSession",PFN_xrBeginSession)
 RESOLVE_XR(endSession,"xrEndSession",PFN_xrEndSession)
 RESOLVE_XR(destroySession,"xrDestroySession",PFN_xrDestroySession)
 RESOLVE_XR(destroySwapchain,"xrDestroySwapchain",PFN_xrDestroySwapchain)
 RESOLVE_XR(createSwapchain,"xrCreateSwapchain",PFN_xrCreateSwapchain)
 RESOLVE_XR(enumerateSwapchainImages,"xrEnumerateSwapchainImages",PFN_xrEnumerateSwapchainImages)
 RESOLVE_XR(acquireSwapchainImage,"xrAcquireSwapchainImage",PFN_xrAcquireSwapchainImage)
 RESOLVE_XR(enumerateViews,"xrEnumerateViewConfigurationViews",PFN_xrEnumerateViewConfigurationViews)
 RESOLVE_XR(releaseSwapchainImage,"xrReleaseSwapchainImage",PFN_xrReleaseSwapchainImage)
 RESOLVE_XR(destroySpace,"xrDestroySpace",PFN_xrDestroySpace)
 RESOLVE_XR(destroyActionSet,"xrDestroyActionSet",PFN_xrDestroyActionSet)
#undef RESOLVE_XR
 {std::lock_guard<std::mutex> g(stateMutex);instances[*instance]=d;}
 logLine("M7.2a OpenXR lifecycle probe attached");
 return r;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loader,const char* layerName,XrNegotiateApiLayerRequest* request){
 if(!loader||!request||!layerName)return XR_ERROR_INITIALIZATION_FAILED;
 if(loader->minInterfaceVersion>1||loader->maxInterfaceVersion<1)return XR_ERROR_INITIALIZATION_FAILED;
 request->layerInterfaceVersion=1;
 request->layerApiVersion=XR_CURRENT_API_VERSION;
 request->getInstanceProcAddr=probeGetInstanceProcAddr;
 request->createApiLayerInstance=probeCreateApiLayerInstance;
 return XR_SUCCESS;
}
