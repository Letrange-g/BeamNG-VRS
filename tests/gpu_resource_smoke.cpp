#include "gpu_resource_trace.hpp"
#include <cassert>
#include <iostream>
using namespace beamng_trace;
template<class T>T h(uintptr_t n){return reinterpret_cast<T>(n);}
void descriptorChurn(){
 const auto device=h<VkDevice>(1);const auto image=h<VkImage>(2);const auto view=h<VkImageView>(3);
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.extent={100,80,1};ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;
 VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorSetCount=1;ai.descriptorPool=h<VkDescriptorPool>(4);
 VkDescriptorImageInfo ii{};ii.imageView=view;
 VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&ii;
 auto prepare=[&](Resources& r){r.createImage(device,image,ci);r.createView(device,view,vi);};
 auto allocate=[&](Resources& r,VkDescriptorSet s){r.allocateSets(device,ai,&s);w.dstSet=s;w.dstBinding=0;r.update(device,1,&w,0,nullptr);};
 auto observe=[&](Resources& r,VkDescriptorSet s){Uses u;BoundSets b;b.bind(h<VkPipelineLayout>(5),0,1,&s);r.collect(device,b,u);return u;};
 // Reproduce the actual 16K stop-accepting-new-sets defect, not just a tiny cache.
 Resources production;prepare(production);
 for(unsigned n=0;n<20000;++n){auto s=h<VkDescriptorSet>(100+n);allocate(production,s);
  if(n%1000==0 || n==19999){auto u=observe(production,s);assert(u.reads.size()==1 && !u.partial);}
  assert(production.setCount()<=production.setCapacity());
 }
 assert(production.setCount()==16384 && production.counters().setEvictions==20000-16384);
 production.resetPool(device,ai.descriptorPool);assert(production.setCount()==0);
 // Sampling a long-lived set keeps it ahead of unused loading-time sets.
 Resources small(3);prepare(small);auto hot=h<VkDescriptorSet>(10),cold=h<VkDescriptorSet>(11),third=h<VkDescriptorSet>(12),newest=h<VkDescriptorSet>(13);
 allocate(small,hot);allocate(small,cold);allocate(small,third);assert(observe(small,hot).reads.size()==1);
 allocate(small,newest);assert(observe(small,cold).partial && observe(small,hot).reads.size()==1);
 // Recover known new writes to an evicted set. Other bindings and its owning
 // pool are unknown, so the partial flag remains and any pool reset removes it.
 w.dstSet=cold;small.update(device,1,&w,0,nullptr);auto recovered=observe(small,cold);
 assert(recovered.reads.size()==1 && recovered.partial && small.counters().setRecoveries==1);
 small.resetPool(device,h<VkDescriptorPool>(99));assert(observe(small,cold).reads.empty());assert(observe(small,hot).reads.size()==1);
 small.freeSets(device,1,&hot);small.allocateSets(device,ai,&hot);auto reused=observe(small,hot);
 assert(reused.reads.empty() && !reused.partial); // New handle generation never inherits old slots.
 // Copy insertion can evict its source; the copied value must already be owned.
 Resources one(1);prepare(one);allocate(one,hot);VkCopyDescriptorSet cp{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};cp.srcSet=hot;cp.dstSet=cold;cp.descriptorCount=1;
 one.update(device,0,nullptr,1,&cp);auto copied=observe(one,cold);assert(copied.reads.size()==1 && copied.partial && one.setCount()==1);
 // A known slot in a partial source is still useful; preserve it with the flag.
 cp.srcSet=cold;cp.dstSet=third;one.update(device,0,nullptr,1,&cp);assert(observe(one,third).reads.size()==1 && observe(one,third).partial);
 one.destroyDevice(device);assert(one.setCount()==0);prepare(one);allocate(one,hot);assert(observe(one,hot).reads.size()==1);
 // Hundreds of material texture descriptors cannot starve one final-eye input.
 Resources materials;prepare(materials);auto texture=ci;texture.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 auto textureImage=h<VkImage>(14);auto textureView=h<VkImageView>(15);materials.createImage(device,textureImage,texture);vi.image=textureImage;materials.createView(device,textureView,vi);
 materials.allocateSets(device,ai,&hot);w.dstSet=hot;ii.imageView=textureView;
 for(unsigned n=0;n<300;++n){w.dstBinding=n;materials.update(device,1,&w,0,nullptr);}
 w.dstBinding=999;ii.imageView=view;materials.update(device,1,&w,0,nullptr);auto target=observe(materials,hot);
 assert(target.reads.size()==1 && !target.partial && materials.counters().slotDrops==0);
 // Replacing that candidate with a material clears the previously recorded edge.
 ii.imageView=textureView;materials.update(device,1,&w,0,nullptr);assert(observe(materials,hot).reads.empty());
}
int main(){
 descriptorChurn();
 Resources r;auto d=h<VkDevice>(1),other=h<VkDevice>(2);
 auto image=h<VkImage>(3);auto view=h<VkImageView>(4);auto set=h<VkDescriptorSet>(5);auto pool=h<VkDescriptorPool>(6);
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.extent={3892,3126,1};ci.format=VK_FORMAT_R16G16B16A16_UNORM;
 ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.mipLevels=1;ci.arrayLayers=1;
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.format=ci.format;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
 vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};r.createImage(d,image,ci);r.createView(d,view,vi);
 VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;r.allocateSets(d,ai,&set);
 VkDescriptorImageInfo ii{};ii.imageView=view;
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.descriptorCount=1;write.pImageInfo=&ii;
 r.update(d,1,&write,0,nullptr);BoundSets bound;bound.bind(h<VkPipelineLayout>(7),0,1,&set);Uses a;r.collect(d,bound,a);
 assert(a.reads.size()==1 && a.storage.empty() && !a.partial);auto old=a.reads[0];assert(old->image->format==VK_FORMAT_R16G16B16A16_UNORM);
 // Format91, previously excluded by early VRS, is observed without classifying it.
 r.observeEye(d,image,0);r.observeEye(d,image,1);assert(old->image->eyeMask==3);
 r.createImage(other,image,ci);r.createView(other,view,vi);r.allocateSets(other,ai,&set);r.update(other,1,&write,0,nullptr);
 Uses b;r.collect(other,bound,b);assert(b.reads.size()==1 && b.reads[0]->image->id!=old->image->id && b.reads[0]->image->eyeMask==0);
 // Destruction and handle reuse cannot alias an earlier sampled observation.
 r.destroyView(d,view);r.destroyImage(d,image);r.createImage(d,image,ci);r.createView(d,view,vi);r.update(d,1,&write,0,nullptr);
 Uses fresh;r.collect(d,bound,fresh);assert(fresh.reads[0]->id!=old->id && fresh.reads[0]->image->id!=old->image->id);
 assert(old->image->eyeMask==3 && fresh.reads[0]->image->eyeMask==0);
 // Storage images are separately reported; read-vs-write is deliberately unknown.
 write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;r.update(d,1,&write,0,nullptr);Uses storage;r.collect(d,bound,storage);
 assert(storage.reads.empty() && storage.storage.size()==1);
 // Ordinary material textures are not candidate render-target links.
 auto tex=h<VkImage>(8);auto texView=h<VkImageView>(9);auto texture=ci;texture.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 r.createImage(d,tex,texture);vi.image=tex;r.createView(d,texView,vi);ii.imageView=texView;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
 r.update(d,1,&write,0,nullptr);Uses material;r.collect(d,bound,material);assert(material.reads.empty() && !material.partial);
 // Missing definitions and multi-element writes must never retain false edges.
 ii.imageView=h<VkImageView>(999);r.update(d,1,&write,0,nullptr);Uses missing;r.collect(d,bound,missing);assert(missing.partial && missing.reads.empty());
 ii.imageView=view;write.descriptorCount=2;r.update(d,1,&write,0,nullptr);Uses array;r.collect(d,bound,array);assert(array.partial && array.reads.empty());
 r.resetPool(d,pool);Uses reset;r.collect(d,bound,reset);assert(reset.partial);Uses independent;r.collect(other,bound,independent);assert(!independent.partial);
 // Single-element copies retain the view generation, not a reusable handle.
 write.descriptorCount=1;r.allocateSets(d,ai,&set);r.update(d,1,&write,0,nullptr);
 auto dest=h<VkDescriptorSet>(10);r.allocateSets(d,ai,&dest);VkCopyDescriptorSet cp{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};cp.srcSet=set;cp.dstSet=dest;cp.descriptorCount=1;
 r.update(d,0,nullptr,1,&cp);BoundSets copied;copied.bind(h<VkPipelineLayout>(7),0,1,&dest);Uses copy;r.collect(d,copied,copy);
 assert(copy.reads.size()==1 && copy.reads[0]->id==fresh.reads[0]->id);
 r.unknown(d,set);r.update(d,0,nullptr,1,&cp);Uses unknownCopy;r.collect(d,copied,unknownCopy);assert(unknownCopy.partial && unknownCopy.reads.empty());
 // A layout change discards retained sets rather than assuming compatibility.
 copied.bind(h<VkPipelineLayout>(11),1,1,&dest);assert(!copied.sets[0] && copied.partial);
 // Attachment slot roles and subresources are captured independently of timing.
 auto pass=h<VkRenderPass>(12);auto fb=h<VkFramebuffer>(13);auto depth=h<VkImageView>(14);vi.image=image;vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,2,1,1,1};r.createView(d,depth,vi);
 VkAttachmentReference colorRef{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthRef{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
 VkSubpassDescription sub{};sub.colorAttachmentCount=1;sub.pColorAttachments=&colorRef;sub.pDepthStencilAttachment=&depthRef;
 VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.subpassCount=1;rp.pSubpasses=&sub;r.pass(d,pass,rp);
 std::array<VkImageView,2> attachments{depth,view};VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.attachmentCount=2;fi.pAttachments=attachments.data();r.framebuffer(d,fb,fi);
 auto targets=r.targets(d,pass,fb);assert(!targets.partial && targets.color->id==fresh.reads[0]->id && targets.depth->range.baseMipLevel==2 && targets.depth->range.baseArrayLayer==1);
 VkImageViewUsageCreateInfo extra{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};vi.pNext=&extra;
 r.createView(d,depth,vi);r.framebuffer(d,fb,fi);assert(r.targets(d,pass,fb).partial);vi.pNext=nullptr;
 r.forgetFramebuffer(d,fb);assert(r.targets(d,pass,fb).partial);assert(targets.depth->image); // Sample owns its lifetime snapshot.
 fi.flags=VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;r.framebuffer(d,fb,fi);assert(r.targets(d,pass,fb).partial);
 // Bound sizes produce explicit incompleteness, not unbounded per-draw data.
 Uses capped;for(unsigned n=0;n<40;++n){auto v=std::make_shared<View>();v->id=100+n;v->image=old->image;Uses::add(capped.reads,v,capped.partial);}
 assert(capped.partial && capped.reads.size()==32);
 for(uint32_t n=0;n<140;++n){write.dstBinding=n;r.update(d,1,&write,0,nullptr);}assert(r.dropped()>0);
 r.transfer(d,image,tex,"CopyImage",1);r.transfer(d,image,tex,"CopyImage",1);unsigned edges=0;
 r.reportTransfers([&](const auto& s,const auto& t,const auto& op,uint32_t regions,uint64_t count){assert(s->id!=t->id && op=="CopyImage" && regions==1 && count==2);++edges;});assert(edges==1);
 r.reportTransfers([&](const auto&,const auto&,const auto&,auto,auto){assert(false);});
 r.destroyDevice(d);Uses gone;r.collect(d,bound,gone);assert(gone.partial);
 Uses retained;r.collect(other,bound,retained);assert(retained.reads.size()==1 && !retained.partial);
 std::cout<<"GPU resource generations, descriptor churn/recovery, device isolation, targets, caps and transfers passed\n";
}
