#pragma once
// M4.5: Explicit Vulkan GPU upload of the FFR R8_UINT shading-rate attachment.
// This is a reusable building block, NOT automatically installed into BeamNG render passes.
#include "ffr_pattern.hpp"
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>
#include <cstring>

namespace beamng_ffr {
// Explicit dispatch prevents the implicit Vulkan-loader calls in M4.5 from
// bypassing layer dispatch or accidentally resolving through our own hooks.
struct Api {
 PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
 PFN_vkCreateImage createImage = nullptr;
 PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
 PFN_vkAllocateMemory allocateMemory = nullptr;
 PFN_vkBindImageMemory bindImageMemory = nullptr;
 PFN_vkCreateBuffer createBuffer = nullptr;
 PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
 PFN_vkBindBufferMemory bindBufferMemory = nullptr;
 PFN_vkMapMemory mapMemory = nullptr;
 PFN_vkUnmapMemory unmapMemory = nullptr;
 PFN_vkCreateCommandPool createCommandPool = nullptr;
 PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
 // Optional for reuse; missing reset functions retain create/destroy behavior.
 PFN_vkResetCommandPool resetCommandPool = nullptr;
 PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
 PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
 PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
 PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
 PFN_vkCreateFence createFence = nullptr;
 PFN_vkResetFences resetFences = nullptr;
 PFN_vkQueueSubmit queueSubmit = nullptr;
 PFN_vkWaitForFences waitForFences = nullptr;
 PFN_vkCreateImageView createImageView = nullptr;
 PFN_vkDestroyImageView destroyImageView = nullptr;
 PFN_vkDestroyImage destroyImage = nullptr;
 PFN_vkFreeMemory freeMemory = nullptr;
 PFN_vkDestroyFence destroyFence = nullptr;
 PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
 PFN_vkDestroyBuffer destroyBuffer = nullptr;
 PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
 bool complete() const {
  return getPhysicalDeviceMemoryProperties && createImage && getImageMemoryRequirements && allocateMemory &&
   bindImageMemory && createBuffer && getBufferMemoryRequirements && bindBufferMemory && mapMemory &&
   unmapMemory && createCommandPool && allocateCommandBuffers && beginCommandBuffer && cmdPipelineBarrier &&
   cmdCopyBufferToImage && endCommandBuffer && createFence && queueSubmit && waitForFences &&
   createImageView && destroyImageView && destroyImage && freeMemory && destroyFence &&
   destroyCommandPool && destroyBuffer && deviceWaitIdle;
 }
};
struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};
inline void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(where) + " VkResult=" + std::to_string(r));
}
inline uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t mask, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No suitable Vulkan memory type");
}
// M8.12a: persistently mapped, reusable upload staging slots.
// PREPARATION ONLY: slots are not submitted to Vulkan and cannot race rendering.
// M8.12b must implement fence-based ownership before any transfer submission.
struct StagingSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
};
inline void destroyStaging(const Api& api, VkDevice device, StagingSlot& slot) {
    if (slot.mapped && slot.memory) api.unmapMemory(device, slot.memory);
    if (slot.buffer) api.destroyBuffer(device, slot.buffer, nullptr);
    if (slot.memory) api.freeMemory(device, slot.memory, nullptr);
    slot = {};
}
inline StagingSlot createStaging(const Api& api, VkPhysicalDevice physical, VkDevice device, VkDeviceSize bytes) {
    if (!api.complete() || !physical || !device || !bytes) throw std::invalid_argument("Invalid staging parameters");
    StagingSlot slot{};
    try {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(api.createBuffer(device, &bi, nullptr, &slot.buffer), "M8.12a vkCreateBuffer");
        VkMemoryRequirements req{};
        api.getBufferMemoryRequirements(device, slot.buffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        api.getPhysicalDeviceMemoryProperties(physical, &props);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(props, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(api.allocateMemory(device, &ai, nullptr, &slot.memory), "M8.12a vkAllocateMemory");
        check(api.bindBufferMemory(device, slot.buffer, slot.memory, 0), "M8.12a vkBindBufferMemory");
        check(api.mapMemory(device, slot.memory, 0, bytes, 0, &slot.mapped), "M8.12a vkMapMemory");
        slot.capacity = bytes;
        return slot;
    } catch (...) {
        destroyStaging(api, device, slot);
        throw;
    }
}
// M8.12c: record a transfer into an existing shading-rate image.
// CALLER CONTRACT (essential): the command buffer must be recording outside a
// render pass; the staging slot must stay immutable until submission completes;
// destination accesses must be ordered (these barriers cover earlier/later
// accesses on the SAME queue; other queues need caller synchronization); and
// the command buffer must execute before any render pass reading that image.
// This function records commands only. It does NOT submit or establish ownership.
inline void recordGazeTransfer(const Api& api, VkCommandBuffer cmd,
                               const StagingSlot& slot, const GpuImage& target) {
    if (!api.cmdPipelineBarrier || !api.cmdCopyBufferToImage || !cmd ||
        !slot.buffer || !target.image || !target.width || !target.height ||
        slot.capacity < VkDeviceSize(target.width) * target.height)
        throw std::invalid_argument("M8.12c invalid gaze transfer resources");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    api.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {target.width, target.height, 1};
    api.cmdCopyBufferToImage(cmd, slot.buffer, target.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    api.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}
inline void destroy(const Api& api, VkDevice device, GpuImage& out) {
    if (out.view) api.destroyImageView(device, out.view, nullptr);
    if (out.image) api.destroyImage(device, out.image, nullptr);
    if (out.memory) api.freeMemory(device, out.memory, nullptr);
    out = {};
}
// Must be called when the supplied graphics queue is not being concurrently used by another thread.
// queueFamilyIndex must support graphics and transfer operations; the caller owns the queue and device.
// This synchronous upload is intended for setup, not per-frame updates.
inline GpuImage upload(const Api& api, VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                       uint32_t queueFamilyIndex, const Pattern& pattern) {
    if (!api.complete() || !device || !physical || !queue || !pattern.width || !pattern.height ||
        pattern.texels.size() != size_t(pattern.width) * pattern.height)
        throw std::invalid_argument("Invalid GPU upload arguments");
    VkPhysicalDeviceMemoryProperties mem{};
    api.getPhysicalDeviceMemoryProperties(physical, &mem);
    GpuImage result{};
    result.width = pattern.width; result.height = pattern.height;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    try {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8_UINT;
        imageInfo.extent = {pattern.width, pattern.height, 1};
        imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(api.createImage(device, &imageInfo, nullptr, &result.image), "vkCreateImage");
        VkMemoryRequirements req{};
        api.getImageMemoryRequirements(device, result.image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(api.allocateMemory(device, &alloc, nullptr, &result.memory), "api.allocateMemory(image)");
        check(api.bindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = pattern.texels.size();
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(api.createBuffer(device, &bufferInfo, nullptr, &staging), "vkCreateBuffer");
        api.getBufferMemoryRequirements(device, staging, &req);
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(mem, req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(api.allocateMemory(device, &alloc, nullptr, &stagingMemory), "api.allocateMemory(staging)");
        check(api.bindBufferMemory(device, staging, stagingMemory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        check(api.mapMemory(device, stagingMemory, 0, bufferInfo.size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, pattern.texels.data(), pattern.texels.size());
        api.unmapMemory(device, stagingMemory);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(api.createCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(api.allocateCommandBuffers(device, &cmdInfo, &cmd), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.beginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = result.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        api.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {pattern.width, pattern.height, 1};
        api.cmdCopyBufferToImage(cmd, staging, result.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
        api.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(api.endCommandBuffer(cmd), "vkEndCommandBuffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(api.createFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        check(api.queueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        check(api.waitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = result.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8_UINT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        check(api.createImageView(device, &viewInfo, nullptr, &result.view), "vkCreateImageView");
        api.destroyFence(device, fence, nullptr);
        api.destroyCommandPool(device, pool, nullptr);
        api.destroyBuffer(device, staging, nullptr);
        api.freeMemory(device, stagingMemory, nullptr);
        return result;
    } catch (...) {
        // On submission failures, the caller must ensure device/queue work has quiesced
        // before releasing these resources; do not attempt to recover a device-lost GPU.
        if (device) api.deviceWaitIdle(device); // Best effort; a lost device may not recover.
        if (fence) api.destroyFence(device, fence, nullptr);
        if (pool) api.destroyCommandPool(device, pool, nullptr);
        if (staging) api.destroyBuffer(device, staging, nullptr);
        if (stagingMemory) api.freeMemory(device, stagingMemory, nullptr);
        destroy(api, device, result);
        throw;
    }
}
} // namespace beamng_ffr
