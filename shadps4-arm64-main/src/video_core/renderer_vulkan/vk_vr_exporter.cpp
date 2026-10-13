// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/vr/vr_host_link.h"
#ifdef ENABLE_OPENXR_HOST
#include "core/vr/openxr_host.h"
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_vr_exporter.h"

#include <vk_mem_alloc.h>

#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Vulkan {

struct VrExporter::Slot {
    Frame frame{};
    u32 index{};
    int fd{-1};

    // Zero copy: the image is the host's buffer.
    vk::DeviceMemory imported_memory{};

    // Otherwise the image is ours, and its pixels travel through a buffer the CPU can read into
    // a mapping of the host's.
    VmaAllocation image_allocation{};
    vk::Buffer readback{};
    VmaAllocation readback_allocation{};
    const u8* readback_data{};
    u8* host_mapping{};
    size_t host_mapping_size{};
};

VrExporter::VrExporter(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

VrExporter::~VrExporter() {
    for (auto& slot : slots) {
        Destroy(*slot);
    }
}

void VrExporter::Destroy(Slot& slot) {
    const vk::Device device = instance.GetDevice();
    if (slot.frame.image_view) {
        device.destroyImageView(slot.frame.image_view);
    }
    if (slot.imported_memory) {
        device.destroyImage(slot.frame.image);
        device.freeMemory(slot.imported_memory);
    } else if (slot.frame.image) {
        vmaDestroyImage(instance.GetAllocator(), slot.frame.image, slot.image_allocation);
    }
    if (slot.readback) {
        vmaDestroyBuffer(instance.GetAllocator(), slot.readback, slot.readback_allocation);
    }
#ifdef __linux__
    if (slot.host_mapping != nullptr) {
        ::munmap(slot.host_mapping, slot.host_mapping_size);
    }
    if (slot.fd >= 0) {
        ::close(slot.fd);
    }
#endif
    slot = {};
}

bool VrExporter::HasLocalHost() const {
#ifdef ENABLE_OPENXR_HOST
    return Core::Vr::OpenXrHost::Instance().IsAvailable();
#else
    return false;
#endif
}

Frame* VrExporter::AcquireLocal(u32 width, u32 height) {
#ifdef ENABLE_OPENXR_HOST
    const auto target = Core::Vr::OpenXrHost::Instance().BeginFrame(width, height);
    if (!target) {
        return nullptr;
    }
    while (local_frames.size() <= target->index) {
        local_frames.push_back(std::make_unique<Frame>());
    }
    // The image is the host's, and may be another one than the last time.
    Frame& frame = *local_frames[target->index];
    frame.image = target->image;
    frame.image_view = target->view;
    frame.width = target->width;
    frame.height = target->height;
    frame.id = static_cast<u8>(target->index);
    frame.host_buffer = static_cast<s8>(target->index);
    return &frame;
#else
    return nullptr;
#endif
}

bool VrExporter::IsLocal(const Frame* frame) const {
    return std::ranges::any_of(local_frames,
                               [&](const auto& local) { return local.get() == frame; });
}

Frame* VrExporter::Acquire(vk::Format format) {
    if (failed || !Core::Vr::HostLink::Instance().IsConnected()) {
        return nullptr;
    }
    if (slots.empty() && !Setup(format)) {
        return nullptr;
    }
    Slot& slot = *slots[next_slot];
    next_slot = (next_slot + 1) % static_cast<u32>(slots.size());
    return &slot.frame;
}

bool VrExporter::Setup(vk::Format format) {
    // The first frames come up while the host is still starting; give it a moment, then show
    // them on the window until it is ready.
    const auto offer = Core::Vr::HostLink::Instance().TakeBuffers(std::chrono::milliseconds{250});
    if (!offer) {
        return false;
    }

    stride = offer->stride;
    swap_red_blue = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;

    bool zero_copy = true;
    for (u32 i = 0; i < offer->count; ++i) {
        auto slot = std::make_unique<Slot>();
        slot->index = i;
        slot->fd = offer->fds[i];
        slot->frame.id = static_cast<u8>(i);
        slot->frame.host_buffer = static_cast<s8>(i);
        slot->frame.width = offer->width;
        slot->frame.height = offer->height;
        // All or nothing, so the host never sees a mix of the two paths.
        zero_copy = zero_copy && Import(*slot, *offer, format);
        if (!zero_copy && !CreateCopy(*slot, *offer, format)) {
            LOG_ERROR(Render_Vulkan, "Unable to use the VR host's buffer {}", i);
            Destroy(*slot);
            for (u32 rest = i + 1; rest < offer->count; ++rest) {
#ifdef __linux__
                ::close(offer->fds[rest]);
#endif
            }
            for (auto& created : slots) {
                Destroy(*created);
            }
            slots.clear();
            failed = true;
            return false;
        }
        slots.push_back(std::move(slot));
    }

    LOG_INFO(Render_Vulkan, "Delivering headset frames to the VR host: {} buffers of {}x{}, {}",
             slots.size(), offer->width, offer->height,
             zero_copy ? "rendered in place" : "copied through the CPU");
    return true;
}

bool VrExporter::Import(Slot& slot, const Core::Vr::HostBuffers& offer, vk::Format format) {
#ifdef __linux__
    if (!instance.IsDmaBufImportSupported()) {
        return false;
    }
    const vk::Device device = instance.GetDevice();

    const vk::StructureChain image_ci = {
        vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {offer.width, offer.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eLinear,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        },
        vk::ExternalMemoryImageCreateInfo{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        },
    };
    const auto [image_result, image] = device.createImage(image_ci.get());
    if (image_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "VR buffer import: no linear {} render target ({})",
                    vk::to_string(format), vk::to_string(image_result));
        return false;
    }

    // The driver has to lay the rows out exactly like the host's buffer does.
    const auto layout = device.getImageSubresourceLayout(
        image, vk::ImageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor});
    const auto requirements = device.getImageMemoryRequirements(image);
    const off_t buffer_size = ::lseek(slot.fd, 0, SEEK_END);
    if (layout.offset != 0 || layout.rowPitch != u64{offer.stride} * 4 ||
        buffer_size < static_cast<off_t>(requirements.size)) {
        LOG_WARNING(Render_Vulkan,
                    "VR buffer import: layout mismatch (offset {}, pitch {} vs {}, size {} vs {})",
                    layout.offset, layout.rowPitch, offer.stride * 4, requirements.size,
                    static_cast<s64>(buffer_size));
        device.destroyImage(image);
        return false;
    }

    vk::MemoryFdPropertiesKHR fd_properties{};
    const auto properties_result = device.getMemoryFdPropertiesKHR(
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, slot.fd, &fd_properties);
    const u32 usable_types = properties_result == vk::Result::eSuccess
                                 ? fd_properties.memoryTypeBits & requirements.memoryTypeBits
                                 : 0;
    if (usable_types == 0) {
        LOG_WARNING(Render_Vulkan, "VR buffer import: no memory type accepts the buffer ({})",
                    vk::to_string(properties_result));
        device.destroyImage(image);
        return false;
    }

    // A successful import takes the descriptor over, so it gets one of its own.
    const int import_fd = ::dup(slot.fd);
    const vk::StructureChain allocate_info = {
        vk::MemoryAllocateInfo{
            .allocationSize = requirements.size,
            .memoryTypeIndex = static_cast<u32>(std::countr_zero(usable_types)),
        },
        vk::ImportMemoryFdInfoKHR{
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
            .fd = import_fd,
        },
        vk::MemoryDedicatedAllocateInfo{
            .image = image,
        },
    };
    const auto [memory_result, memory] = device.allocateMemory(allocate_info.get());
    if (memory_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "VR buffer import: the driver refused the buffer ({})",
                    vk::to_string(memory_result));
        ::close(import_fd);
        device.destroyImage(image);
        return false;
    }
    if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
        device.destroyImage(image);
        device.freeMemory(memory);
        return false;
    }

    const vk::ImageViewCreateInfo view_ci = {
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    const auto [view_result, view] = device.createImageView(view_ci);
    if (view_result != vk::Result::eSuccess) {
        device.destroyImage(image);
        device.freeMemory(memory);
        return false;
    }

    slot.frame.image = image;
    slot.frame.image_view = view;
    slot.imported_memory = memory;
    return true;
#else
    return false;
#endif
}

bool VrExporter::CreateCopy(Slot& slot, const Core::Vr::HostBuffers& offer, vk::Format format) {
    const vk::Device device = instance.GetDevice();

#ifdef __linux__
    slot.host_mapping_size = size_t{offer.stride} * 4 * offer.height;
    void* mapping = ::mmap(nullptr, slot.host_mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           slot.fd, 0);
    if (mapping == MAP_FAILED) {
        LOG_ERROR(Render_Vulkan, "Unable to map the VR host's buffer: {}", std::strerror(errno));
        slot.host_mapping_size = 0;
        return false;
    }
    slot.host_mapping = static_cast<u8*>(mapping);
#else
    return false;
#endif

    const VkImageCreateInfo image_ci = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {offer.width, offer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
    });
    const VmaAllocationCreateInfo image_alloc_ci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(), &image_ci, &image_alloc_ci, &image,
                       &slot.image_allocation, nullptr) != VK_SUCCESS) {
        return false;
    }
    slot.frame.image = vk::Image{image};

    const vk::ImageViewCreateInfo view_ci = {
        .image = slot.frame.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    const auto [view_result, view] = device.createImageView(view_ci);
    if (view_result != vk::Result::eSuccess) {
        return false;
    }
    slot.frame.image_view = view;

    const VkBufferCreateInfo buffer_ci = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = u64{offer.width} * 4 * offer.height,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    });
    // The pixels are read back every frame, which wants memory the CPU caches.
    const VmaAllocationCreateInfo buffer_alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocationInfo buffer_info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &buffer_alloc_ci, &buffer,
                        &slot.readback_allocation, &buffer_info) != VK_SUCCESS) {
        return false;
    }
    slot.readback = vk::Buffer{buffer};
    slot.readback_data = static_cast<const u8*>(buffer_info.pMappedData);
    return slot.readback_data != nullptr;
}

void VrExporter::Finalize(Frame* frame, vk::CommandBuffer cmdbuf) {
    if (IsLocal(frame)) {
        // Drawn into the host's own image, which it reads on the same device.
        return;
    }
    Slot& slot = *slots[frame->host_buffer];
    if (!slot.readback) {
        // Rendered in place: once the commands have run, the host's buffer holds the frame.
        return;
    }

    // The post-processing pass leaves the frame in the general layout, after a barrier of its
    // own that waits for its colour writes.
    const vk::ImageMemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = frame->image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
    const vk::BufferImageCopy region = {
        .imageSubresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .layerCount = 1,
        },
        .imageExtent = {frame->width, frame->height, 1},
    };
    cmdbuf.copyImageToBuffer(frame->image, vk::ImageLayout::eTransferSrcOptimal, slot.readback,
                             region);
}

void VrExporter::Deliver(Frame* frame, const Core::Vr::PresentedFrame& info) {
    if (IsLocal(frame)) {
#ifdef ENABLE_OPENXR_HOST
        // The host copies the image as soon as it hears about the frame.
        scheduler.GetMasterSemaphore()->Wait(frame->ready_tick);
        Core::Vr::OpenXrHost::Instance().EndFrame(static_cast<u32>(frame->host_buffer), info);
#endif
        return;
    }
    Slot& slot = *slots[frame->host_buffer];

    // The host samples the buffer as soon as it hears about the frame.
    scheduler.GetMasterSemaphore()->Wait(frame->ready_tick);

#ifdef __linux__
    if (slot.readback) {
        dma_buf_sync sync{.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
        ::ioctl(slot.fd, DMA_BUF_IOCTL_SYNC, &sync);
        const size_t row_bytes = size_t{frame->width} * 4;
        const size_t host_row_bytes = size_t{stride} * 4;
        if (row_bytes == host_row_bytes) {
            std::memcpy(slot.host_mapping, slot.readback_data, row_bytes * frame->height);
        } else {
            for (u32 y = 0; y < frame->height; ++y) {
                std::memcpy(slot.host_mapping + y * host_row_bytes,
                            slot.readback_data + y * row_bytes, row_bytes);
            }
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        ::ioctl(slot.fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
#endif

    Core::Vr::Protocol::Frame message;
    message.buffer = slot.index;
    message.frame_id = info.id;
    message.eye_width = info.eye_width;
    message.eye_height = info.eye_height;
    message.position[0] = info.render_pose.position.x;
    message.position[1] = info.render_pose.position.y;
    message.position[2] = info.render_pose.position.z;
    message.orientation[0] = info.render_pose.orientation.x;
    message.orientation[1] = info.render_pose.orientation.y;
    message.orientation[2] = info.render_pose.orientation.z;
    message.orientation[3] = info.render_pose.orientation.w;
    message.fov[0] = info.fov.tan_out;
    message.fov[1] = info.fov.tan_in;
    message.fov[2] = info.fov.tan_top;
    message.fov[3] = info.fov.tan_bottom;
    message.swap_red_blue = swap_red_blue;
    Core::Vr::HostLink::Instance().SendFrame(message);
}

} // namespace Vulkan
