//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace Vulkan {
class Instance;
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        /// 0..1: how much the picture is sharpened on the way (host_shaders/post_process.frag).
        float sharpen = 0.0f;
        /// For an output image of an sRGB format, which does the encoding for display itself.
        u32 linear_out = 0;
        /// The input is read as the bytes it holds, already encoded for display (an sRGB image
        /// seen through a view of its UNORM sibling, with `gamma` 1): nothing to encode.
        u32 encoded_in = 0;
        /// Within `centre_radius` of the middle of the picture (a share of the way to its edge,
        /// on both axes) it is sharpened by `sharpen_centre` instead: where FSR 1 enlarged the
        /// scene, which leaves it crisp already (TitleUpscale). 0: no such part.
        float sharpen_centre = 0.0f;
        float centre_radius = 0.0f;
    };

    void Create(const Instance& instance, MasterSemaphore* master_semaphore,
                vk::Format surface_format);

    /// An input image and the part of the output frame it is drawn into.
    struct Region {
        vk::ImageView input;
        vk::Rect2D area;
        std::optional<vk::Rect2D> clip;
    };

    // Frame marker: a row of black and white blocks stamped along the top-left edge of a frame.
    // Two sync blocks (white, black) are followed by a 16 bit frame id and an 8 bit check value,
    // least significant bit first. It lets a host compositor recognise which emulated frame it
    // is looking at after the image has travelled through a window system.
    static constexpr u32 MarkerBlockSize = 8;
    static constexpr u32 MarkerSyncBlocks = 2;
    static constexpr u32 MarkerIdBits = 16;
    static constexpr u32 MarkerCheckBits = 8;
    static constexpr u32 MarkerBlocks = MarkerSyncBlocks + MarkerIdBits + MarkerCheckBits;
    static constexpr u32 MarkerCheck(u32 id) {
        return ((id & 0xff) ^ ((id >> 8) & 0xff) ^ 0x5a) & 0xff;
    }

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

    /// Draws every region into the output frame and optionally stamps a frame marker.
    void Render(vk::CommandBuffer cmdbuf, std::span<const Region> regions, Frame& output,
                Settings settings, std::optional<u32> marker = std::nullopt);

private:
    vk::Device device{};
    bool uses_push_descriptors{};
    // Pool sizes must outlive desc_heap (DescriptorHeap stores a span to it).
    static constexpr std::array<vk::DescriptorPoolSize, 1> pool_sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 64},
    }};
    DescriptorHeap desc_heap;
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
