// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <cstdlib>
#include <ranges>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/known_title.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_foveation.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance* instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci,
                         std::span<const vk::Format> view_formats_) {
    this->image_ci = image_ci;
    view_formats.assign(view_formats_.begin(), view_formats_.end());
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    const VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = image_ci_unsafe.pNext,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = reinterpret_cast<const VkFormat*>(view_formats.data()),
    };
    if (!view_formats.empty()) {
        image_ci_unsafe.pNext = &format_list;
    }
    VkImage unsafe_image{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_info, &unsafe_image,
                                     &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
}

namespace {

/// The same format with the other transfer function, if there is one.
vk::Format SrgbSibling(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Unorm:
        return vk::Format::eB8G8R8A8Srgb;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eBc1RgbUnormBlock:
        return vk::Format::eBc1RgbSrgbBlock;
    case vk::Format::eBc1RgbSrgbBlock:
        return vk::Format::eBc1RgbUnormBlock;
    case vk::Format::eBc1RgbaUnormBlock:
        return vk::Format::eBc1RgbaSrgbBlock;
    case vk::Format::eBc1RgbaSrgbBlock:
        return vk::Format::eBc1RgbaUnormBlock;
    case vk::Format::eBc2UnormBlock:
        return vk::Format::eBc2SrgbBlock;
    case vk::Format::eBc2SrgbBlock:
        return vk::Format::eBc2UnormBlock;
    case vk::Format::eBc3UnormBlock:
        return vk::Format::eBc3SrgbBlock;
    case vk::Format::eBc3SrgbBlock:
        return vk::Format::eBc3UnormBlock;
    case vk::Format::eBc7UnormBlock:
        return vk::Format::eBc7SrgbBlock;
    case vk::Format::eBc7SrgbBlock:
        return vk::Format::eBc7UnormBlock;
    default:
        return vk::Format::eUndefined;
    }
}

/// The formats an image of `format` is created to be viewed in: its own and, where one exists,
/// the one that differs in the transfer function only (titles render to one and sample the
/// other). SHADPS4_IMAGE_FORMATS=any gives every image the freedom of any compatible format,
/// as it was before; =own leaves the sibling out (which exercises the fallback).
boost::container::static_vector<vk::Format, 4> ViewFormatsFor(vk::Format format, bool is_depth) {
    static const int mode = [] {
        const char* value = std::getenv("SHADPS4_IMAGE_FORMATS");
        if (value == nullptr) {
            return 0;
        }
        return std::strcmp(value, "any") == 0 ? 1 : std::strcmp(value, "own") == 0 ? 2 : 0;
    }();
    boost::container::static_vector<vk::Format, 4> formats;
    // Depth buffers are viewed as what they are; drivers do not hold their mutability
    // against them.
    if (mode == 1 || is_depth) {
        return formats;
    }
    formats.push_back(format);
    if (const auto sibling = SrgbSibling(format); mode != 2 && sibling != vk::Format::eUndefined) {
        formats.push_back(sibling);
    }
    return formats;
}

} // namespace

Image::Image(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
             BlitHelper& blit_helper_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_)
    : instance{&instance_}, scheduler{&scheduler_}, blit_helper{&blit_helper_},
      slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    // Here we force `eExtendedUsage` as don't know all image usage cases beforehand. In normal case
    // the texture cache should re-create the resource with the usage requested
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance->Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance->IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance->GetSupportedFormat(info.pixel_format, format_features);
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const auto image_format_properties =
        instance->GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;
    // Drivers have been seen to offer more samples for a format than they can render with.
    supported_samples &= instance->GetFramebufferSampleCounts();
    attachment_samples = supported_samples;
    if (usage_flags & vk::ImageUsageFlagBits::eStorage) {
        auto attachment_info = format_info;
        attachment_info.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        const auto attachment_properties =
            instance->GetPhysicalDevice().getImageFormatProperties2(attachment_info);
        if (attachment_properties.result == vk::Result::eSuccess) {
            attachment_samples = attachment_properties.value.imageFormatProperties.sampleCounts &
                                 instance->GetFramebufferSampleCounts();
        }
    }

    vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    SetHostSamples(image_ci, info.num_samples);

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    backing->image.Create(image_ci, ViewFormatsFor(image_ci.format, info.props.is_depth));

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

void Image::EnsureViewFormat(vk::Format format) {
    if (backing->image.AllowsViewFormat(format)) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "image {}x{} of format {} is to be viewed as {}: moving it to an image that "
                "allows any format",
                info.size.width, info.size.height, vk::to_string(backing->image.image_ci.format),
                vk::to_string(format));

    BackingImage* const old_backing = backing;
    const auto image_ci = old_backing->image.image_ci;
    // (A deque keeps its elements where they are when one is added at the end.)
    BackingImage* const new_backing = &backing_images.emplace_back();
    new_backing->num_samples = old_backing->num_samples;
    new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    new_backing->image.Create(image_ci);
    Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                          "Image {}x{}x{} {} {:#x}:{:#x} (any format)", info.size.width,
                          info.size.height, info.size.depth, vk::to_string(info.pixel_format),
                          info.guest_address, info.guest_size);

    // The contents move over as they are: both images have the same format.
    scheduler->EndRendering();
    auto barriers = GetBarriers(vk::ImageLayout::eTransferSrcOptimal,
                                vk::AccessFlagBits2::eTransferRead,
                                vk::PipelineStageFlagBits2::eTransfer, std::nullopt);
    barriers.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = new_backing->image,
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    });
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
    boost::container::small_vector<vk::ImageCopy, 14> regions;
    for (u32 mip = 0; mip < image_ci.mipLevels; ++mip) {
        regions.push_back(vk::ImageCopy{
            .srcSubresource{
                .aspectMask = aspect_mask,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image_ci.arrayLayers,
            },
            .dstSubresource{
                .aspectMask = aspect_mask,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image_ci.arrayLayers,
            },
            .extent{
                .width = std::max(image_ci.extent.width >> mip, 1u),
                .height = std::max(image_ci.extent.height >> mip, 1u),
                .depth = std::max(image_ci.extent.depth >> mip, 1u),
            },
        });
    }
    cmdbuf.copyImage(old_backing->image, vk::ImageLayout::eTransferSrcOptimal,
                     new_backing->image, vk::ImageLayout::eTransferDstOptimal, regions);

    new_backing->state.layout = vk::ImageLayout::eTransferDstOptimal;
    new_backing->state.access_mask = vk::AccessFlagBits2::eTransferWrite;
    new_backing->state.pl_stage = vk::PipelineStageFlagBits2::eTransfer;
    old_backing->retired = true;
    backing = new_backing;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        SetBackingSamples(info.num_samples);
    }
    // (The format was seen to for it then: a backing that cannot have it is left for another.)
    if (backing->last_view_id && backing->last_view_info == view_info &&
        !Vulkan::FrameStats::AbOff("textures")) {
        return (*slot_image_views)[backing->last_view_id];
    }
    EnsureViewFormat(ImageView::HostFormat(*instance, view_info, *this));
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    ImageViewId view_id;
    if (it != view_infos.end()) {
        view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
    } else {
        view_id = slot_image_views->insert(*instance, view_info, *this);
        backing->image_view_infos.emplace_back(view_info);
        backing->image_view_ids.emplace_back(view_id);
    }
    backing->last_view_info = view_info;
    backing->last_view_id = view_id;
    return (*slot_image_views)[view_id];
}

Image::Barriers Image::GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                                   vk::PipelineStageFlags2 dst_stage,
                                   std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    // A state that writes is followed by a barrier even when nothing changes, so only
    // requests that read can be known to need nothing.
    constexpr auto any_write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
    const bool settles = needs_partial_transition && !(dst_mask & any_write_flags);
    if (settles) {
        for (const auto& request : backing->settled) {
            if (request.state_version == backing->state_version && request.layout == dst_layout &&
                request.access_mask == dst_mask && request.range == *subres_range) {
                // What the last request was is remembered here as it is further down.
                last_state.layout = dst_layout;
                last_state.access_mask = dst_mask;
                last_state.pl_stage = dst_stage;
                return {};
            }
        }
    }

    Barriers barriers;
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
        if (!barriers.empty()) {
            ++backing->state_version;
        }
        if (settles) {
            backing->settled[backing->next_settled++ % backing->settled.size()] = {
                .range = *subres_range,
                .layout = dst_layout,
                .access_mask = dst_mask,
                .state_version = backing->state_version,
            };
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return {};
        }

        ++backing->state_version;
        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;

    return barriers;
}

void Image::Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                    std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf /*= {}*/) {
    // Adjust pipeline stage
    const vk::PipelineStageFlags2 dst_pl_stage =
        (dst_mask == vk::AccessFlagBits2::eTransferRead ||
         dst_mask == vk::AccessFlagBits2::eTransferWrite)
            ? vk::PipelineStageFlagBits2::eTransfer
            : vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;

    const auto barriers = GetBarriers(dst_layout, dst_mask, dst_pl_stage, range);
    if (barriers.empty()) {
        return;
    }

    if (!cmdbuf) {
        // When using external cmdbuf you are responsible for ending rp.
        scheduler->EndRendering();
        cmdbuf = scheduler->CommandBuffer();
    }
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::Barrier);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void Image::Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer,
                   u64 offset) {
    SetBackingSamples(info.num_samples, false);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = info.guest_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eCopy, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    // Provenance for late DEVICE_LOST digs. Keep this below normal release verbosity because
    // uploads happen many times per frame.
    LOG_TRACE(Render_Vulkan,
                "COPY_BUFFER_TO_IMAGE_PROV srcBuffer={:#x} offset={:#x} size={:#x} "
                "dstImage={:#x} tick={} copies={} width={} height={} guestSize={:#x} "
                "path=Image::Upload",
                u64(VkBuffer(buffer)), offset, info.guest_size, u64(VkImage(GetImage())),
                scheduler->CurrentTick(), static_cast<u32>(upload_copies.size()), info.size.width,
                info.size.height, info.guest_size);
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             upload_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
    flags &= ~ImageFlagBits::Dirty;
}

void Image::Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                     u64 offset, u64 download_size) {
    SetBackingSamples(info.num_samples);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eCopy, {});
    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             download_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

static std::pair<u32, u32> SanitizeCopyLayers(const ImageInfo& src_info, const ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

void Image::CopyImage(Image& src_image) {
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::ImageCopy);
    const auto& src_info = src_image.info;

    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src_info.pixel_format != info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src_info.pixel_format), vk::to_string(info.pixel_format));
    }

    const u32 base_width = src_info.size.width;
    const u32 base_height = src_info.size.height;
    const u32 base_depth =
        info.type == AmdGpu::ImageType::Color3D ? info.size.depth : src_info.size.depth;

    // Match sample count before copying
    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect =
        src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const vk::ImageAspectFlags dst_aspect = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src_info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src_info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d);

        vk::ImageCopy region{};

        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;

        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();

    if (!regions.empty()) {
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                         backing->state.layout, regions);
    }

    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}
void Image::CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset) {
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::ImageCopy);
    const auto& src_info = src_image.info;
    const u32 num_mips = std::min(src_info.resources.levels, info.resources.levels);
    const u32 num_layers = std::min(src_info.resources.layers, info.resources.layers);
    ASSERT(src_info.resources.layers == info.resources.layers || num_mips == 1);

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto mip_w = std::max(src_info.size.width >> mip, 1u);
        const auto mip_h = std::max(src_info.size.height >> mip, 1u);
        const auto mip_d = std::max(src_info.size.depth >> mip, 1u);

        buffer_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_w, mip_h, mip_d},
        });
    }

    const vk::BufferMemoryBarrier2 pre_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    const vk::BufferMemoryBarrier2 post_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    scheduler->EndRendering();
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_copy_barrier,
    });

    cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             buffer_copies);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_copy_barrier,
    });

    for (auto& copy : buffer_copies) {
        copy.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    }

    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             buffer_copies);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::CopyMip(Image& src_image, u32 mip, u32 slice) {
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::ImageCopy);
    const auto& src_info = src_image.info;

    const auto src_block_dim = src_info.BlockDim();
    const auto dst_block_dim = info.BlockDim();
    const auto mip_block_w = std::max(dst_block_dim.width >> mip, 1u);
    const auto mip_block_h = std::max(dst_block_dim.height >> mip, 1u);
    ASSERT(mip_block_w == src_block_dim.width);
    ASSERT(mip_block_h == src_block_dim.height);

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, src_info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src_info.size.width, src_info.size.height, src_info.size.depth},
    };

    SetBackingSamples(info.num_samples);
    src_image.SetBackingSamples(src_info.num_samples);

    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                     backing->state.layout, image_copy);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                    const VideoCore::SubresourceRange& mrt1_range) {
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::ImageCopy);
    SetBackingSamples(1, false);
    scheduler->EndRendering();

    // Where the host holds what the guest multisamples with one sample a pixel, there is
    // nothing to resolve, and a copy would leave every edge as jagged as it was drawn: the
    // picture gets its edges smoothed on the way instead. (Under the A/B measuring aid as
    // "resolveaa", copied as it is.)
    if (blit_helper->SmoothsResolves() && !Vulkan::FrameStats::AbOff("resolveaa") &&
        src_image.info.num_samples > 1 &&
        src_image.backing->image.image_ci.samples == vk::SampleCountFlagBits::e1 &&
        src_image.info.size == info.size && !info.props.is_depth &&
        instance->IsFormatSupported(src_image.info.pixel_format,
                                    vk::FormatFeatureFlagBits2::eSampledImageFilterLinear) &&
        (src_image.usage_flags & vk::ImageUsageFlagBits::eSampled) &&
        (usage_flags & vk::ImageUsageFlagBits::eColorAttachment)) {
        src_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                          vk::AccessFlagBits2::eShaderRead, mrt0_range);
        Transit(vk::ImageLayout::eColorAttachmentOptimal,
                vk::AccessFlagBits2::eColorAttachmentWrite, mrt1_range);
        // With foveated rendering an eye's scene is shaded in blocks of pixels towards its
        // edges: smoothing its edges there is time the GPU does not have (a twentieth of a
        // frame's in the first level). (Under the A/B measuring aid as "fxaafine", smoothed
        // all over.)
        const bool fine_only = Vulkan::Foveation::Level() > 0 &&
                               instance->IsFoveationSupported() &&
                               Core::KnownTitle::IsSceneSize(info.size.width, info.size.height) &&
                               !Vulkan::FrameStats::AbOff("fxaafine");
        blit_helper->SmoothInto(info.size.width, info.size.height, src_image.info.pixel_format,
                                info.pixel_format, src_image.GetImage(), mrt0_range.base.layer,
                                GetImage(), mrt1_range.base.layer, fine_only);
        flags |= VideoCore::ImageFlagBits::GpuModified;
        flags &= ~VideoCore::ImageFlagBits::Dirty;
        return;
    }

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      mrt0_range);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, mrt1_range);

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_image.info, info, 1);
    // A guest multisampled image is single sampled where the host cannot multisample it.
    if (src_image.backing->image.image_ci.samples == vk::SampleCountFlagBits::e1) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->CommandBuffer().copyImage(src_image.GetImage(),
                                             vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                             vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {info.size.width, info.size.height, 1},
        };
        scheduler->CommandBuffer().resolveImage(src_image.GetImage(),
                                                vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                                vk::ImageLayout::eTransferDstOptimal, region);
    }

    flags |= VideoCore::ImageFlagBits::GpuModified;
    flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Image::Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range) {
    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::Clear);
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.clearColorImage(GetImage(), vk::ImageLayout::eTransferDstOptimal, clear_value.color,
                           vk_range);
}

void Image::SetHostSamples(vk::ImageCreateInfo& image_ci, u32 num_samples) const {
    image_ci.usage = usage_flags;
    image_ci.samples = LiverpoolToVK::NumSamples(num_samples, supported_samples);
    if (num_samples == 1 || image_ci.samples != vk::SampleCountFlagBits::e1) {
        return;
    }
    // Drivers without multisampled storage images, which is every mobile one, report that an
    // image which may be bound as a storage image cannot be multisampled at all. The depth
    // buffer and the pipeline it is used with are multisampled though, and a single sampled
    // color target among them garbles the picture. A multisampled image could not be bound as
    // a storage image on such a driver anyway, so that usage is what gets dropped.
    const auto samples = LiverpoolToVK::NumSamples(num_samples, attachment_samples);
    if (samples != vk::SampleCountFlagBits::e1) {
        image_ci.samples = samples;
        image_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
}

void Image::SetBackingSamples(u32 num_samples, bool copy_backing) {
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!info.props.is_depth, "Swapping samples is only valid for color images");
    auto new_image_ci = backing->image.image_ci;
    SetHostSamples(new_image_ci, num_samples);
    if (new_image_ci.samples == backing->image.image_ci.samples) {
        // A host that stops at fewer samples than the guest uses ends up with the same image for
        // several guest sample counts. That one image serves them all: there is nothing to
        // create and, above all, nothing to copy back and forth every time the guest goes from
        // drawing to the image to reading it.
        backing->num_samples = num_samples;
        return;
    }

    BackingImage* new_backing;
    auto it = std::ranges::find_if(backing_images, [&](const BackingImage& candidate) {
        return !candidate.retired && candidate.image.image_ci.samples == new_image_ci.samples;
    });
    if (it == backing_images.end()) {
        // What the current image allows its views goes for this one too.
        const auto view_formats = backing->image.view_formats;
        new_backing = &backing_images.emplace_back();
        new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
        new_backing->image.Create(new_image_ci, view_formats);

        Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }
    new_backing->num_samples = num_samples;

    if (copy_backing) {
        scheduler->EndRendering();
        ASSERT(info.resources.levels == 1 && info.resources.layers == 1);

        // Transition current backing to shader read layout
        auto barriers =
            GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, std::nullopt);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
        });
        const auto cmdbuf = scheduler->CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });

        // Copy between ms and non ms backing images. What counts is what the host images are:
        // both are single sampled where the host cannot multisample this format.
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->image.image_ci.samples,
            info.pixel_format, backing->image.image_ci.samples != vk::SampleCountFlagBits::e1,
            backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

} // namespace VideoCore
