#include "ini_config.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

int main() {
 using namespace beamng_config;
 std::ifstream shipped("packaging/BeamNG-VRS.ini", std::ios::binary);
 assert(shipped);
 const auto defaults = parse(shipped);
 assert(defaults.at("BEAMNG_VRS_ENABLE_FEATURES") == "1");
 assert(defaults.at("BEAMNG_VRS_M89_STATIC_TEST") == "1");
 assert(defaults.at("BEAMNG_VRS_M812_LIVE_GAZE") == "1");
 assert(defaults.at("BEAMNG_VRS_M84_CPU_GAZE") == "1");
 assert(defaults.at("BEAMNG_VRS_M815_ASYNC") == "1");
 assert(defaults.at("BEAMNG_VRS_CENTER_RADIUS") == "0.25");
 assert(defaults.at("BEAMNG_EYE_FORCE_EXTENSION") == "1");
 assert(defaults.at("BEAMNG_VRS_GAZE_GRACE_MS") == "30");
 assert(defaults.at("BEAMNG_VRS_GAZE_UPDATE_MS") == "8");
 assert(defaults.at("BEAMNG_VRS_SHAPE") == "circle");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_TOP") == "0.30");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.30");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_LEFT") == "0");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_RIGHT") == "0");
 assert(defaults.at("BEAMNG_VRS_TURBO_MODE")=="0");
 assert(defaults.at("BEAMNG_VRS_MID_RADIUS")=="0.55" && defaults.at("BEAMNG_VRS_SCISSOR_ENABLE")=="1");
 assert(defaults.at("BEAMNG_VRS_ALL_PASSES")=="1");
 assert(defaults.at("BEAMNG_VRS_LENS_MASK")=="1" && defaults.at("BEAMNG_VRS_LENS_MASK_SIZE")=="1.15");
 std::istringstream mask("[LensMask]\nEnabled=true\nSize=1.05\n");
 const auto masked=parse(mask);
 assert(masked.at("BEAMNG_VRS_LENS_MASK")=="1" && masked.at("BEAMNG_VRS_LENS_MASK_SIZE")=="1.05");
 for(const char* layers : {"[OpenXR]\nDisableLayers=\n", "[OpenXR]\nDisableLayers=*\n",
     "[openxr]\ndisablelayers = XR_APILAYER_A_overlay , XR_APILAYER_b.2-x\n"}) {
  std::istringstream input(layers);
  assert(parse(input).at("BEAMNG_VRS_LENS_MASK")=="1");
 }
 assert(defaults.at("BEAMNG_VRS_RENDERER")=="d3d12");
 std::istringstream renderer("[renderer]\napi = Vulkan\n");
 assert(parse(renderer).at("BEAMNG_VRS_RENDERER")=="vulkan");
 std::istringstream scale("[Renderer]\nRenderScale=0.75\n");
 assert(parse(scale).at("BEAMNG_VRS_RENDER_SCALE")=="0.75");
 assert(!defaults.count("BEAMNG_VRS_RENDER_SCALE"));
 std::istringstream oldFarClip("[Renderer]\nFarClip=2000\nRenderScale=1\n");
 assert(parse(oldFarClip).at("BEAMNG_VRS_RENDER_SCALE")=="1");
 std::istringstream drawDistance("[Renderer]\nVRDrawDistance=800\n");
 assert(parse(drawDistance).at("BEAMNG_VRS_VR_DRAW_DISTANCE")=="800");
 assert(!defaults.count("BEAMNG_VRS_VR_DRAW_DISTANCE"));
 std::istringstream finalOnly("[Foveation]\nAllPasses=false\n");
 assert(parse(finalOnly).at("BEAMNG_VRS_ALL_PASSES")=="0");
 std::istringstream independent("[Foveation]\nEnabled=false\n[Scissor]\nEnabled=true\n[Performance]\nTurboMode=true\n");
 const auto options=parse(independent);
 for(const auto* gate:{"BEAMNG_VRS_ENABLE_FEATURES","BEAMNG_VRS_ATTACHMENT_PIPELINES","BEAMNG_VRS_M84_CPU_GAZE","BEAMNG_VRS_M815_ASYNC","BEAMNG_EYE_SAMPLE","BEAMNG_EYE_FORCE_EXTENSION"})
  assert(options.at(gate)=="0");
 assert(options.at("BEAMNG_VRS_SCISSOR_ENABLE")=="1" && options.at("BEAMNG_VRS_TURBO_MODE")=="1");
 std::istringstream enabledAgain("[Foveation]\nEnabled=TRUE\n[Performance]\nTurboMode=0\n");
 const auto on=parse(enabledAgain);assert(on.at("BEAMNG_VRS_ENABLE_FEATURES")=="1" && on.at("BEAMNG_VRS_TURBO_MODE")=="0");
 for(const auto& pair:{std::pair{"off","-1"},std::pair{"errors","0"},std::pair{"info","1"},std::pair{"debug","2"}}){
  std::istringstream input(std::string("[Logging]\nLevel=")+pair.first+"\n");
  assert(parse(input).at("BEAMNG_VRS_LOG_LEVEL")==pair.second);
 }
 std::istringstream omitted("[Debug]\nDrawOutline=false\n");
 const auto fallback = parse(omitted);
 assert(fallback.at("BEAMNG_VRS_TURBO_MODE")=="0");
 assert(fallback.at("BEAMNG_VRS_MID_RADIUS")=="0.55" && fallback.at("BEAMNG_VRS_SCISSOR_ENABLE")=="1" && fallback.at("BEAMNG_VRS_LENS_MASK")=="1");
 assert(fallback.at("BEAMNG_VRS_SHAPE") == "circle");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_TOP") == "0.30");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.30");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_LEFT") == "0");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_RIGHT") == "0");
 std::istringstream sides("[Scissor]\nLeft=0.05\nRight=0.49\n");
 const auto horizontal=parse(sides);
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_LEFT") == "0.05");
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_RIGHT") == "0.49");
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT") == "0.05");
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT") == "0.05");
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_RIGHT") == "0.49");
 assert(horizontal.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT") == "0.49");
 for(const auto& input : {
     "[Scissor]\nLeft=0.1\nRight=0.2\nLeftEyeLeft=0.05\nLeftEyeRight=0\nRightEyeLeft=0\nRightEyeRight=0.49\n",
     "[Scissor]\nLeftEyeLeft=0.05\nLeftEyeRight=0\nRightEyeLeft=0\nRightEyeRight=0.49\nLeft=0.1\nRight=0.2\n"}) {
  std::istringstream eyes(input);const auto perEye=parse(eyes);
  assert(perEye.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT")=="0.05");
  assert(perEye.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_RIGHT")=="0");
  assert(perEye.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT")=="0");
  assert(perEye.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT")=="0.49");
 }
 std::istringstream partial("[Scissor]\nLeft=0.1\nRight=0.2\nRightEyeLeft=0\n");
 const auto partialEye=parse(partial);
 assert(partialEye.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT")=="0.1");
 assert(partialEye.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT")=="0");
 assert(partialEye.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT")=="0.2");
 for(const auto* name:{"LEFT_EYE_LEFT","RIGHT_EYE_RIGHT"}) {
  const auto key=std::string("BEAMNG_VRS_SCISSOR_")+name;
  assert(defaults.at(key)=="0.10" && fallback.at(key)=="0.10");
 }
 for(const auto* name:{"LEFT_EYE_RIGHT","RIGHT_EYE_LEFT"}) {
  const auto key=std::string("BEAMNG_VRS_SCISSOR_")+name;
  assert(defaults.at(key)=="0" && fallback.at(key)=="0");
 }
 std::istringstream oldZero("[Scissor]\nLeft=0\nRight=0\n");
 const auto legacyZero=parse(oldZero);
 for(const auto* name:{"LEFT_EYE_LEFT","LEFT_EYE_RIGHT","RIGHT_EYE_LEFT","RIGHT_EYE_RIGHT"})
  assert(legacyZero.at(std::string("BEAMNG_VRS_SCISSOR_")+name)=="0");
 std::istringstream oneLegacy("[Scissor]\nLeft=0\n");
 const auto legacyLeft=parse(oneLegacy);
 assert(legacyLeft.at("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT")=="0");
 assert(legacyLeft.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT")=="0");
 assert(legacyLeft.at("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT")=="0.10");
 for(const char* size : {"0", "0.0", "0.1"}) {
  std::istringstream equalSizes(std::string("[Foveation]\nSize1x1=") + size + "\nSize2x2=" + size + "\nShape=ellipse\n");
  const auto accepted = parse(equalSizes);
  assert(accepted.at("BEAMNG_VRS_CENTER_RADIUS") == size);
  assert(accepted.at("BEAMNG_VRS_MID_RADIUS") == size);
  assert(accepted.at("BEAMNG_VRS_ENABLE_FEATURES") == "1");
 }
 std::istringstream text("\xEF\xBB\xBF; comment\r\n [DeBuG]\r\nDrawOutline = TRUE\r\n[Scissor]\r\nEnabled = false\r\nTop=0.1\r\nBottom=0.3\r\n[Foveation]\r\nShape=SQUARE\r\n");
 const auto values = parse(text);
 assert(values.at("BEAMNG_VRS_DEBUG_OUTLINE") == "1");
 assert(values.at("BEAMNG_VRS_SCISSOR_ENABLE") == "0");
 assert(values.at("BEAMNG_VRS_SCISSOR_TOP") == "0.1");
 assert(values.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.3");
 assert(values.at("BEAMNG_VRS_SHAPE") == "square");
 assert(values.at("BEAMNG_VRS_GAZE_GRACE_MS") == "30");
 for(const char* bad : {"[Logging]\nLevel=verbose", "[Logging]\nLevel=off\nlevel=info", "[Performance]\nTurboMode=yes", "[Performance]\nTurboMode=true\nturbomode=false",
     "[Foveation]\nAllPasses=yes", "[LensMask]\nSize=1.6", "[LensMask]\nSize=0.4", "[LensMask]\nEnabled=on", "[Foveation]\nEnabled=2", "[Foveation]\nEnabled=true\nenabled=false", "[Foveation]\nTypo=1", "[other]\nEnabled=1", "[Debug]\nDrawOutline=1\ndrawoutline=0",
     "[Foveation]\nSize1x1=nan", "[Foveation]\nSize1x1=1.2", "[Foveation]\nSize1x1=-1",
     "[Foveation]\nSize1x1=0.7\nSize2x2=0.5", "[Foveation]\nSize1x1=0,25",
     "[Scissor]\nTop=0.5", "[Scissor]\nBottom=-0.1", "[Scissor]\nEnabled=yes",
     "[Scissor]\nLeft=-0.1", "[Scissor]\nLeft=0.5", "[Scissor]\nRight=0.5",
     "[Scissor]\nLeftEyeLeft=-0.1", "[Scissor]\nLeftEyeRight=0.5",
     "[Scissor]\nRightEyeLeft=NaN", "[Scissor]\nRightEyeRight=0.50",
     "[Scissor]\nLeftEyeLeft=0\nlefteyeleft=0.1", "[Scissor]\nRight=NaN", "[Scissor]\nLeft=0\nleft=0.1",
     "[Tracking]\nGracePeriodMs=1001", "[Tracking]\nGazeRefreshMs=1",
     "[Tracking]\nGazeRefreshMs=8.5", "[Tracking]\nGazeRefreshMs=8.0",
     "[Foveation]\nShape=triangle", "[Debug]\nDrawOutline=1 ; inline comment",
     "[OpenXR]\nDisableLayers=XR layer", "[OpenXR]\nDisableLayers=A,,B", "[OpenXR]\nDisableLayers=A,", "[OpenXR]\nDisableLayers=A,*",
     "[OpenXR]\nDisableLayers=A\ndisablelayers=B", "[OpenXR]\nTypo=1", "[Renderer]\nAPI=dx12", "[Renderer]\nRenderScale=1.5", "[Renderer]\nRenderScale=0.2", "[Renderer]\nRenderScale=auto", "[Renderer]\nVRDrawDistance=50", "[Renderer]\nVRDrawDistance=20000", "[Renderer]\nVRDrawDistance=far","[Renderer]\nAPI=vulkan\napi=d3d12", "[Debug", "DrawOutline=1", "# empty\n"}) {
  bool rejected = false;
  try { std::istringstream input(bad); (void)parse(input); } catch(const std::runtime_error&) { rejected = true; }
  assert(rejected);
 }
 std::cout << "INI defaults, feature gates, scissor edges and malformed settings passed\n";
}
