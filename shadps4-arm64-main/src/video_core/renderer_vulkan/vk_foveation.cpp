// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_foveation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

/// How a texel of a map says how coarsely to shade: log2 of the width, twice, then of the height.
constexpr u8 Rate(u32 width, u32 height) {
    return static_cast<u8>((std::countr_zero(width) << 2) | std::countr_zero(height));
}

/// From how far out (as a share of the way from the middle to the edge, counted on both axes)
/// the picture is shaded in blocks of 2x2, and in blocks of 4x4, at each level.
struct Rings {
    float coarse;
    float coarsest;
};
constexpr Rings LevelRings[Foveation::Strongest + 1] = {
    {2.0f, 2.0f},   // off
    {0.80f, 2.0f},  // half of the picture (its corners are a lot of it)
    {0.62f, 2.0f},  // seven tenths
    {0.50f, 0.85f}, // four fifths, the far corners in blocks of 4x4
    {0.40f, 0.70f}, // nine tenths, three fifths of the picture in blocks of 4x4
};

} // namespace

int Foveation::Level() {
    static const int level = [] {
        const char* value = std::getenv("SHADPS4_VR_FOVEATION");
        return value != nullptr ? std::clamp(std::atoi(value), 0, Strongest) : 0;
    }();
    return level;
}

float Foveation::CoarseFrom() {
    return LevelRings[Level()].coarse;
}

Foveation::Foveation(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    if (Level() == 0) {
        return;
    }
    if (!instance.IsFoveationSupported()) {
        LOG_WARNING(Render_Vulkan, "Foveation was asked for, but the GPU cannot shade coarsely "
                                   "where a map says so: the pictures are shaded in full");
        return;
    }
    level = Level();
    texel = instance.FoveationTexel();
    LOG_INFO(Render_Vulkan,
             "Foveation {}: an eye's scene is shaded in blocks of 2x2 pixels from {:.0f}% of the "
             "way to its edges{} (maps of {}x{} pixels a texel)",
             level, LevelRings[level].coarse * 100.0f,
             LevelRings[level].coarsest < 1.5f
                 ? fmt::format(", in blocks of 4x4 from {:.0f}%", LevelRings[level].coarsest * 100.0f)
                 : std::string{},
             texel, texel);
}

Foveation::~Foveation() {
    const auto device = instance.GetDevice();
    for (const Map& map : maps) {
        if (map.view) {
            device.destroyImageView(map.view);
        }
        if (map.image) {
            vmaDestroyImage(instance.GetAllocator(), static_cast<VkImage>(map.image),
                            map.allocation);
        }
    }
}

vk::ImageView Foveation::MapFor(u32 width, u32 height) {
    if (level == 0 || width == 0 || height == 0) {
        return {};
    }
    const int wanted = level > 1 && FrameStats::AbOff("fov") ? level - 1 : level;
    for (const Map& map : maps) {
        if (map.width == width && map.height == height && map.level == wanted) {
            return map.view;
        }
    }
    Map map{.width = width, .height = height, .level = wanted};
    if (!Make(map)) {
        LOG_WARNING(Render_Vulkan, "Foveation: no map for {}x{}: that size is shaded in full",
                    width, height);
        // Not tried again: the size is in the list without a view.
    }
    maps.push_back(map);
    return map.view;
}

bool Foveation::Make(Map& map) {
    const u32 columns = (map.width + texel - 1) / texel;
    const u32 rows = (map.height + texel - 1) / texel;
    const auto device = instance.GetDevice();

    const VkImageCreateInfo image_ci = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Uint,
        .extent = {columns, rows, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eFragmentShadingRateAttachmentKHR |
                 vk::ImageUsageFlagBits::eTransferDst,
    });
    const VmaAllocationCreateInfo image_alloc_ci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(), &image_ci, &image_alloc_ci, &image,
                       &map.allocation, nullptr) != VK_SUCCESS) {
        return false;
    }
    map.image = vk::Image{image};
    const auto [view_result, view] = device.createImageView({
        .image = map.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Uint,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (view_result != vk::Result::eSuccess) {
        return false;
    }

    // The rates, texel by texel: the middle of each texel, measured from the middle of the
    // picture as a share of the way to its edge on each axis.
    const Rings rings = LevelRings[map.level];
    const u8 coarsest = instance.IsFoveationRateSupported(4, 4) ? Rate(4, 4) : Rate(2, 2);
    std::vector<u8> rates(size_t{columns} * rows);
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            const float x = ((column + 0.5f) * texel / map.width) * 2.0f - 1.0f;
            const float y = ((row + 0.5f) * texel / map.height) * 2.0f - 1.0f;
            const float out = std::sqrt(x * x + y * y);
            rates[size_t{row} * columns + column] = out >= rings.coarsest ? coarsest
                                                    : out >= rings.coarse  ? Rate(2, 2)
                                                                           : Rate(1, 1);
        }
    }

    // Through a buffer the CPU writes, kept until the GPU has copied it.
    const VkBufferCreateInfo buffer_ci = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = rates.size(),
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    });
    const VmaAllocationCreateInfo buffer_alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation buffer_allocation{};
    VmaAllocationInfo buffer_info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &buffer_alloc_ci, &buffer,
                        &buffer_allocation, &buffer_info) != VK_SUCCESS) {
        device.destroyImageView(view);
        return false;
    }
    std::memcpy(buffer_info.pMappedData, rates.data(), rates.size());
    vmaFlushAllocation(instance.GetAllocator(), buffer_allocation, 0, VK_WHOLE_SIZE);

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier2 to_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = map.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
    const vk::BufferImageCopy copy{
        .imageSubresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .layerCount = 1,
        },
        .imageExtent = {columns, rows, 1},
    };
    cmdbuf.copyBufferToImage(vk::Buffer{buffer}, map.image, vk::ImageLayout::eTransferDstOptimal,
                             copy);
    const vk::ImageMemoryBarrier2 to_use{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShadingRateAttachmentKHR,
        .dstAccessMask = vk::AccessFlagBits2::eFragmentShadingRateAttachmentReadKHR,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR,
        .image = map.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_use});
    const VmaAllocator allocator = instance.GetAllocator();
    scheduler.DeferOperation(
        [allocator, buffer, buffer_allocation] { vmaDestroyBuffer(allocator, buffer, buffer_allocation); });

    map.view = view;
    u32 coarse = 0;
    for (const u8 rate : rates) {
        coarse += rate != Rate(1, 1) ? 1 : 0;
    }
    LOG_INFO(Render_Vulkan, "Foveation: map for {}x{} made, {}% of it shaded coarsely",
             map.width, map.height, coarse * 100 / static_cast<u32>(rates.size()));
    return true;
}

} // namespace Vulkan
