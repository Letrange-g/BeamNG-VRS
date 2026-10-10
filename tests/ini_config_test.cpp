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
 assert(defaults.at("BEAMNG_VRS_SHAPE") == "ellipse");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_TOP") == "0.30");
 assert(defaults.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.30");
 std::istringstream omitted("[Debug]\nDrawOutline=false\n");
 const auto fallback = parse(omitted);
 assert(fallback.at("BEAMNG_VRS_SHAPE") == "ellipse");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_TOP") == "0.30");
 assert(fallback.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.30");
 std::istringstream text("\xEF\xBB\xBF; comment\r\n [DeBuG]\r\nDrawOutline = TRUE\r\n[Scissor]\r\nEnabled = false\r\nTop=0.1\r\nBottom=0.3\r\n[Foveation]\r\nShape=SQUARE\r\n");
 const auto values = parse(text);
 assert(values.at("BEAMNG_VRS_DEBUG_OUTLINE") == "1");
 assert(values.at("BEAMNG_VRS_SCISSOR_ENABLE") == "0");
 assert(values.at("BEAMNG_VRS_SCISSOR_TOP") == "0.1");
 assert(values.at("BEAMNG_VRS_SCISSOR_BOTTOM") == "0.3");
 assert(values.at("BEAMNG_VRS_SHAPE") == "square");
 assert(values.at("BEAMNG_VRS_GAZE_GRACE_MS") == "30");
 for(const char* bad : {"[Foveation]\nTypo=1", "[other]\nEnabled=1", "[Debug]\nDrawOutline=1\ndrawoutline=0",
     "[Foveation]\nSize1x1=nan", "[Foveation]\nSize1x1=1.2", "[Foveation]\nSize1x1=-1",
     "[Foveation]\nSize1x1=0.7\nSize2x2=0.5", "[Foveation]\nSize1x1=0,25",
     "[Scissor]\nTop=0.5", "[Scissor]\nBottom=-0.1", "[Scissor]\nEnabled=yes",
     "[Tracking]\nGracePeriodMs=1001", "[Tracking]\nGazeRefreshMs=1",
     "[Tracking]\nGazeRefreshMs=8.5", "[Tracking]\nGazeRefreshMs=8.0",
     "[Foveation]\nShape=triangle", "[Debug]\nDrawOutline=1 ; inline comment", "[Debug", "DrawOutline=1", "# empty\n"}) {
  bool rejected = false;
  try { std::istringstream input(bad); (void)parse(input); } catch(const std::runtime_error&) { rejected = true; }
  assert(rejected);
 }
 std::cout << "INI defaults, feature gates, scissor edges and malformed settings passed\n";
}
