// Exercise the real Vulkan hooks and staging code without a GPU/runtime.
#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
#include <new>

namespace {std::atomic<uint64_t> allocations{0};}
void* operator new(size_t n){allocations.fetch_add(1);if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);}
void operator delete[](void* p,size_t)noexcept{std::free(p);}
namespace {
VkPipeline bound=VK_NULL_HANDLE;
std::array<VkRect2D,16> scissors{};
std::array<VkRect2D,4> cleared{};
VkRect2D pipelineScissor{};
uint32_t scissorCount=0,clearCalls=0,clearRects=0;
uint32_t pipelineCreates=0,failCreateAt=0,beginCalls=0;
std::unordered_map<VkPipeline,VkRect2D> createdScissors;
std::vector<VkPipeline> destroyedPipelines;
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){++beginCalls;}
VKAPI_ATTR void VKAPI_CALL beginPass2(VkCommandBuffer,const VkRenderPassBeginInfo*,const VkSubpassBeginInfo*){++beginCalls;}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer,VkPipelineBindPoint,VkPipeline p){bound=p;}
VKAPI_ATTR void VKAPI_CALL setScissor(VkCommandBuffer,uint32_t,uint32_t count,const VkRect2D* rects){
 scissorCount=count;assert(count<=scissors.size());for(uint32_t i=0;i<count;++i)scissors[i]=rects[i];
}
VKAPI_ATTR void VKAPI_CALL clear(VkCommandBuffer,uint32_t attachmentCount,const VkClearAttachment*,uint32_t count,const VkClearRect* rects){
 assert(attachmentCount==1 && count<=cleared.size());++clearCalls;clearRects=count;
 for(uint32_t i=0;i<count;++i){assert(rects[i].baseArrayLayer==0 && rects[i].layerCount==1);cleared[i]=rects[i].rect;}
}
VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice,VkPipelineCache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks*,VkPipeline* out){
 assert(count==1 && infos[0].pViewportState && infos[0].pViewportState->pScissors);
 pipelineScissor=infos[0].pViewportState->pScissors[0];
 out[0]=reinterpret_cast<VkPipeline>(uintptr_t(10+(++pipelineCreates)));
 createdScissors[out[0]]=pipelineScissor;
 return failCreateAt==pipelineCreates?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice,VkPipeline pipeline,const VkAllocationCallbacks*){destroyedPipelines.push_back(pipeline);}
VKAPI_ATTR VkResult VKAPI_CALL createFramebuffer(VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer* out){
 *out=reinterpret_cast<VkFramebuffer>(uintptr_t(301));return VK_SUCCESS;
}
bool contains(VkRect2D r,int32_t x,int32_t y){
 return x>=r.offset.x && y>=r.offset.y && uint32_t(x-r.offset.x)<r.extent.width && uint32_t(y-r.offset.y)<r.extent.height;
}
void checkClears(VkRect2D area,VkRect2D keep){
 for(int32_t y=0;y<100;++y)for(int32_t x=0;x<100;++x){
  unsigned writes=0;for(uint32_t i=0;i<clearRects;++i)writes+=contains(cleared[i],x,y);
  assert(writes==unsigned(contains(area,x,y) && !contains(keep,x,y)));
 }
}
}
int main(int argc,char** argv){
 assert(argc==2);
 const std::string mode=argv[1];
 const bool crop=mode!="quiet";
 const bool cropOnly=mode=="scissor_only";
 const bool perEye=mode=="scissor_per_eye" || mode=="scissor_per_eye_nose";
 const bool nose=mode=="scissor_per_eye_nose";
 const bool sides=mode=="scissor_sides" || mode=="scissor_sides_only";
 const bool sidesOnly=mode=="scissor_sides_only", edges=mode=="scissor_edges";
 const VkRect2D expectedCrop=edges?VkRect2D{{49,49},{2,2}}:
  sides?VkRect2D{{10,sidesOnly?0:30},{70,sidesOnly?100u:40u}}:VkRect2D{{0,30},{100,40}};
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL",cropOnly?"-1":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_ENABLE",crop?"1":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_TOP",edges?"0.49":sidesOnly?"0":"0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_BOTTOM",edges?"0.49":sidesOnly?"0":"0.30");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_LEFT",edges?"0.49":sides?"0.10":"0");
 SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_RIGHT",edges?"0.49":sides?"0.20":"0");
 if(perEye){
  SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_LEFT_EYE_LEFT",nose?"0":"0.10");
  SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_LEFT_EYE_RIGHT",nose?"0.10":"0");
  SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_RIGHT_EYE_LEFT",nose?"0.20":"0");
  SetEnvironmentVariableA("BEAMNG_VRS_SCISSOR_RIGHT_EYE_RIGHT",nose?"0":"0.20");
 }
 SetEnvironmentVariableA("BEAMNG_VRS_DEBUG_OUTLINE","0");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL",cropOnly?"0":"1");
 SetEnvironmentVariableA("BEAMNG_VRS_M89_STATIC_TEST",cropOnly?"0":"1");
 SetEnvironmentVariableA("BEAMNG_VRS_M84_CPU_GAZE",cropOnly?"0":"1");
 SetEnvironmentVariableA("BEAMNG_VRS_GAZE_GRACE_MS","0");
 SetEnvironmentVariableA("BEAMNG_VRS_CENTER_RADIUS","0.25");
 SetEnvironmentVariableA("BEAMNG_VRS_MID_RADIUS","0.60");
 SetEnvironmentVariableA("BEAMNG_VRS_SHAPE","ellipse");
 wchar_t temporary[32768]{};assert(GetTempPathW(32768,temporary));
 const auto missing=std::filesystem::path(temporary)/(L"BeamNG-VRS-no-config-"+std::to_wstring(GetCurrentProcessId())+L".ini");
 assert(SetEnvironmentVariableW(L"BEAMNG_VRS_CONFIG",missing.c_str()));
 const auto device=reinterpret_cast<VkDevice>(uintptr_t(1));
 const auto cmd=reinterpret_cast<VkCommandBuffer>(uintptr_t(2));
 const auto original=reinterpret_cast<VkPipeline>(uintptr_t(3));
 const auto replacement=reinterpret_cast<VkPipeline>(uintptr_t(4));
 const auto pass=reinterpret_cast<VkRenderPass>(uintptr_t(5));
 DeviceDispatch d{};d.cmdBindPipeline=bind;d.cmdSetScissor=setScissor;d.cmdClearAttachments=clear;d.createPipelines=createPipelines;d.destroyPipeline=destroyPipeline;d.cmdBeginRenderPass=beginPass;d.cmdBeginRenderPass2=beginPass2;
 devices[device]=d;commandDevices[cmd]=device;
 // Synthetic compatibility keys for the mock render passes below. Real pass
 // creation/translation and mismatches are covered by vrs_compatibility_smoke.
 if(!cropOnly){m89ActiveShadows[cmd]=pass;m89Variants[original]={device,replacement,pass,VK_NULL_HANDLE,{{1}}};m832ShadowSignatures[pass]={{1}};}
 const auto expectedBound=cropOnly?original:replacement;
 m87ActivePasses[cmd]=pass;
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==expectedBound);
 VkRect2D rect{{0,0},{100,100}};
 cropStates[cmd]={true,100,100,rect,0};
 const auto expected=crop?expectedCrop:rect;
 auto same=[](VkRect2D a,VkRect2D b){return a.offset.x==b.offset.x && a.offset.y==b.offset.y && a.extent.width==b.extent.width && a.extent.height==b.extent.height;};
 vkCmdSetScissor(cmd,0,1,&rect);assert(same(scissors[0],expected));
 const uint64_t before=allocations.load();
 for(unsigned i=0;i<1000;++i){
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);
  vkCmdSetScissor(cmd,0,1,&rect);
 }
 assert(allocations.load()==before);assert(m87PassBindCounts.empty());assert(bound==expectedBound);
 VkRect2D stereo[2]{rect,rect};
 const uint64_t multiBefore=allocations.load();vkCmdSetScissor(cmd,0,2,stereo);
 assert(allocations.load()==multiBefore && scissorCount==2 && same(scissors[0],expected) && same(scissors[1],expected));
 cropBlackBars(cmd,d);assert(clearCalls==(crop?1u:0u));
 if(crop){
  assert(clearRects==((sides && !sidesOnly) || edges?4u:2u));checkClears(rect,expectedCrop);
  // Restrict each clear to the original render area, including corners.
  VkRect2D partial{{0,20},{40,50}};cropStates[cmd]={true,100,100,partial,0};clearCalls=clearRects=0;
  cropBlackBars(cmd,d);assert(clearCalls<=1);checkClears(partial,expectedCrop);
  cropStates[cmd]={true,100,100,expectedCrop,0};clearCalls=clearRects=0;
  cropBlackBars(cmd,d);assert(clearCalls==0 && clearRects==0);
 }
 if(!perEye){
 // Matching static scissors use the same crop, while dynamic and other-size
 // pipeline scissors remain application-owned until the eye command hook.
 m814ActiveWidth=100;m814ActiveHeight=100;
 VkViewport viewport{0,0,100,100,0,1};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
 vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;
 VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pViewportState=&vp;
 VkPipeline made=VK_NULL_HANDLE;
 assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
 assert(same(pipelineScissor,expected));
 const VkDynamicState dynamic=VK_DYNAMIC_STATE_SCISSOR;
 VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamicInfo.dynamicStateCount=1;dynamicInfo.pDynamicStates=&dynamic;
 info.pDynamicState=&dynamicInfo;
 assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS && same(pipelineScissor,rect));
 info.pDynamicState=nullptr;VkRect2D other{{0,0},{80,100}};vp.pScissors=&other;
 assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS && same(pipelineScissor,other));
 if(!cropOnly){
 // The replacement bound during a shadow pass must inherit the static crop.
 devices[device].vrsEnabled=true;devices[device].destroyPipeline=destroyPipeline;
 shadowPasses[pass]={device,reinterpret_cast<VkRenderPass>(uintptr_t(7))};
 m832ShadowSignatures[shadowPasses[pass].shadow]={{1}};
 info.renderPass=pass;vp.pScissors=&rect;
 auto createsBefore=pipelineCreates;
 assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
 assert(pipelineCreates==createsBefore+2 && m89Variants.count(made) && same(pipelineScissor,expected));
 info.pDynamicState=&dynamicInfo;createsBefore=pipelineCreates;
 assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
 assert(pipelineCreates==createsBefore+2 && same(pipelineScissor,rect));
 }
 }
 m89ActiveShadows.erase(cmd);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,original);assert(bound==original);
 if(perEye){
  m814ActiveWidth=m814ActiveHeight=100;
  const VkRect2D leftKeep{{nose?0:10,30},{90,40}},rightKeep{{nose?20:0,30},{80,40}};
  const auto leftImage=reinterpret_cast<VkImage>(uintptr_t(201)),rightImage=reinterpret_cast<VkImage>(uintptr_t(202));
  const auto leftView=reinterpret_cast<VkImageView>(uintptr_t(203)),rightView=reinterpret_cast<VkImageView>(uintptr_t(204));
  const auto framebuffer=reinterpret_cast<VkFramebuffer>(uintptr_t(205));
  m74gViews[leftView]={device,leftImage};m74gViews[rightView]={device,rightImage};
  m75Sources[leftImage]=0;m75Sources[rightImage]=1;
  framebuffers[framebuffer]={100,100,1,1};passInfos[pass]={1,1,1,1,false,true};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea=rect;
  VkSubpassBeginInfo subpass{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};subpass.contents=VK_SUBPASS_CONTENTS_INLINE;
  // Actual begin/end hooks select by attachment image, independent of outlines,
  // gaze and VRS substitution. Both APIs preserve the exact per-eye black bars.
  for(int eye=0;eye<2;++eye){
   m74dAttachments[framebuffer]={{eye?rightView:leftView},pass};
   if(eye)vkCmdBeginRenderPass2(cmd,&begin,&subpass);
   else vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
   assert(cropStates.at(cmd).eye==eye);
   const auto keep=eye?rightKeep:leftKeep;
   vkCmdSetScissor(cmd,0,1,&rect);assert(same(scissors[0],keep));
   clearCalls=clearRects=0;
   if(eye)vkCmdEndRenderPass2(cmd,nullptr);else vkCmdEndRenderPass(cmd);
   assert(clearCalls==1 && clearRects==3 && !cropStates.count(cmd));checkClears(rect,keep);
  }
  assert(beginCalls==2);
  m74dAttachments[framebuffer]={{leftView,rightView},pass};
  m830StartCrop(cmd,&begin,true);assert(cropStates.at(cmd).eye==-1);
  vkCmdSetScissor(cmd,0,1,&rect);assert(same(scissors[0],expectedCrop));
  m74dAttachments.erase(framebuffer);m830StartCrop(cmd,&begin,true);assert(cropStates.at(cmd).eye==-1);
  m830StartCrop(cmd,&begin,false);assert(!cropStates.count(cmd));
  framebuffers[framebuffer].layers=2;m830StartCrop(cmd,&begin,true);assert(!cropStates.count(cmd));
  framebuffers[framebuffer].layers=1;
  VkViewport viewport{0,0,100,100,0,1};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pViewportState=&vp;info.renderPass=pass;
  VkPipeline made=VK_NULL_HANDLE;
  auto createsBefore=pipelineCreates;
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
  assert(pipelineCreates==createsBefore+3 && m830CropVariants.count(made));
  assert(same(createdScissors.at(made),expectedCrop));
  auto checkStatic=[&](VkPipeline originalBase,VkPipeline actualBase){
   assert(same(createdScissors.at(m830CropVariants.at(actualBase).eyes[0]),leftKeep));
   assert(same(createdScissors.at(m830CropVariants.at(actualBase).eyes[1]),rightKeep));
   for(int eye=0;eye<2;++eye){
    cropStates[cmd]={true,100,100,rect,0,eye};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,originalBase);
    assert(bound==m830CropVariants.at(actualBase).eyes[eye]);
    const auto count=allocations.load();
    for(unsigned i=0;i<1000;++i)vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,originalBase);
    assert(allocations.load()==count);
   }
   cropStates[cmd]={true,100,100,rect,0,-1};
   vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,originalBase);assert(bound==actualBase);
  };
  checkStatic(made,made);
  vkDestroyPipeline(device,made,nullptr);assert(!m830CropVariants.count(made));
  devices[device].vrsEnabled=true;
  const auto shadow=reinterpret_cast<VkRenderPass>(uintptr_t(207));shadowPasses[pass]={device,shadow};
  m832ShadowSignatures[shadow]={{1}};
  createsBefore=pipelineCreates;
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
  assert(pipelineCreates==createsBefore+6 && m89Variants.count(made));
  const auto compatible=m89Variants.at(made).pipeline;
  m89ActiveShadows[cmd]=shadow;checkStatic(made,compatible);
  const auto destroyedBefore=destroyedPipelines.size();
  vkDestroyPipeline(device,made,nullptr);
  assert(destroyedPipelines.size()==destroyedBefore+6 && !m830CropVariants.count(made) && !m830CropVariants.count(compatible));
  m89ActiveShadows.erase(cmd);shadowPasses.erase(pass);devices[device].vrsEnabled=false;
  // Partial copy creation must be cleaned up without failing the app pipeline.
  createsBefore=pipelineCreates;failCreateAt=createsBefore+3;
  const auto failureDestroyed=destroyedPipelines.size();
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
  assert(!m830CropVariants.count(made) && destroyedPipelines.size()==failureDestroyed+2);
  assert(same(createdScissors.at(made),expectedCrop));failCreateAt=0;
  vkDestroyPipeline(device,made,nullptr);
  const VkDynamicState dynamic=VK_DYNAMIC_STATE_SCISSOR;
  VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamicInfo.dynamicStateCount=1;dynamicInfo.pDynamicStates=&dynamic;
  info.pDynamicState=&dynamicInfo;createsBefore=pipelineCreates;
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
  assert(pipelineCreates==createsBefore+1 && !m830CropVariants.count(made) && same(createdScissors.at(made),rect));
  vkDestroyPipeline(device,made,nullptr);
  cropStates.erase(cmd);
 }
 if(cropOnly){
  // A 75% eye target must work with VRS disabled. Exercise object creation
  // rather than manually seeding the active size or attachment metadata.
  SetEnvironmentVariableA("BEAMNG_VRS_TARGET_WIDTH","3892");
  SetEnvironmentVariableA("BEAMNG_VRS_TARGET_HEIGHT","3126");
  const VkRect2D eyeRect{{0,0},{3892,3126}};
  VkViewport viewport{0,0,3892,3126,0,1};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&eyeRect;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.pViewportState=&vp;
  VkPipeline made=VK_NULL_HANDLE;
  assert(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&made)==VK_SUCCESS);
  assert(m813Width()==3892 && m813Height()==3126 && same(pipelineScissor,cropRect(3892,3126)));
  const auto image=reinterpret_cast<VkImage>(uintptr_t(302));
  const auto view=reinterpret_cast<VkImageView>(uintptr_t(303));
  m74gViews[view]={device,image};m75Sources[image]=1;
  devices[device].createFramebuffer=createFramebuffer;
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass=pass;fb.width=3892;fb.height=3126;fb.layers=1;fb.attachmentCount=1;fb.pAttachments=&view;
  VkFramebuffer framebuffer=VK_NULL_HANDLE;
  assert(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer)==VK_SUCCESS);
  assert(m74dAttachments.at(framebuffer).views[0]==view);
  passInfos[pass]={1,1,1,1,false,true};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass=pass;begin.framebuffer=framebuffer;begin.renderArea=eyeRect;
  vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
  assert(cropStates.at(cmd).eye==1 && cropStates.at(cmd).width==3892);
  vkCmdSetScissor(cmd,0,1,&eyeRect);assert(same(scissors[0],cropRect(3892,3126,1)));
  clearCalls=clearRects=0;vkCmdEndRenderPass(cmd);
  assert(clearCalls==1 && clearRects==2 && !cropStates.count(cmd));
  assert(uploadedPatterns.empty() && m83EyePatterns.empty() && m812Staging.empty() && m814ResourceSets.empty());
  assert(!m8Enabled() && !devices[device].vrsEnabled && m89Variants.empty());
  m84PrepareEyePatterns();assert(m812bPreparedCount[0]==0 && m812bPreparedCount[1]==0);
  assert(m817LogLevel()==-1);log("ERROR logging-off fixture");
  std::cout<<"Scissor-only: static/dynamic crop and black borders without VRS substitution or gaze pattern preparation passed\n";
  return 0;
 }
 // Stage through the real producer. A newer sample with identical payload
 // must not create a new upload, even while the latest source slot is pinned.
 m814ActiveWidth=64;m814ActiveHeight=64;
 std::array<std::array<std::array<uint8_t,64>,3>,2> memory{};
 auto& staging=m812Staging[device];
 for(int eye=0;eye<2;++eye)for(int slot=0;slot<3;++slot){
  staging.slots[eye][slot].mapped=memory[eye][slot].data();staging.slots[eye][slot].capacity=64;
 }
 HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(beamng_projected_gaze::Data),beamng_projected_gaze::mappingName);assert(mapping);
 auto* shared=static_cast<beamng_projected_gaze::Data*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(beamng_projected_gaze::Data)));assert(shared);
 auto sample=[&](float u,float v,uint32_t mask=3){
  InterlockedIncrement(&shared->sequence);MemoryBarrier();shared->magic=beamng_projected_gaze::magicValue;
  shared->processId=GetCurrentProcessId();shared->eyeValidMask=mask;
  shared->poseTickMs=shared->projectionTickMs=GetTickCount64();
  for(int eye=0;eye<2;++eye){shared->eyeU[eye]=u;shared->eyeV[eye]=v;}
  MemoryBarrier();InterlockedIncrement(&shared->sequence);
 };
 auto prepare=[&](){Sleep(12);m84PrepareEyePatterns();};
 sample(.43f,.66f);prepare();assert(m812bPreparedCount[0]==1 && m812bPreparedCount[1]==1);
 auto& state=m815SlotStates[device][m814Key(64,64)];
 for(int eye=0;eye<2;++eye)state.busy[eye][state.latestSlot[eye]]=true;
 sample(.43f,.66f);prepare();assert(m812bPreparedCount[0]==1 && m821GazeStats.identicalPattern==2);
 sample(.9f,.1f);prepare();assert(m812bPreparedCount[0]==2 && m812bPreparedCount[1]==2);
 // Exhausted slots remain immutable and the new sample is retried later.
 for(auto& eye:state.busy)eye.fill(true);
 const auto saved=memory;sample(.1f,.9f);prepare();assert(memory==saved && m812bPreparedCount[0]==2);
 for(auto& eye:state.busy)eye.fill(false);
 prepare();assert(m812bPreparedCount[0]==3 && m812bPreparedCount[1]==3);
 // The same source sequence must be restaged after a resolution switch.
 m814ActiveWidth=80;prepare();assert(m812bPreparedCount[0]==4 && m812bPreparedCount[1]==4);
 sample(.1f,.9f,0);prepare();assert(m812bPreparedCount[0]==5 && m825StagedCenters[0].u==.5f);
 // Returning to a cached resolution during dropout still restores center.
 m814ActiveWidth=64;state.latestSlot={{-1,-1}};prepare();assert(m812bPreparedCount[0]==6 && m812bPreparedCount[1]==6);
 // Zero sizes are a valid diagnostic: every eligible tile becomes 4x4.
 // Exercise the actual layer's numeric reader and fixed/live pattern paths.
 SetEnvironmentVariableA("BEAMNG_VRS_CENTER_RADIUS","0");
 SetEnvironmentVariableA("BEAMNG_VRS_MID_RADIUS","0");
 const auto coarse=m58MakePattern(64,64);
 assert(coarse.texels.size()==16);
 for(const auto value:coarse.texels)assert(value==beamng_ffr::rate(4,4));
 sample(.4f,.7f);prepare();assert(m812bPreparedCount[0]==7 && m812bPreparedCount[1]==7);
 for(int eye=0;eye<2;++eye){
  const auto latest=state.latestSlot[eye];assert(latest>=0);
  for(size_t i=0;i<coarse.texels.size();++i)assert(memory[eye][latest][i]==beamng_ffr::rate(4,4));
 }
 UnmapViewOfFile(shared);CloseHandle(mapping);
 std::cout<<"Real Vulkan hooks: allocation-free dispatch/scissors, pipeline substitution, batched bars, identical-payload skip, pinned staging and resize fallback passed\n";
}
