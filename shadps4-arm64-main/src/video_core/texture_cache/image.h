// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <span>

#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

#include <deque>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
    Picked = 1 << 7,      ///< Temporary flag to mark the image as picked
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)},
          view_formats{std::move(other.view_formats)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        view_formats = std::move(other.view_formats);
        return *this;
    }

    /// `view_formats` names the formats views of the image will have. Without it a mutable
    /// image may be viewed in any compatible format, and a driver has to store it in a way
    /// that allows that: on Adreno that means neither compressed nor tiled, which costs
    /// memory bandwidth every time the image is drawn to or sampled.
    void Create(const vk::ImageCreateInfo& image_ci, std::span<const vk::Format> view_formats = {});

    /// Whether a view of this image may have `format`.
    bool AllowsViewFormat(vk::Format format) const {
        return view_formats.empty() ||
               std::ranges::find(view_formats, format) != view_formats.end();
    }

    void Destroy();

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
    /// Empty when views may have any compatible format.
    boost::container::static_vector<vk::Format, 4> view_formats;
};

constexpr Common::SlotId NULL_IMAGE_ID{0};

class BlitHelper;

struct Image {
    Image(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, BlitHelper& blit_helper,
          Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    bool Overlaps(VAddr overlap_cpu_addr, size_t overlap_size) const noexcept {
        const VAddr overlap_end = overlap_cpu_addr + overlap_size;
        const auto image_addr = info.guest_address;
        const auto image_end = info.guest_address + info.guest_size;
        return image_addr < overlap_end && overlap_cpu_addr < image_end;
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    bool IsTracked() {
        return track_addr != 0 && track_addr_end != 0;
    }

    bool SafeToDownload() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    using Barriers = boost::container::small_vector<vk::ImageMemoryBarrier2, 32>;
    Barriers GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                         vk::PipelineStageFlags2 dst_stage,
                         std::optional<SubresourceRange> subres_range);
    void Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                 std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf = {});
    void Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer, u64 offset);
    void Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                  u64 offset, u64 download_size);

    void CopyImage(Image& src_image);
    void CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset);
    void CopyMip(Image& src_image, u32 mip, u32 slice);

    void Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                 const VideoCore::SubresourceRange& mrt1_range);
    void Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range);

    /// Makes sure a view of the image can have `format`. Images are created for the formats
    /// titles usually view them in; one that asks for another gets the image moved to one
    /// that allows any.
    void EnsureViewFormat(vk::Format format);

    void SetBackingSamples(u32 num_samples, bool copy_backing = true);

    /// Sets the sample count, and the usage that goes with it, of a backing image the guest sees
    /// as having `num_samples` samples.
    void SetHostSamples(vk::ImageCreateInfo& image_ci, u32 num_samples) const;

public:
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    BlitHelper* blit_helper;
    Common::SlotVector<ImageView>* slot_image_views;
    ImageInfo info;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;
    /// Sample counts available when the image gives up being usable as a storage image.
    vk::SampleCountFlags attachment_samples = vk::SampleCountFlagBits::e1;
    ImageFlagBits flags = ImageFlagBits::Dirty;
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    ImageId depth_id{};
    u64 depth_uid{};

    // Resource state tracking
    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        /// The view last asked for (FindView), most of the time the one asked for next.
        ImageViewInfo last_view_info{};
        ImageViewId last_view_id{};
        /// Requests for part of the image that found, or left, that part in the state asked
        /// for. They stay true until some state changes (`state_version` counts those): a
        /// draw asks for the same thing as the one before it far more often than not, and
        /// going through every layer and mip level of the request to find nothing to do is
        /// what made binding a texture expensive.
        struct SettledRequest {
            SubresourceRange range;
            vk::ImageLayout layout;
            vk::AccessFlags2 access_mask;
            u64 state_version;
        };
        std::array<SettledRequest, 4> settled{};
        u32 next_settled{};
        u64 state_version{1};
        u32 num_samples;
        /// Replaced by another image that holds the contents now; only kept because views of
        /// it may still be in use.
        bool retired{};
    };
    std::deque<BackingImage> backing_images;
    BackingImage* backing{};
    boost::container::static_vector<u64, 16> mip_hashes{};
    u64 image_uid{};
    u64 lru_id{};
    u64 tick_accessed_last{};
    u64 hash{};

    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};

    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore
