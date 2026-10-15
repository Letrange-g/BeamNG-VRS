#include "gpu_pass_profile.hpp"
#include <cassert>
#include <iostream>
#include <string>
using namespace beamng_profile;
namespace {
template<class T>T h(uintptr_t n){return reinterpret_cast<T>(n);}
struct Queries {std::array<uint64_t,256> time{};bool pending=false,available=true;};
std::map<VkQueryPool,Queries> queries;
std::vector<std::string> lines;
unsigned creates=0,reads=0,destroys=0,stamps=0;
uintptr_t next=100;uint64_t tick=100;bool unavailable=false;
void logger(const char* s){lines.emplace_back(s);}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkQueryPoolCreateInfo* ci,const VkAllocationCallbacks*,VkQueryPool* out){
 assert(ci->queryType==VK_QUERY_TYPE_TIMESTAMP && ci->queryCount==256);
 *out=h<VkQueryPool>(next++);queries[*out]={};++creates;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkQueryPool p,const VkAllocationCallbacks*){assert(!queries.at(p).pending);queries.erase(p);++destroys;}
VKAPI_ATTR void VKAPI_CALL reset(VkCommandBuffer,VkQueryPool p,uint32_t first,uint32_t n){assert(!queries.at(p).pending && first==0 && n==256);}
VKAPI_ATTR void VKAPI_CALL stamp(VkCommandBuffer,VkPipelineStageFlagBits stage,VkQueryPool p,uint32_t index){
 assert(stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);queries.at(p).time[index]=tick;tick+=1000000;++stamps;
}
VKAPI_ATTR VkResult VKAPI_CALL results(VkDevice,VkQueryPool p,uint32_t first,uint32_t count,size_t bytes,void* data,VkDeviceSize stride,VkQueryResultFlags flags){
 assert(!queries.at(p).pending && first==0 && bytes==count*16 && stride==16);
 assert(flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));++reads;
 auto* out=static_cast<std::array<uint64_t,2>*>(data);
 for(uint32_t i=0;i<count;++i)out[i]={queries.at(p).time[i],unavailable?0u:1u};
 return unavailable?VK_NOT_READY:VK_SUCCESS;
}
bool contains(const std::string& text){return std::any_of(lines.begin(),lines.end(),[&](const auto& s){return s.find(text)!=std::string::npos;});}
}
int main(){
 double ms=0;assert(elapsed(250,10,8,1000,ms) && std::abs(ms-0.016)<1e-12);
 assert(!elapsed(0,1,0,1,ms) && !elapsed(0,1,65,1,ms) && !elapsed(0,1,64,0,ms));
 assert(elapsed(~uint64_t(0)-4,5,64,1,ms) && std::abs(ms-0.000010)<1e-12);
 const auto device=h<VkDevice>(1);const auto cmd=h<VkCommandBuffer>(2);const auto pool=h<VkCommandPool>(3);const auto pass=h<VkRenderPass>(4);
 Api api{create,destroy,results,reset,stamp};
 VkQueueFamilyProperties family{};family.timestampValidBits=64;family.queueFlags=VK_QUEUE_GRAPHICS_BIT;
 VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; // Reusable, but single submit in this recording.
 VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R16G16B16A16_SFLOAT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
 VkSubpassDescription sub{};sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
 VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;
 VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.renderArea.extent={100,80};
 auto init=[&](Profiler& p){p.device(device,api,1,{family});p.pool(device,pool,0,0);p.allocate(device,pool,cmd,true);p.pass(device,pass,rp);};
 auto graphics=[&](Profiler& p,bool shadow){p.legacy(cmd,&begin,0,0,false,shadow,true);p.bind(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(5),true);p.draw(cmd);p.end(cmd);};
 {
  Profiler p(logger);init(p);p.begin(cmd,&cb,true);graphics(p,false);
  assert(creates==1 && stamps==2 && reads==0 && destroys==0);
  p.submitted(cmd);for(auto& [id,q]:queries)q.pending=true;
  assert(reads==0); // Submission never reads or waits for GPU results.
  for(auto& [id,q]:queries)q.pending=false;
  p.reset(cmd);assert(reads==1 && destroys==1);p.destroyDevice(device);
  assert(contains("kind=graphics") && contains("avgMs=1.0000") && contains("vrsDrawCalls=0"));
 }
 lines.clear();{
  Profiler p(logger);init(p);p.begin(cmd,&cb,true);graphics(p,true);p.submitted(cmd);p.reset(cmd);
  p.begin(cmd,&cb,true);p.bind(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,h<VkPipeline>(6),false);p.compute(cmd,true);p.endCompute(cmd);p.submitted(cmd);p.resetPool(device,pool,true);p.destroyDevice(device);
  assert(contains("vrsDrawCalls=1") && contains("kind=compute"));
 }
 {
  Profiler p;init(p);unsigned before=reads;
  p.begin(cmd,&cb,true);graphics(p,true);p.reset(cmd);assert(reads==before); // Never submitted.
  p.begin(cmd,&cb,true);graphics(p,true);p.submitted(cmd);p.submitted(cmd);p.reset(cmd);assert(reads==before); // Replay excluded.
  p.begin(cmd,&cb,true);graphics(p,true);p.submitted(cmd);unavailable=true;p.reset(cmd);unavailable=false;assert(reads==before+1);
  before=creates;cb.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;p.begin(cmd,&cb,true);graphics(p,true);assert(creates==before);cb.flags=0;
  p.allocate(device,pool,cmd,false);p.begin(cmd,&cb,true);graphics(p,true);assert(creates==before);
  p.allocate(device,pool,cmd,true);p.begin(cmd,&cb,true);p.dynamic(cmd);graphics(p,true);assert(creates==before);
  p.pool(device,pool,0,VK_COMMAND_POOL_CREATE_PROTECTED_BIT);p.begin(cmd,&cb,true);graphics(p,true);assert(creates==before);
  p.destroyDevice(device);
 }
 {
  Profiler p;init(p);const auto before=creates;
  for(unsigned n=0;n<17;++n){auto c=h<VkCommandBuffer>(200+n);p.allocate(device,pool,c,true);p.begin(c,&cb,true);p.compute(c,true);p.endCompute(c);}
  assert(creates==before+16);p.destroyDevice(device); // Live-pool cap; unsubmitted pools can be destroyed without reads.
 }
 {
  Profiler p;init(p);p.begin(cmd,&cb,true);const auto before=stamps;
  for(unsigned n=0;n<200;++n){p.compute(cmd,true);p.endCompute(cmd);}
  assert(stamps==before+256);p.submitted(cmd);p.reset(cmd);p.destroyDevice(device);
 }
 {
  Profiler p;init(p);auto other=h<VkDevice>(99);auto otherCmd=h<VkCommandBuffer>(98);
  p.device(other,api,1,{family});p.pool(other,pool,0,0);p.allocate(other,pool,otherCmd,true);
  p.resetPool(device,pool,true);p.begin(otherCmd,&cb,true);const auto before=creates;
  p.compute(otherCmd,true);p.endCompute(otherCmd);assert(creates==before+1);
  p.destroyDevice(other);p.destroyDevice(device); // Same pool handle on two devices is independent.
 }
 assert(queries.empty() && creates==destroys);
 std::cout<<"GPU profiler timestamps, nonblocking readback, replay exclusion, coverage and lifetime passed\n";
}
