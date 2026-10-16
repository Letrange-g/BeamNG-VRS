#include "gpu_resource_trace.hpp"
#include <cassert>
#include <iostream>
using namespace beamng_trace;
template<class T>T h(uintptr_t n){return reinterpret_cast<T>(n);}
int main(){
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
 std::cout<<"GPU resource generations, device isolation, descriptors, targets, caps and transfers passed\n";
}
