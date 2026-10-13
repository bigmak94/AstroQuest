// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <tsl/robin_map.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

class Image;
class ImageView;
struct ImageInfo;

class BlitHelper {
    static constexpr size_t MaxMsPipelines = 6;

public:
    explicit BlitHelper(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~BlitHelper();

    // In both of these `samples` is how many samples the destination image really has. That is
    // the guest's count only where the host supports it: mobile GPUs stop at four, and a pipeline
    // that rasterizes more samples than its attachment holds garbles the image.
    void ReinterpretColorAsMsDepth(u32 width, u32 height, vk::SampleCountFlagBits samples,
                                   vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                   vk::Image source, vk::Image dest);

    void CopyBetweenMsImages(u32 width, u32 height, vk::SampleCountFlagBits samples,
                             vk::Format pixel_format, bool src_msaa, vk::Image source,
                             vk::Image dest);

    /// Whether a resolve of a target that the host holds with one sample a pixel smooths the
    /// edges in it (SmoothInto) instead of copying it as it is. SHADPS4_RESOLVE_AA=0: no.
    bool SmoothsResolves() const {
        return smooths_resolves;
    }

    /// Draws `source` into `dest`, both single sampled and of the given size, with the edges
    /// smoothed (host_shaders/resolve_aa.frag). The source has to be in a layout for reading
    /// in shaders and the destination in the one for colour attachments.
    /// `fine_only`: edges are smoothed only where an eye's scene is shaded pixel by pixel
    /// (Vulkan::Foveation), for a target of the size the scene is drawn at.
    void SmoothInto(u32 width, u32 height, vk::Format src_pixel_format, vk::Format dst_pixel_format,
                    vk::Image source, u32 src_layer, vk::Image dest, u32 dst_layer,
                    bool fine_only = false);

private:
    void CreateShaders();
    void CreatePipelineLayouts();

    struct MsPipelineKey {
        vk::SampleCountFlagBits samples;
        vk::Format attachment_format;
        bool src_msaa;

        auto operator<=>(const MsPipelineKey&) const noexcept = default;
    };
    void CreateColorToMSDepthPipeline(const MsPipelineKey& key);
    void CreateMsCopyPipeline(const MsPipelineKey& key);
    vk::Pipeline SmoothPipeline(vk::Format attachment_format, bool fine_only);

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    bool uses_push_descriptors{};
    // Pool sizes must outlive desc_heap (DescriptorHeap stores a span to it).
    static constexpr std::array<vk::DescriptorPoolSize, 2> pool_sizes{{
        {vk::DescriptorType::eSampledImage, 64},
        {vk::DescriptorType::eCombinedImageSampler, 64},
    }};
    Vulkan::DescriptorHeap desc_heap;
    vk::UniqueDescriptorSetLayout single_texture_descriptor_set_layout;
    vk::UniquePipelineLayout single_texture_pl_layout;
    vk::ShaderModule fs_tri_vertex;
    vk::ShaderModule color_to_ms_depth_frag;
    vk::ShaderModule src_msaa_copy_frag;
    vk::ShaderModule src_non_msaa_copy_frag;

    using MsPipeline = std::pair<MsPipelineKey, vk::UniquePipeline>;
    std::vector<MsPipeline> color_to_ms_depth_pl;
    std::vector<MsPipeline> ms_image_copy_pl;

    // Smoothing in place of a resolve: a texture read through a sampler, one pipeline for
    // every format drawn to.
    bool smooths_resolves{};
    vk::UniqueDescriptorSetLayout sampled_texture_descriptor_set_layout;
    vk::UniquePipelineLayout sampled_texture_pl_layout;
    vk::UniqueSampler linear_sampler;
    vk::ShaderModule resolve_aa_frag;
    /// The same, smoothing only the finely shaded middle; null without foveation.
    vk::ShaderModule resolve_aa_fine_frag;
    std::vector<std::pair<vk::Format, vk::UniquePipeline>> resolve_aa_pl;
    std::vector<std::pair<vk::Format, vk::UniquePipeline>> resolve_aa_fine_pl;
};

} // namespace VideoCore
