// Exercise the real OpenXR interceptor with a deterministic mock runtime.
#include "../src/openxr_probe.cpp"
#include <cassert>
#include <iostream>
namespace {
bool failViews=false,failPose=false;
float cant=0; // radians each eye view is rotated outward about +y
float yaw=0;  // radians both eye views (the head) are turned left about +y
XRAPI_ATTR XrResult XRAPI_CALL locateViews(XrSession,const XrViewLocateInfo*,XrViewState* state,
 uint32_t capacity,uint32_t* count,XrView* views){
 if(failViews)return XR_ERROR_RUNTIME_FAILURE;
 *count=2;
 if(capacity>=2 && views){
  state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for(unsigned i=0;i<2;++i){
   const float half=(yaw+(i?-cant:cant))*.5f; // left eye turns left (+y), right eye right
   views[i].pose.orientation={0,sinf(half),0,cosf(half)};views[i].fov={-.7f,.7f,.7f,-.7f};
  }
 }
 return XR_SUCCESS;
}
// Writes a Bigscreen-layout sample: combined gaze @24, valid flag @48, and a
// counter in the left-eye slot so consecutive samples differ like live data.
void writeBigscreen(uint8_t* p,float x,float y,float z,int32_t valid){
 static float noise=0;noise+=.001f;
 memcpy(p,&noise,sizeof(noise));
 const float g[3]{x,y,z};memcpy(p+24,g,sizeof(g));memcpy(p+48,&valid,sizeof(valid));
}
XRAPI_ATTR XrResult XRAPI_CALL locateSpace(XrSpace,XrSpace,XrTime,XrSpaceLocation* loc){
 if(failPose)return XR_ERROR_RUNTIME_FAILURE;
 loc->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
 loc->pose.orientation={0,0,0,1};return XR_SUCCESS;
}
}
int main(){
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","0");
 // A per-process mapping name keeps the test independent of a running BeyondHID.
 SetEnvironmentVariableA("BEAMNG_EYE_SAMPLE","1");SetEnvironmentVariableA("BEAMNG_VRS_BIGSCREEN_GAZE","1");
 static wchar_t mappingName[64]{};swprintf_s(mappingName,L"BeamNGVRSTestGaze%lu",GetCurrentProcessId());
 m853MappingName=mappingName;m853RetryMs=0;m853StaleMs=50;
 const auto inst=reinterpret_cast<XrInstance>(uintptr_t(1));
 const auto session=reinterpret_cast<XrSession>(uintptr_t(2));
 const auto space=reinterpret_cast<XrSpace>(uintptr_t(3));
 Dispatch dispatch{};dispatch.locateViews=locateViews;dispatch.locateSpace=locateSpace;
 instances[inst]=dispatch;
 GazeSession gs{};gs.instance=inst;gs.ready=true;gs.gazeSpace=space;sessions[session]=gs;
 XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};info.space=space;info.displayTime=1234;
 XrViewState state{XR_TYPE_VIEW_STATE};uint32_t count=0;
 XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
 // Projection publication no longer depends on earlier raw diagnostic data.
 assert(!gazeShared);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared && m827GazeShared->eyeValidMask==3);
 assert(m827GazeShared->eyeU[0]==.5f && m827GazeShared->eyeV[1]==.5f);
 assert(m827GazeShared->poseTime==1234 && m827GazeShared->projectionTime==1234);
 const LONG sequence=m827GazeShared->sequence;
 const uint64_t projectionTick=m827GazeShared->projectionTickMs;
 XrSpaceLocation raw{XR_TYPE_SPACE_LOCATION};raw.pose.orientation.w=1;
 publishGaze(raw,1235);publishGaze(raw,1236);
 assert(gazeShared && m827GazeShared->projectionTickMs==projectionTick);
 assert(m827GazeShared->sequence==sequence && m827GazeShared->projectionTime==1234);
 // A count-only query keeps the completed projection intact.
 assert(probeLocateViews(session,&info,&state,0,&count,nullptr)==XR_SUCCESS);
 assert(m827GazeShared->sequence==sequence && m827GazeShared->eyeValidMask==3);
 failPose=true;
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==0 && m827GazeShared->poseTickMs==0);
 failPose=false;failViews=true;
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_ERROR_RUNTIME_FAILURE);
 assert(m827GazeShared->eyeValidMask==0);
 failViews=false;info.displayTime=1240;
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->projectionTime==1240);
 assert(!(m827GazeShared->sequence&1));
 // M8.53: live Bigscreen gaze replaces OpenXR gaze, even while OpenXR fails.
 HANDLE bs=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,mappingName);
 auto* bsData=static_cast<uint8_t*>(MapViewOfFile(bs,FILE_MAP_WRITE,0,0,4096));
 assert(bs && bsData);
 failPose=true;
 writeBigscreen(bsData,.2f,0,-.98f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->eyeU[0]>.6f && m827GazeShared->eyeU[1]>.6f);
 assert(fabsf(m827GazeShared->eyeV[0]-.5f)<.001f);
 // +y is up, so looking up moves v toward the top of the image.
 writeBigscreen(bsData,0,.3f,-.95f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->eyeV[0]<.4f);
 // Canted views: straight-ahead gaze lands symmetrically off-center per eye.
 cant=.2f;writeBigscreen(bsData,0,0,-1,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->eyeU[0]>.55f && m827GazeShared->eyeU[1]<.45f);
 assert(fabsf(m827GazeShared->eyeU[0]+m827GazeShared->eyeU[1]-1.f)<.001f);
 cant=0;
 // Head-relative gaze turns with the head: straight ahead stays centered.
 yaw=.4f;writeBigscreen(bsData,0,0,-1,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3);
 assert(fabsf(m827GazeShared->eyeU[0]-.5f)<.001f && fabsf(m827GazeShared->eyeU[1]-.5f)<.001f);
 // Gaze to the right of a turned head still lands right of center.
 writeBigscreen(bsData,.2f,0,-.98f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeU[0]>.6f && m827GazeShared->eyeU[1]>.6f);
 yaw=0;
 // Invalid samples while the mapping is live publish no gaze (grace applies),
 // never the OpenXR fallback.
 failPose=false;
 writeBigscreen(bsData,.2f,0,-.98f,0);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==0);
 writeBigscreen(bsData,0,0,1,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==0);
 writeBigscreen(bsData,NAN,0,-1,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==0);
 // A lasting invalid state falls back to OpenXR gaze (centered mock) ...
 m853InvalidMs=50;Sleep(120);
 writeBigscreen(bsData,.2f,0,-.98f,2);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->eyeU[0]==.5f);
 // ... until valid samples return.
 m853InvalidMs=1000;
 writeBigscreen(bsData,.2f,0,-.98f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeU[0]>.6f);
 // Frozen data stops counting as live: OpenXR gaze takes over (centered mock).
 writeBigscreen(bsData,.2f,0,-.98f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeU[0]>.6f);
 Sleep(120);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeValidMask==3 && m827GazeShared->eyeU[0]==.5f);
 // A reopened mapping with the same frozen bytes stays stale.
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeU[0]==.5f);
 // Fresh data is picked up again.
 writeBigscreen(bsData,.2f,0,-.98f,1);
 assert(probeLocateViews(session,&info,&state,2,&count,views)==XR_SUCCESS);
 assert(m827GazeShared->eyeU[0]>.6f);
 UnmapViewOfFile(bsData);CloseHandle(bs);
 UnmapViewOfFile(m827GazeShared);CloseHandle(m827GazeMapping);
 UnmapViewOfFile(gazeShared);CloseHandle(gazeMapping);
 std::cout<<"Real OpenXR interceptor: projection publication, independent freshness, failure and recovery passed\n";
}
