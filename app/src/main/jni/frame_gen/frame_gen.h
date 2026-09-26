/**
 * frame_gen.h
 *
 * Vulkan LSFG frame-generation pipeline for mpvlibAndroid.
 *
 * Ported from Eden Emulator's Vulkan::FrameGen.
 * Integrates with mpv's --vo=gpu-next --gpu-api=vulkan render context.
 *
 * Flow:
 *   1. Init(device, queue, shader_modules)  — build compute pipelines from SPIR-V
 *   2. Process(frame_image, extent, format) — feed real frame N as input
 *   3. GeneratedCount()                     — how many frames to generate
 *   4. GenerateInto(dst_image, generation)  — write generated frame
 *   5. Present real frame after all generated frames
 */

#pragma once
#include <vulkan/vulkan.h>
#include <optional>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace FrameGen {

struct Frame {
    VkImage     image     = VK_NULL_HANDLE;
    VkImageView view      = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkFence     fence     = VK_NULL_HANDLE;
};

class VulkanFrameGen {
public:
    VulkanFrameGen() = default;
    ~VulkanFrameGen();

    // Initialise pipelines from the cached SPIR-V shader modules.
    // Call once after BuildShaderCache() succeeds.
    bool Init(VkDevice device, VkPhysicalDevice physdev, VkQueue queue, uint32_t queue_family);

    // Feed real frame N (already rendered by mpv into dst_image).
    void Process(VkCommandBuffer cmd, VkImage real_frame, VkExtent2D extent, VkFormat format);

    // How many generated frames to insert before the real frame.
    size_t GeneratedCount(int multiplier) const;

    // Render generated frame `index` into `dst`.
    void GenerateInto(VkCommandBuffer cmd, VkImage dst, VkExtent2D extent, size_t index);

    // Must be called if resolution or format changes.
    void Invalidate();

    bool IsReady() const { return ready_; }

private:
    void BuildPipelines(VkExtent2D extent, VkFormat format);
    void AllocateWorkImages(VkExtent2D extent, VkFormat format);
    void FreeWorkImages();

    VkDevice         device_       = VK_NULL_HANDLE;
    VkPhysicalDevice physdev_      = VK_NULL_HANDLE;
    VkQueue          queue_        = VK_NULL_HANDLE;
    uint32_t         queue_family_ = 0;

    // Compute pipelines for each LSFG stage
    VkPipeline       pipe_mipmaps_  = VK_NULL_HANDLE;
    VkPipeline       pipe_generate_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_        = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_     = VK_NULL_HANDLE;

    // Working images: prev frame, current frame, flow field, output
    VkImage          img_prev_   = VK_NULL_HANDLE;
    VkImage          img_curr_   = VK_NULL_HANDLE;
    VkImage          img_flow_   = VK_NULL_HANDLE;
    VkDeviceMemory   mem_prev_   = VK_NULL_HANDLE;
    VkDeviceMemory   mem_curr_   = VK_NULL_HANDLE;
    VkDeviceMemory   mem_flow_   = VK_NULL_HANDLE;

    VkExtent2D built_extent_{0, 0};
    VkFormat   built_format_  = VK_FORMAT_UNDEFINED;
    bool       ready_         = false;
    bool       has_prev_      = false;
};

// Global singleton accessed from render.cpp
VulkanFrameGen& GetFrameGen();

// Forwarded from lossless_dll.h
bool        VulkanSupported();
std::string GetLosslessDllPath();
std::string GetShaderCachePath();
int         GetInstalledLosslessStatus(); // LosslessStatus enum value
int         BuildShaderCache();
bool        RemoveInstalledLosslessDll();

} // namespace FrameGen