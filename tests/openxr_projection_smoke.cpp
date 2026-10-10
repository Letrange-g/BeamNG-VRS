// Exercise the real OpenXR interceptor with a deterministic mock runtime.
#include "../src/openxr_probe.cpp"
#include <cassert>
#include <iostream>
namespace {
bool failViews=false,failPose=false;
XRAPI_ATTR XrResult XRAPI_CALL locateViews(XrSession,const XrViewLocateInfo*,XrViewState* state,
 uint32_t capacity,uint32_t* count,XrView* views){
 if(failViews)return XR_ERROR_RUNTIME_FAILURE;
 *count=2;
 if(capacity>=2 && views){
  state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for(unsigned i=0;i<2;++i){views[i].pose.orientation={0,0,0,1};views[i].fov={-.7f,.7f,.7f,-.7f};}
 }
 return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL locateSpace(XrSpace,XrSpace,XrTime,XrSpaceLocation* loc){
 if(failPose)return XR_ERROR_RUNTIME_FAILURE;
 loc->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
 loc->pose.orientation={0,0,0,1};return XR_SUCCESS;
}
}
int main(){
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","0");
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
 UnmapViewOfFile(m827GazeShared);CloseHandle(m827GazeMapping);
 UnmapViewOfFile(gazeShared);CloseHandle(gazeMapping);
 std::cout<<"Real OpenXR interceptor: projection publication, independent freshness, failure and recovery passed\n";
}
