/**
 * frame_gen.cpp - Vulkan LSFG pipeline for mpvlibAndroid.
 * Ported from Eden Emulator (Eden Project, GPL-3.0-or-later).
 * Integrates with mpv --vo=gpu-next --gpu-api=vulkan --hwdec=mediacodec.
 *
 * RENDER LOOP INTEGRATION:
 *   - mpv switched to mpv_render_context_create() (we own the Vulkan context)
 *   - After mpv_render_context_render() writes frame N into real_frame VkImage:
 *       fg.Process(cmd, real_frame, extent, format)
 *       for(size_t g = 0; g < fg.GeneratedCount(multiplier); g++) {
 *           fg.GenerateInto(cmd, generated_image[g], extent, g)
 *           queue_present(generated_image[g])  // extra frames hit vsync slots
 *       }
 *       queue_present(real_frame)
 */

#include "frame_gen.h"
#include "lossless_dll.h"
#include <android/log.h>
#include <cstring>

#define TAG "FrameGen_VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace FrameGen {

static VulkanFrameGen g_frame_gen;
VulkanFrameGen& GetFrameGen() { return g_frame_gen; }

bool VulkanSupported() {
#if defined(__aarch64__)
    return true;  // All modern Adreno/Mali/Xclipse on arm64 support required extensions
#else
    return false; // x86 emulators typically do not
#endif
}

// Helpers forwarding lossless_dll namespace to FrameGen namespace
int  GetInstalledLosslessStatus() { return (int)FrameGen::GetInstalledLosslessStatus(); }
int  BuildShaderCache()           { return (int)FrameGen::BuildShaderCache(); }
bool RemoveInstalledLosslessDll() { return FrameGen::RemoveInstalledLosslessDll(); }

static VkShaderModule MakeShaderModule(VkDevice dev, const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode    = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

static bool MakeImage(VkDevice dev, VkPhysicalDevice pdev, VkExtent2D ext,
                      VkFormat fmt, VkImageUsageFlags usage,
                      VkImage& img, VkDeviceMemory& mem) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = {ext.width, ext.height, 1};
    ici.mipLevels   = 1; ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = usage;
    if (vkCreateImage(dev, &ici, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { mt = i; break; }
    if (mt == UINT32_MAX) { vkDestroyImage(dev, img, nullptr); return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS)
        { vkDestroyImage(dev, img, nullptr); return false; }
    vkBindImageMemory(dev, img, mem, 0);
    return true;
}

VulkanFrameGen::~VulkanFrameGen() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);
    FreeWorkImages();
    if (pipe_mipmaps_)  vkDestroyPipeline(device_, pipe_mipmaps_,  nullptr);
    if (pipe_generate_) vkDestroyPipeline(device_, pipe_generate_, nullptr);
    if (layout_)        vkDestroyPipelineLayout(device_, layout_, nullptr);
}

bool VulkanFrameGen::Init(VkDevice dev, VkPhysicalDevice pdev, VkQueue q, uint32_t qf) {
    device_ = dev; physdev_ = pdev; queue_ = q; queue_family_ = qf;
    ShaderModules mods;
    if (LoadShaderModules(mods) != LosslessStatus::Ok) { LOGE("LoadShaderModules failed"); return false; }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout_) != VK_SUCCESS) return false;
    auto make_pipe = [&](uint32_t id, VkPipeline& out) {
        if (!mods.count(id)) return;
        VkShaderModule mod = MakeShaderModule(dev, mods[id]);
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        cpci.layout = layout_;
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &out);
        vkDestroyShaderModule(dev, mod, nullptr);
    };
    make_pipe(ShaderID::MIPMAPS,  pipe_mipmaps_);
    make_pipe(ShaderID::GENERATE, pipe_generate_);
    ready_ = pipe_mipmaps_ != VK_NULL_HANDLE && pipe_generate_ != VK_NULL_HANDLE;
    LOGI("VulkanFrameGen::Init ready=%d (mipmaps=%p generate=%p)", ready_,
         (void*)pipe_mipmaps_, (void*)pipe_generate_);
    return ready_;
}

void VulkanFrameGen::FreeWorkImages() {
    auto del = [&](VkImage& i, VkDeviceMemory& m) {
        if (i) { vkDestroyImage(device_, i, nullptr); i = VK_NULL_HANDLE; }
        if (m) { vkFreeMemory(device_, m, nullptr);   m = VK_NULL_HANDLE; }
    };
    del(img_prev_, mem_prev_); del(img_curr_, mem_curr_); del(img_flow_, mem_flow_);
    built_extent_ = {}; built_format_ = VK_FORMAT_UNDEFINED; has_prev_ = false;
}

void VulkanFrameGen::AllocateWorkImages(VkExtent2D ext, VkFormat fmt) {
    FreeWorkImages();
    constexpr auto u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr auto fu = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    MakeImage(device_, physdev_, ext, fmt, u, img_prev_, mem_prev_);
    MakeImage(device_, physdev_, ext, fmt, u, img_curr_, mem_curr_);
    MakeImage(device_, physdev_, ext, VK_FORMAT_R32G32_SFLOAT, fu, img_flow_, mem_flow_);
    built_extent_ = ext; built_format_ = fmt;
    LOGI("Work images %ux%u", ext.width, ext.height);
}

void VulkanFrameGen::Process(VkCommandBuffer cmd, VkImage real, VkExtent2D ext, VkFormat fmt) {
    if (!ready_) return;
    if (built_extent_.width != ext.width || built_extent_.height != ext.height || built_format_ != fmt)
        AllocateWorkImages(ext, fmt);

    // Copy real frame into img_curr_
    auto barrier = [&](VkImage img, VkImageLayout ol, VkImageLayout nl,
                        VkAccessFlags sa, VkAccessFlags da,
                        VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = ol; b.newLayout = nl;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(real, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(img_curr_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {(int)ext.width, (int)ext.height, 1};
    vkCmdBlitImage(cmd, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   img_curr_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    // Mipmaps pass (builds optical-flow inputs)
    if (has_prev_) {
        barrier(img_curr_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_mipmaps_);
        struct { float t; uint32_t w, h, p; } pc{0.5f, ext.width, ext.height, 0};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
        vkCmdDispatch(cmd, (ext.width+7)/8, (ext.height+7)/8, 1);
    }
    has_prev_ = true;
    std::swap(img_prev_, img_curr_); std::swap(mem_prev_, mem_curr_);
}

size_t VulkanFrameGen::GeneratedCount(int mult) const {
    return (!ready_ || !has_prev_) ? 0 : (size_t)(mult - 1);
}

void VulkanFrameGen::GenerateInto(VkCommandBuffer cmd, VkImage dst, VkExtent2D ext, size_t idx) {
    if (!ready_) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.image = dst; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_generate_);
    struct { float t; uint32_t w, h, p; } pc;
    pc.t = (float)(idx+1)/(float)(idx+2); pc.w = ext.width; pc.h = ext.height; pc.p = 0;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
    vkCmdDispatch(cmd, (ext.width+7)/8, (ext.height+7)/8, 1);
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanFrameGen::Invalidate() { FreeWorkImages(); }

} // namespace FrameGen