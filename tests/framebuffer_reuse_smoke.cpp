#include "../src/layer.cpp"
#include <cassert>
#include <iostream>
namespace {
uintptr_t nextFb=1000;
unsigned created=0,destroyed=0;
std::unordered_set<VkFramebuffer> alive;
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer* out){
 *out=reinterpret_cast<VkFramebuffer>(nextFb++);alive.insert(*out);++created;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkFramebuffer fb,const VkAllocationCallbacks*){assert(alive.erase(fb)==1);++destroyed;}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice,VkImageView,const VkAllocationCallbacks*){}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice,VkRenderPass,const VkAllocationCallbacks*){}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice,const VkAllocationCallbacks*){assert(alive.empty());}
template<class T>T handle(uintptr_t v){return reinterpret_cast<T>(v);}
}
int main(){
 SetEnvironmentVariableA("BEAMNG_VRS_CONFIG","Z:\\nonexistent-fb-reuse.ini");
 SetEnvironmentVariableA("BEAMNG_VRS_M8_EXPERIMENTAL","1");
 SetEnvironmentVariableA("BEAMNG_VRS_M823_LAZY_FB","1");
 SetEnvironmentVariableA("BEAMNG_VRS_EARLY_PASSES","off");
 SetEnvironmentVariableA("BEAMNG_VRS_LOG_LEVEL","-1");
 m814ActiveWidth=100;m814ActiveHeight=80;
 const auto device=handle<VkDevice>(1),other=handle<VkDevice>(2);
 const auto pass=handle<VkRenderPass>(3),shadow=handle<VkRenderPass>(4);
 const auto view=handle<VkImageView>(5),rate=handle<VkImageView>(6);
 DeviceDispatch d{};d.createFramebuffer=create;d.destroyFramebuffer=destroy;
 d.destroyImageView=destroyView;d.destroyRenderPass=destroyPass;d.destroyDevice=destroyDevice;
 devices[device]=d;devices[other]=d;
 shadowPasses[pass]={device,shadow};uploadedPatterns[device].view=rate;
 m83EyePatterns[device][0].view=rate;m83EyePatterns[device][1].view=handle<VkImageView>(7);
 m74gViews[view]={device,handle<VkImage>(8)};
 VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
 ci.renderPass=pass;ci.width=100;ci.height=80;ci.layers=1;ci.attachmentCount=1;ci.pAttachments=&view;
 auto original=[&](const VkAllocationCallbacks* alloc=nullptr){VkFramebuffer fb{};
  assert(vkCreateFramebuffer(device,&ci,alloc,&fb)==VK_SUCCESS);assert(m823EnsureEyeFramebuffer(device,fb,0));return fb;};
 auto a=original();const auto variant=m83EyeFramebuffers.at(a).eyes[0];
 auto b=original();assert(m83EyeFramebuffers.at(b).eyes[0]!=variant); // Active is never shared.
 vkDestroyFramebuffer(device,a,nullptr);assert(alive.count(variant));
 const auto before=created;auto c=original();assert(created==before+1); // Only the original is created.
 assert(m83EyeFramebuffers.at(c).eyes[0]==variant && m834FramebufferCache.hits==1);
 vkDestroyFramebuffer(device,b,nullptr);vkDestroyFramebuffer(device,c,nullptr);
 assert(m834FramebufferCache.idle()==2);
 vkDestroyImageView(device,view,nullptr);assert(m834FramebufferCache.idle()==0 && alive.empty());
 // Same numeric view after destruction must allocate a fresh variant.
 m74gViews[view]={device,handle<VkImage>(9)};a=original();
 assert(m83EyeFramebuffers.at(a).eyes[0]!=variant);
 // Invalidate while active, then destroy original: must not enter idle cache.
 vkDestroyImageView(device,view,nullptr);vkDestroyFramebuffer(device,a,nullptr);assert(alive.empty());
 m74gViews[view]={device,handle<VkImage>(10)};
 VkAllocationCallbacks custom{};a=original(&custom);vkDestroyFramebuffer(device,a,&custom);
 assert(m834FramebufferCache.idle()==0 && alive.empty());
 a=original();vkDestroyFramebuffer(device,a,nullptr);assert(m834FramebufferCache.idle()==1);
 vkDestroyRenderPass(device,pass,nullptr);assert(m834FramebufferCache.idle()==0 && alive.empty());
 shadowPasses[pass]={device,shadow};a=original();vkDestroyFramebuffer(device,a,nullptr);
 // Device cleanup drains idle entries before destroying layer image resources.
 vkDestroyDevice(device,nullptr);assert(m834FramebufferCache.idle()==0 && alive.empty());
 assert(devices.count(other));
 std::cout<<"Framebuffer real-hook reuse, ownership and destruction passed\n";
}
