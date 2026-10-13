// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "core/vr/spectator_view.h"
#ifdef ENABLE_OPENXR_HOST
#include "core/vr/openxr_host.h"
#endif
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/stall_log.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <imgui.h>
#include <png.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, Scheduler& scheduler, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance,
                 scheduler,
                 VideoCore::MemoryUsage::Download,
                 0,
                 vk::BufferUsageFlagBits::eTransferDst,
                 static_cast<u64>(width_) * static_cast<u64>(height_) * 4},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr == nullptr) {
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return false;
    }

    png_init_io(png_ptr, file.file);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    thread_local std::vector<png_bytep> rows;
    rows.resize(height);
    for (u32 y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * width * 4);
    }

    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return true;
}

static void SavePendingScreenshots(const std::vector<ScreenshotReadback>& readbacks) {
    for (const auto& readback : readbacks) {
        if (readback.paths.empty()) {
            continue;
        }

        std::vector<u8> rgba;
        if (!ConvertReadbackToRgba8(readback, rgba)) {
            continue;
        }

        const auto& primary_path = readback.paths.front();
        if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
            LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
            continue;
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

        std::ifstream file(primary_path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);

        for (size_t i = 1; i < readback.paths.size(); ++i) {
            const auto& path = readback.paths[i];
            std::error_code ec{};
            std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
            if (ec) {
                // Fallback for platforms/filesystems where copy_file can fail for transient
                // reasons.
                if (!WritePng(path, rgba, readback.width, readback.height)) {
                    LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                    continue;
                }
            }

            LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
            std::ifstream file(path, std::ios::binary);
            std::vector<u8> imgdata;
            if (file) {
                imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>());
            }
            shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                                 shadNotifications::position::BottomRight, imgdata);
        }
    }
}

// SHADPS4_DBG_EYE_SCAN=<n>: every n-th frame for a headset, both eyes' pictures are read back,
// and with them the picture handed to the headset's app (both eyes side by side, after the eye
// copy), and looked over for black blocks in lit surroundings (issue #19: black rectangles on
// the Quest). A block is told of when it is black in one eye only, or when it was lit at the
// look before (a few frames earlier). Pictures of such moments go to <UserDir>/eye_scan/ (at most
// one set every 2 seconds, 80 in all), one set a minute to compare with; each find is logged
// with the seconds since the first look.
namespace EyeScan {

constexpr u32 Tile = 4;

struct Tiles {
    u32 columns{};
    u32 rows{};
    std::vector<u8> peak; ///< The brightest channel of any pixel of a 4x4 tile.
    std::vector<u8> mean; ///< The average over its pixels of each one's brightest channel.

    bool Black(u32 column, u32 row) const {
        return peak[row * columns + column] <= 6;
    }
    /// Inside the middle of the picture, away from the border the view may be shaped with.
    bool Middle(u32 column, u32 row) const {
        const float x = (column + 0.5f) / columns * 2.0f - 1.0f;
        const float y = (row + 0.5f) / rows * 2.0f - 1.0f;
        return x * x + y * y < 0.85f;
    }
};

struct Block {
    u32 x, y, width, height; ///< In tiles.
    u32 tiles;
    float lit_border;
    bool other_eye_too;
};

static Tiles Measure(std::span<const u8> rgba, u32 width, u32 height) {
    Tiles tiles{.columns = width / Tile, .rows = height / Tile};
    tiles.peak.assign(tiles.columns * tiles.rows, 0);
    tiles.mean.assign(tiles.columns * tiles.rows, 0);
    for (u32 row = 0; row < tiles.rows; ++row) {
        for (u32 column = 0; column < tiles.columns; ++column) {
            u32 peak = 0;
            u32 sum = 0;
            for (u32 y = 0; y < Tile; ++y) {
                const u8* p = rgba.data() + ((row * Tile + y) * width + column * Tile) * 4;
                for (u32 x = 0; x < Tile; ++x, p += 4) {
                    const u32 brightest = std::max({p[0], p[1], p[2]});
                    peak = std::max(peak, brightest);
                    sum += brightest;
                }
            }
            tiles.peak[row * tiles.columns + column] = static_cast<u8>(peak);
            tiles.mean[row * tiles.columns + column] = static_cast<u8>(sum / (Tile * Tile));
        }
    }
    return tiles;
}

/// Groups of touching black tiles in the middle of the picture that are mostly surrounded by
/// lit ones: not the night sky or a shadow, something cut out of a lit picture.
static std::vector<Block> FindBlocks(const Tiles& tiles) {
    std::vector<Block> blocks;
    std::vector<u8> seen(tiles.peak.size(), 0);
    std::vector<u32> stack;
    for (u32 start = 0; start < tiles.peak.size(); ++start) {
        const u32 start_column = start % tiles.columns;
        const u32 start_row = start / tiles.columns;
        if (seen[start] || !tiles.Black(start_column, start_row) ||
            !tiles.Middle(start_column, start_row)) {
            continue;
        }
        u32 left = start_column, right = start_column, top = start_row, bottom = start_row;
        u32 count = 0, border = 0, lit = 0;
        stack.assign(1, start);
        seen[start] = 1;
        while (!stack.empty()) {
            const u32 at = stack.back();
            stack.pop_back();
            const u32 column = at % tiles.columns;
            const u32 row = at / tiles.columns;
            ++count;
            left = std::min(left, column);
            right = std::max(right, column);
            top = std::min(top, row);
            bottom = std::max(bottom, row);
            const std::array<std::pair<s32, s32>, 4> steps{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (const auto& [dx, dy] : steps) {
                const s32 c = static_cast<s32>(column) + dx;
                const s32 r = static_cast<s32>(row) + dy;
                if (c < 0 || r < 0 || c >= static_cast<s32>(tiles.columns) ||
                    r >= static_cast<s32>(tiles.rows)) {
                    continue;
                }
                const u32 next = static_cast<u32>(r) * tiles.columns + static_cast<u32>(c);
                if (tiles.Black(static_cast<u32>(c), static_cast<u32>(r)) &&
                    tiles.Middle(static_cast<u32>(c), static_cast<u32>(r))) {
                    if (!seen[next]) {
                        seen[next] = 1;
                        stack.push_back(next);
                    }
                } else if (!tiles.Black(static_cast<u32>(c), static_cast<u32>(r))) {
                    ++border;
                    lit += tiles.mean[next] >= 8 ? 1 : 0;
                }
            }
        }
        const float lit_border = border > 0 ? static_cast<float>(lit) / border : 0.0f;
        if (count >= 4 && count <= 16000 && lit_border >= 0.6f) {
            blocks.push_back({left, top, right - left + 1, bottom - top + 1, count, lit_border,
                              false});
        }
    }
    return blocks;
}

/// Whether the other eye has about as much black in the same rows near the block (the two
/// pictures differ by the distance between the eyes, so not in the same place).
static bool OtherEyeToo(const Block& block, const Tiles& other) {
    constexpr s32 Reach = 80; // tiles to either side: 320 pixels
    const s32 from = std::max(0, static_cast<s32>(block.x) - Reach);
    const s32 to = std::min(static_cast<s32>(other.columns) - 1,
                            static_cast<s32>(block.x + block.width) + Reach);
    u32 black = 0;
    for (u32 row = block.y; row < block.y + block.height && row < other.rows; ++row) {
        for (s32 column = from; column <= to; ++column) {
            black += other.Black(static_cast<u32>(column), row) ? 1 : 0;
        }
    }
    return black * 2 >= block.tiles;
}

static std::atomic_bool busy{false};

/// Whether most of a block's tiles were lit at the look before.
static bool WasLit(const Block& block, const Tiles& tiles, const Tiles& before) {
    if (before.columns != tiles.columns || before.rows != tiles.rows) {
        return false;
    }
    u32 lit = 0;
    for (u32 row = block.y; row < block.y + block.height; ++row) {
        for (u32 column = block.x; column < block.x + block.width; ++column) {
            if (tiles.Black(column, row)) {
                lit += before.mean[row * tiles.columns + column] >= 24 ? 1 : 0;
            }
        }
    }
    return lit * 10 >= block.tiles * 7;
}

/// One picture's two eyes: the blocks worth telling of, as a log text; how many there were.
static u32 LookAtPair(std::string_view what, const std::array<Tiles, 2>& tiles,
                      std::array<Tiles, 2>& before, std::string& found) {
    u32 told = 0;
    for (u32 eye = 0; eye < 2; ++eye) {
        for (auto& block : FindBlocks(tiles[eye])) {
            block.other_eye_too = OtherEyeToo(block, tiles[eye ^ 1]);
            const bool appeared = WasLit(block, tiles[eye], before[eye]);
            if (block.other_eye_too && !appeared) {
                continue;
            }
            ++told;
            found += fmt::format(
                " {}, {} eye: {}x{} px at {},{} ({} tiles, border {:.0f}% lit; {}{});", what,
                eye == 0 ? "left" : "right", block.width * Tile, block.height * Tile,
                block.x * Tile, block.y * Tile, block.tiles, block.lit_border * 100.0f,
                block.other_eye_too ? "black in the other eye too" : "this eye only",
                appeared ? ", lit at the look before" : "");
        }
    }
    before = tiles;
    return told;
}

/// `pictures`: the left eye, the right eye and, where there is one, the picture handed to the
/// headset's app with both eyes side by side.
static void Look(const std::vector<ScreenshotReadback>& pictures, u32 frame_id) {
    using Clock = std::chrono::steady_clock;
    static const auto first = Clock::now();
    static u32 saved = 0;
    static auto last_save = Clock::time_point{};
    static auto last_reference = Clock::time_point{};
    static u64 scans = 0;
    static u64 finds = 0;
    static std::array<Tiles, 2> eyes_before, copy_before;
    const auto now = Clock::now();
    std::array<std::vector<u8>, 2> eyes;
    std::array<Tiles, 2> eye_tiles;
    for (u32 eye = 0; eye < 2; ++eye) {
        if (!ConvertReadbackToRgba8(pictures[eye], eyes[eye])) {
            return;
        }
        eye_tiles[eye] = Measure(eyes[eye], pictures[eye].width, pictures[eye].height);
    }
    std::vector<u8> copy;
    std::array<std::vector<u8>, 2> halves;
    std::array<Tiles, 2> copy_tiles;
    const bool with_copy = pictures.size() > 2 && ConvertReadbackToRgba8(pictures[2], copy);
    u32 half_width = 0;
    if (with_copy) {
        half_width = pictures[2].width / 2;
        const u32 height = pictures[2].height;
        for (u32 eye = 0; eye < 2; ++eye) {
            halves[eye].resize(size_t{half_width} * height * 4);
            for (u32 y = 0; y < height; ++y) {
                std::memcpy(halves[eye].data() + size_t{y} * half_width * 4,
                            copy.data() + (size_t{y} * pictures[2].width + eye * half_width) * 4,
                            size_t{half_width} * 4);
            }
            copy_tiles[eye] = Measure(halves[eye], half_width, height);
        }
    }
    ++scans;
    std::string found;
    u32 told = LookAtPair("the game's picture", eye_tiles, eyes_before, found);
    if (with_copy) {
        told += LookAtPair("as handed to the headset", copy_tiles, copy_before, found);
    }
    finds += told;
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "eye_scan";
    const auto save = [&](std::string_view kind) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (u32 eye = 0; eye < 2; ++eye) {
            WritePng(dir / fmt::format("{:06}-{}-{}.png", frame_id, kind, eye == 0 ? "L" : "R"),
                     eyes[eye], pictures[eye].width, pictures[eye].height);
        }
        if (with_copy) {
            WritePng(dir / fmt::format("{:06}-{}-headset.png", frame_id, kind), copy,
                     pictures[2].width, pictures[2].height);
        }
    };
    const double seconds = std::chrono::duration<double>(now - first).count();
    if (told > 0) {
        LOG_INFO(Render_Vulkan, "Eye scan at {:.1f} s, frame {}:{}", seconds, frame_id, found);
        if (saved < 80 && now - last_save >= std::chrono::seconds{2}) {
            ++saved;
            last_save = now;
            save("found");
        }
    } else if (now - last_reference >= std::chrono::seconds{60}) {
        last_reference = now;
        save("plain");
    }
    if (scans % 300 == 0) {
        LOG_INFO(Render_Vulkan,
                 "Eye scan at {:.1f} s: {} frames looked at{}, {} blocks told of ({} sets of "
                 "pictures kept)",
                 seconds, scans, with_copy ? " (with the picture handed to the headset)" : "",
                 finds, saved);
    }
}

} // namespace EyeScan

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(instance, draw_scheduler.GetMasterSemaphore(), device,
                    instance.GetAllocator(), num_images);
    pp_pass.Create(instance, draw_scheduler.GetMasterSemaphore(),
                   swapchain.GetSurfaceFormat().format);
    vr_exporter = std::make_unique<VrExporter>(instance, draw_scheduler);
#ifdef ENABLE_OPENXR_HOST
    if (vr_exporter->HasLocalHost()) {
        // The machine has a headset of its own: its session is made on this device.
        hmd_pp_pass.Create(instance, draw_scheduler.GetMasterSemaphore(),
                           Core::Vr::OpenXrHost::FrameFormat);
        Core::Vr::OpenXrHost::Instance().Start({
            .instance = instance.GetInstance(),
            .physical_device = instance.GetPhysicalDevice(),
            .device = instance.GetDevice(),
            .queue = instance.GetGraphicsQueue(),
            .queue_family = instance.GetGraphicsQueueFamilyIndex(),
            .queue_index = 0,
        });
    }
#endif

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
}

Presenter::~Presenter() {
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    // Naming the format the views will have lets the driver keep the image compressed.
    const vk::ImageFormatListCreateInfo format_list = {
        .viewFormatCount = 1,
        .pViewFormats = &format,
    };
    const vk::ImageCreateInfo image_info = {
        .pNext = &format_list,
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    // Once presented, a frame goes back to the pool and from there to the renderer, for the next
    // picture. It can only be shown again while it is still in the pool, and it is taken out of
    // it for as long as that takes: a renderer that starts on the frame half way through
    // replaces the tick its presentation waits for, and a presentation that waits for a tick
    // nobody will ever submit blocks the queue, and with it the emulator, for good.
    Frame* frame = nullptr;
    {
        std::scoped_lock lock{free_mutex};
        const size_t count = last_submit_frame != nullptr ? free_queue.size() : 0;
        for (size_t i = 0; i < count; ++i) {
            Frame* const candidate = free_queue.front();
            free_queue.pop();
            if (candidate == last_submit_frame && frame == nullptr) {
                frame = candidate;
            } else {
                free_queue.push(candidate);
            }
        }
    }
    if (frame == nullptr) {
        return nullptr;
    }

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Soft-fail: hard ASSERT became exit 133 on Mali after first game frame.
            // Returning the frame lets Present discover device-lost on acquire/present.
            LOG_CRITICAL(Render_Vulkan, "Device lost during waiting for a frame (PrepareLastFrame)");
            break;
        }
        LOG_ERROR(Render_Vulkan, "Unexpected waitForFences result in PrepareLastFrame: {}",
                  vk::to_string(result));
        break;
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_timeline = scheduler.GetMasterSemaphore();
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
#ifdef ENABLE_BACHATA_RUNTIME
    // The embedded X11 server's DRI3 path exposes the guest frame with the opposite R/B
    // memory order from desktop WSI. Compensate in the sampled image view.
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    default:
        break;
    }
#endif
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();
    if (!frame) {
        return nullptr;
    }

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_game_only_count > 0) {
        pending_screenshots.reserve(1);
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        pending_screenshots.emplace_back(
            instance, draw_scheduler, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height, view_info.format, hdr_encoded);
        auto& readback = pending_screenshots.back();

        // Capture the guest output before any host-side scaling (FSR/PP) is applied.
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                      cmdbuf);
        CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            readback);
    }

    // Continue with host-side passes that draw the displayed (scaled) frame.
    image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
                  cmdbuf);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    // Flush frame creation commands.
    frame->ready_timeline = draw_scheduler.GetMasterSemaphore();
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    return frame;
}

HmdFrames Presenter::PrepareHmdFrame(std::span<const AmdGpu::Image, 2> eye_textures,
                                    const Core::Vr::Fov& fov, u32 frame_id,
                                    u32& eye_width, u32& eye_height) {
    // The guest hands the eyes over as plain textures; they were rendered as color targets, so
    // the cache already holds their contents.
    std::array<VideoCore::TextureCache::ImageDesc, 2> descs;
    std::array<VideoCore::ImageId, 2> image_ids;
    for (u32 eye = 0; eye < 2; ++eye) {
        descs[eye] = VideoCore::TextureCache::ImageDesc{eye_textures[eye], Shader::ImageResource{}};
        image_ids[eye] = texture_cache.FindImage(descs[eye]);
        texture_cache.UpdateImage(image_ids[eye]);
    }

    // Diagnostics: creating <UserDir>/dump_render_targets dumps what the guest has rendered so
    // far, once, to tell a guest that draws nothing apart from output that gets lost on the way.
    {
        std::error_code ec;
        const auto dump_flag =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "dump_render_targets";
        static u32 frames_until_check = 0;
        if (frames_until_check-- == 0) {
            frames_until_check = 120;
            if (std::filesystem::exists(dump_flag, ec)) {
                std::filesystem::remove(dump_flag, ec);
                DumpGpuImages();
                Rasterizer::StartDrawTrace(3000);
            }
        }
    }

    // SHADPS4_DBG_TRACE_HMD_FRAMES=<frame>,<frame>...: after each of those frames, the draws of
    // the next ones are traced (2500 entries).
    {
        static const std::vector<u32> trace_frames = [] {
            std::vector<u32> frames;
            if (const char* value = std::getenv("SHADPS4_DBG_TRACE_HMD_FRAMES")) {
                for (const char* at = value; *at != '\0';) {
                    char* end = nullptr;
                    frames.push_back(static_cast<u32>(std::strtoul(at, &end, 10)));
                    if (end == at) {
                        break;
                    }
                    at = *end == ',' ? end + 1 : end;
                }
            }
            return frames;
        }();
        // SHADPS4_DBG_DUMP_HMD_FRAMES=<frame>,...: what the guest has rendered when that frame is
        // handed over, to <UserDir>/rt_dump-<frame>.
        static const std::vector<u32> dump_frames = [] {
            std::vector<u32> frames;
            if (const char* value = std::getenv("SHADPS4_DBG_DUMP_HMD_FRAMES")) {
                for (const char* at = value; *at != '\0';) {
                    char* end = nullptr;
                    frames.push_back(static_cast<u32>(std::strtoul(at, &end, 10)));
                    if (end == at) {
                        break;
                    }
                    at = *end == ',' ? end + 1 : end;
                }
            }
            return frames;
        }();
        if (std::ranges::find(dump_frames, frame_id) != dump_frames.end()) {
            DumpGpuImages(fmt::format("rt_dump-{}", frame_id));
        }
        static u32 marked = 0;
        if (std::ranges::find(trace_frames, frame_id) != trace_frames.end()) {
            Rasterizer::StartDrawTrace(2500);
            marked = 12;
        }
        if (marked > 0 || Rasterizer::DrawTraceActive()) {
            marked -= marked > 0 ? 1 : 0;
            LOG_INFO(Render_Vulkan, "TRACE HMD frame {} handed over", frame_id);
        }
    }
    static u32 logged_frames = 0;
    if (logged_frames < 3 || (logged_frames % 600) == 0) {
        for (u32 eye = 0; eye < 2; ++eye) {
            const auto& image = texture_cache.GetImage(image_ids[eye]);
            LOG_INFO(Render_Vulkan,
                     "HMD frame {} eye {}: image {} at {:#x}, {}x{}, format {}, samples {}, "
                     "flags {:#x}",
                     frame_id, eye, image_ids[eye].index, image.info.guest_address,
                     image.info.size.width, image.info.size.height,
                     vk::to_string(image.info.pixel_format), image.info.num_samples,
                     static_cast<u32>(image.flags));
        }
    }
    ++logged_frames;

    const auto& left_info = texture_cache.GetImage(image_ids[0]).info;
    eye_width = left_info.size.width;
    eye_height = left_info.size.height;

    // With a VR host attached the frame goes into one of its buffers instead of the window. A
    // headset of the machine's own gets a frame of its own next to the window's: the eyes'
    // pictures at their full size, where the window only has a look at them.
    Frame* frame = vr_exporter->Acquire(swapchain.GetSurfaceFormat().format);
    const bool exported = frame != nullptr;
    static const auto desktop_view = [] {
        const char* value = std::getenv("SHADPS4_VR_DESKTOP_VIEW");
        return Core::Vr::ParseDesktopView(value != nullptr ? value : "");
    }();
    const bool spectator = desktop_view != Core::Vr::DesktopView::Stereo && !exported;
    static const bool desktop_crop = [] {
        const char* value = std::getenv("SHADPS4_VR_DESKTOP_CROP");
        return value != nullptr && std::string_view{value} == "1";
    }();
    const bool crop = spectator && desktop_crop;
    const float desktop_aspect = Core::Vr::DesktopViewAspect(
        desktop_view, fov, static_cast<float>(eye_width) / eye_height);
    Frame* const local = exported ? nullptr : vr_exporter->AcquireLocal(eye_width * 2, eye_height);
    if (!exported) {
        expected_ratio = crop ? std::nullopt : std::optional{desktop_aspect};
        frame = GetRenderFrame();
        if (!frame && !local) {
            return {};
        }
    }

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    boost::container::static_vector<vk::ImageMemoryBarrier2, 2> pre_barriers;
    for (const Frame* target : {frame, local}) {
        if (target == nullptr) {
            continue;
        }
        pre_barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = target->image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pImageMemoryBarriers = pre_barriers.data(),
    });

    // Pictures the title keeps sRGB-encoded are read as the bytes they hold where what is made
    // of them is encoded the same way (gamma 1) and they are copied pixel for pixel: the
    // hardware then does not decode every tap the sharpening takes, only for the shader to
    // encode it again. (Where a picture is drawn larger or smaller, as into a window, its
    // pixels are blended, and blended encoded they would come out darker in fine detail.) Both
    // eyes or neither: they share the pass's settings. (Not where an image was made without
    // that view's format, to be moved to one that allows it; nor under the A/B measuring aid
    // as "ppcopy".)
    const auto encoded_sibling = [](vk::Format format) {
        switch (format) {
        case vk::Format::eR8G8B8A8Srgb:
            return vk::Format::eR8G8B8A8Unorm;
        case vk::Format::eB8G8R8A8Srgb:
            return vk::Format::eB8G8R8A8Unorm;
        default:
            return vk::Format::eUndefined;
        }
    };
    const auto pixel_for_pixel = [&](const Frame* target) {
        return target == nullptr ||
               (target->width == eye_width * 2 && target->height == eye_height);
    };
    const auto& right_info = texture_cache.GetImage(image_ids[1]).info;
    bool encoded_in = pp_settings.gamma == 1.0f && pp_settings.hdr == 0 && !spectator &&
                      pixel_for_pixel(frame) && pixel_for_pixel(local) &&
                      right_info.size.width == eye_width && right_info.size.height == eye_height &&
                      !FrameStats::AbOff("ppcopy");
    for (u32 eye = 0; eye < 2 && encoded_in; ++eye) {
        const vk::Format sibling = encoded_sibling(descs[eye].view_info.format);
        encoded_in = sibling != vk::Format::eUndefined &&
                     texture_cache.GetImage(image_ids[eye]).backing->image.AllowsViewFormat(
                         sibling);
    }
    static const u32 eye_scan_every = [] {
        const char* value = std::getenv("SHADPS4_DBG_EYE_SCAN");
        return value != nullptr ? static_cast<u32>(std::max(0, std::atoi(value))) : 0u;
    }();
    std::shared_ptr<std::vector<ScreenshotReadback>> eye_scan;
    if (eye_scan_every > 0 && frame_id % eye_scan_every == 0 && !EyeScan::busy.exchange(true)) {
        eye_scan = std::make_shared<std::vector<ScreenshotReadback>>();
        eye_scan->reserve(2);
    }
    std::array<vk::ImageView, 2> eye_views;
    for (u32 eye = 0; eye < 2; ++eye) {
        auto view_info = descs[eye].view_info;
        // Exclude alpha from output frame to avoid blending with UI.
        view_info.mapping.a = vk::ComponentSwizzle::eOne;
        if (encoded_in) {
            view_info.format = encoded_sibling(view_info.format);
        }

        // The view first: an image that does not allow the view's format moves to a backing
        // that does, and that is the one to get ready for reading.
        auto& image = texture_cache.GetImage(image_ids[eye]);
        eye_views[eye] = *image.FindView(view_info).image_view;
        if (eye_scan) {
            image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                          {}, cmdbuf);
            eye_scan->emplace_back(instance, draw_scheduler, ScreenshotKind::GameOnly,
                                   std::vector<std::filesystem::path>{}, image.info.size.width,
                                   image.info.size.height, image.info.pixel_format, false);
            CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                eye_scan->back());
        }
        image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                      {}, cmdbuf);
    }
    // Left eye on the left half, right eye on the right half.
    const auto regions_for = [&](const Frame& target) {
        const u32 half_width = target.width / 2;
        std::array<HostPasses::PostProcessingPass::Region, 2> regions;
        for (u32 eye = 0; eye < 2; ++eye) {
            regions[eye] = {
                .input = eye_views[eye],
                .area{
                    .offset{.x = static_cast<s32>(eye * half_width), .y = 0},
                    .extent{.width = half_width, .height = target.height},
                },
            };
        }
        return regions;
    };
    // The eyes' pictures are shown far larger than they are drawn (a headset's display has
    // several pixels for each of theirs), which blurs them: SHADPS4_VR_SHARPEN=<0..1> sharpens
    // them on the way by that much.
    static const float sharpen = [] {
        const char* value = std::getenv("SHADPS4_VR_SHARPEN");
        return value != nullptr ? std::clamp(static_cast<float>(std::atof(value)), 0.0f, 1.0f)
                                : 0.0f;
    }();
    auto hmd_settings = pp_settings;
    hmd_settings.sharpen = sharpen;
    hmd_settings.encoded_in = encoded_in ? 1 : 0;
    // Where FSR 1 enlarged the title's scene (TitleUpscale), the picture is crisp already:
    // sharpened by SHADPS4_VR_SHARPEN_FSR there (half of the rest by default).
    if (rasterizer->EyesUpscaled()) {
        static const float sharpen_fsr = [] {
            const char* value = std::getenv("SHADPS4_VR_SHARPEN_FSR");
            return value != nullptr
                       ? std::clamp(static_cast<float>(std::atof(value)), 0.0f, 1.0f)
                       : -1.0f;
        }();
        hmd_settings.sharpen_centre = sharpen_fsr >= 0.0f ? sharpen_fsr : sharpen * 0.5f;
        hmd_settings.centre_radius = rasterizer->UpscaledRadius();
    }
    if (frame != nullptr) {
        const auto bounds = Core::Vr::SpectatorContentRect(frame->width, frame->height,
                                                        desktop_aspect, crop);
        const vk::Rect2D content{{bounds.x, bounds.y}, {bounds.width, bounds.height}};
        const vk::Rect2D clip{{bounds.x, std::max(0, bounds.y)},
                             {bounds.width, std::min(bounds.height, frame->height)}};
        if (spectator && desktop_view == Core::Vr::DesktopView::Combined) {
            const auto layout = Core::Vr::CombinedEyeRegions(content.extent.width, fov);
            const auto region_for = [&](u32 eye) {
                const auto& region = layout[eye];
                return HostPasses::PostProcessingPass::Region{
                    .input = eye_views[eye],
                    .area{
                        .offset{.x = content.offset.x + static_cast<s32>(region.x),
                                .y = content.offset.y},
                        .extent{.width = region.width, .height = content.extent.height},
                    },
                    .clip = vk::Rect2D{
                        .offset{.x = content.offset.x + static_cast<s32>(region.clip_x),
                                .y = clip.offset.y},
                        .extent{.width = region.clip_width, .height = clip.extent.height},
                    },
                };
            };
            const std::array regions{region_for(0), region_for(1)};
            const auto count = layout[1].clip_width == 0 ? 1u : 2u;
            pp_pass.Render(cmdbuf, std::span{regions}.first(count), *frame, hmd_settings);
        } else if (spectator) {
            const std::array regions{
                HostPasses::PostProcessingPass::Region{
                    .input = eye_views[0], .area = content, .clip = clip},
            };
            pp_pass.Render(cmdbuf, regions, *frame, hmd_settings);
        } else {
            pp_pass.Render(cmdbuf, regions_for(*frame), *frame, hmd_settings,
                           exported ? std::nullopt : std::optional<u32>{frame_id});
        }
        if (exported && eye_scan) {
            // What the headset's app gets, as the eye copy left it (in the general layout).
            const auto copy_barrier = [&](vk::PipelineStageFlags2 src_stage,
                                          vk::AccessFlags2 src_access,
                                          vk::PipelineStageFlags2 dst_stage,
                                          vk::AccessFlags2 dst_access) {
                const vk::ImageMemoryBarrier2 barrier = {
                    .srcStageMask = src_stage,
                    .srcAccessMask = src_access,
                    .dstStageMask = dst_stage,
                    .dstAccessMask = dst_access,
                    .oldLayout = vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eGeneral,
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
            };
            copy_barrier(vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                             vk::PipelineStageFlagBits2::eFragmentShader,
                         vk::AccessFlagBits2::eColorAttachmentWrite,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            eye_scan->emplace_back(instance, draw_scheduler, ScreenshotKind::GameOnly,
                                   std::vector<std::filesystem::path>{}, frame->width,
                                   frame->height, swapchain.GetSurfaceFormat().format, false);
            CopyImageToReadback(cmdbuf, frame->image, vk::ImageLayout::eGeneral, eye_scan->back());
            copy_barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                         vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                             vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eTransferRead);
        }
        if (exported) {
            vr_exporter->Finalize(frame, cmdbuf);
        }
        DebugState.output_resolution = {frame->width, frame->height};
    }
    if (local != nullptr) {
        // An image that encodes for display itself.
        auto local_settings = hmd_settings;
        local_settings.linear_out = 1;
        hmd_pp_pass.Render(cmdbuf, regions_for(*local), *local, local_settings);
        DebugState.output_resolution = {local->width, local->height};
    }
    DebugState.game_resolution = {eye_width * 2, eye_height};
    if (eye_scan) {
        draw_scheduler.DeferPriorityOperation([eye_scan, frame_id]() {
            EyeScan::Look(*eye_scan, frame_id);
            EyeScan::busy = false;
        });
    }

    // Flush frame creation commands.
    for (Frame* target : {frame, local}) {
        if (target != nullptr) {
            target->ready_timeline = draw_scheduler.GetMasterSemaphore();
            target->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
            target->ready_tick = draw_scheduler.CurrentTick();
        }
    }
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    return {.shown = frame, .exported = local};
}

bool Presenter::IsFrameFinished(const Frame* frame) {
    return frame == nullptr || draw_scheduler.IsFree(frame->ready_tick);
}

bool Presenter::IsGpuBusy() {
    // The tick being recorded has not been handed to the GPU yet; the one before it has.
    const u64 recording = draw_scheduler.CurrentTick();
    return recording > 1 && !draw_scheduler.IsFree(recording - 1);
}

bool Presenter::DeliverHmdFrame(Frame* frame, const Core::Vr::PresentedFrame& info) {
    if (frame == nullptr || frame->host_buffer < 0) {
        return false;
    }
    vr_exporter->Deliver(frame, info);
    return true;
}

void Presenter::DumpGpuImages(std::string_view folder) {
    // SHADPS4_DUMP_WIDTH=<pixels>: how wide the pictures are at most (640 unless told; the
    // targets' own width for looking at what is in a pixel).
    static const u32 ThumbWidth = [] {
        const char* value = std::getenv("SHADPS4_DUMP_WIDTH");
        const int width = value != nullptr ? std::atoi(value) : 0;
        return width >= 64 ? static_cast<u32>(width) : 640u;
    }();
    const auto dump_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / folder;
    std::error_code ec;
    std::filesystem::remove_all(dump_dir, ec);
    std::filesystem::create_directories(dump_dir, ec);

    // Collect first: transitions and submits below must not run under the cache lock.
    std::vector<VideoCore::Image*> images;
    texture_cache.ForEachImage([&](VideoCore::Image& image) {
        const auto& info = image.info;
        LOG_INFO(Render_Vulkan,
                 "cache image at {:#x}: {}x{}x{}, {}, samples {}, levels {}, layers {}, flags {:#x}{}",
                 info.guest_address, info.size.width, info.size.height, info.size.depth,
                 vk::to_string(info.pixel_format), info.num_samples, info.resources.levels,
                 info.resources.layers, static_cast<u32>(image.flags),
                 info.props.is_depth ? ", depth" : "");
        if (!(image.flags & VideoCore::ImageFlagBits::GpuModified) || info.props.is_depth ||
            info.props.is_volume || info.size.width < 64 || info.size.height < 64) {
            return;
        }
        images.push_back(&image);
    });

    const vk::Device device = instance.GetDevice();
    u32 index = 0;
    for (VideoCore::Image* image : images) {
        const auto& info = image->info;
        const u32 width = std::min(info.size.width, ThumbWidth);
        const u32 height = std::max(1u, info.size.height * width / info.size.width);

        const vk::ImageCreateInfo thumb_ci = {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
        };
        const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage thumb_handle{};
        VmaAllocation thumb_allocation{};
        const VkImageCreateInfo thumb_ci_raw = static_cast<VkImageCreateInfo>(thumb_ci);
        if (vmaCreateImage(instance.GetAllocator(), &thumb_ci_raw, &alloc_info, &thumb_handle,
                           &thumb_allocation, nullptr) != VK_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "Could not allocate a thumbnail for the render target dump");
            return;
        }
        const vk::Image thumb{thumb_handle};

        ScreenshotReadback readback{instance,
                                    draw_scheduler,
                                    ScreenshotKind::GameOnly,
                                    {},
                                    width,
                                    height,
                                    vk::Format::eR8G8B8A8Unorm,
                                    false};

        draw_scheduler.EndRendering();
        const auto cmdbuf = draw_scheduler.CommandBuffer();
        const vk::ImageSubresourceRange color_range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        };
        const auto thumb_barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = thumb,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, barrier);
        };

        image->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                       cmdbuf);

        // Multisampled targets cannot be blitted, they are resolved into a scratch image first.
        const u32 samples = static_cast<u32>(image->backing->image.image_ci.samples);
        vk::Image blit_source = image->GetImage();
        VkImage resolved_handle{};
        VmaAllocation resolved_allocation{};
        if (samples > 1) {
            const vk::ImageCreateInfo resolved_ci = {
                .imageType = vk::ImageType::e2D,
                .format = info.pixel_format,
                .extent = {info.size.width, info.size.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc |
                         vk::ImageUsageFlagBits::eColorAttachment,
            };
            const VkImageCreateInfo resolved_ci_raw = static_cast<VkImageCreateInfo>(resolved_ci);
            if (vmaCreateImage(instance.GetAllocator(), &resolved_ci_raw, &alloc_info,
                               &resolved_handle, &resolved_allocation, nullptr) != VK_SUCCESS) {
                vmaDestroyImage(instance.GetAllocator(), thumb_handle, thumb_allocation);
                continue;
            }
            const vk::Image resolved{resolved_handle};
            const auto resolved_barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
                const vk::ImageMemoryBarrier barrier = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                    .oldLayout = from,
                    .newLayout = to,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = resolved,
                    .subresourceRange = color_range,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                       vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                                       barrier);
            };
            resolved_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
            cmdbuf.resolveImage(image->GetImage(), vk::ImageLayout::eTransferSrcOptimal, resolved,
                                vk::ImageLayout::eTransferDstOptimal,
                                vk::ImageResolve{
                                    .srcSubresource = MakeImageSubresourceLayers(),
                                    .dstSubresource = MakeImageSubresourceLayers(),
                                    .extent = {info.size.width, info.size.height, 1},
                                });
            resolved_barrier(vk::ImageLayout::eTransferDstOptimal,
                             vk::ImageLayout::eTransferSrcOptimal);
            blit_source = resolved;
        }

        thumb_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
        cmdbuf.blitImage(blit_source, vk::ImageLayout::eTransferSrcOptimal, thumb,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitStretch(info.size.width, info.size.height, width, height),
                         vk::Filter::eLinear);
        thumb_barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
        CopyImageToReadback(cmdbuf, thumb, vk::ImageLayout::eTransferSrcOptimal, readback);
        draw_scheduler.Finish();

        std::vector<u8> rgba(static_cast<size_t>(width) * height * 4);
        std::memcpy(rgba.data(), readback.buffer.mapped_data.data(), rgba.size());
        for (size_t i = 3; i < rgba.size(); i += 4) {
            rgba[i] = 0xff;
        }
        const auto path = dump_dir / fmt::format("{:03}_{:x}_{}x{}_{}_s{}.png", index++,
                                                 info.guest_address, info.size.width,
                                                 info.size.height,
                                                 vk::to_string(info.pixel_format), samples);
        WritePng(path, rgba, width, height);
        vmaDestroyImage(instance.GetAllocator(), thumb_handle, thumb_allocation);
        if (resolved_handle) {
            vmaDestroyImage(instance.GetAllocator(), resolved_handle, resolved_allocation);
        }
    }
    LOG_INFO(Render_Vulkan, "Dumped {} render targets to {}", index, dump_dir.string());

    // SHADPS4_DUMP_IMAGE=<address>: the texture at that address as it is held, every level with
    // all its layers, raw (rt_dump/image_<address>_level<n>_<w>x<h>x<layers>_<format>.bin): for
    // what the pictures above leave out, textures the game only reads.
    const char* wanted_text = std::getenv("SHADPS4_DUMP_IMAGE");
    if (wanted_text == nullptr) {
        return;
    }
    // (Several addresses may be given, apart by commas.)
    std::vector<VAddr> wanted_list;
    for (const char* at = wanted_text; *at != '\0';) {
        char* end = nullptr;
        wanted_list.push_back(std::strtoull(at, &end, 16));
        if (end == at) {
            break;
        }
        at = *end == ',' ? end + 1 : end;
    }
    std::vector<VideoCore::Image*> raw_images;
    texture_cache.ForEachImage([&](VideoCore::Image& image) {
        if (std::ranges::find(wanted_list, image.info.guest_address) != wanted_list.end() &&
            image.info.num_samples == 1) {
            raw_images.push_back(&image);
        }
    });
    for (VideoCore::Image* image : raw_images) {
        const auto& info = image->info;
        const bool block = info.props.is_block;
        const u32 unit = info.num_bits / 8; // bytes a pixel, or a block of 4x4 for compressed
        for (u32 level = 0; level < info.resources.levels; ++level) {
            const u32 w = std::max(info.size.width >> level, 1u);
            const u32 h = std::max(info.size.height >> level, 1u);
            const u32 units_w = block ? (w + 3) / 4 : w;
            const u32 units_h = block ? (h + 3) / 4 : h;
            const u64 bytes = u64{units_w} * units_h * unit * info.resources.layers;
            VideoCore::Buffer buffer{instance,
                                     draw_scheduler,
                                     VideoCore::MemoryUsage::Download,
                                     0,
                                     vk::BufferUsageFlagBits::eTransferDst,
                                     bytes};
            draw_scheduler.EndRendering();
            const auto cmdbuf = draw_scheduler.CommandBuffer();
            image->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                           {}, cmdbuf);
            const vk::BufferImageCopy region = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                          : vk::ImageAspectFlagBits::eColor,
                        .mipLevel = level,
                        .baseArrayLayer = 0,
                        .layerCount = info.resources.layers,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {w, h, 1},
            };
            cmdbuf.copyImageToBuffer(image->GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                     buffer.Handle(), region);
            draw_scheduler.Finish();
            const auto path =
                dump_dir / fmt::format("image_{:x}_level{}_{}x{}x{}_{}.bin", info.guest_address,
                                       level, w, h, info.resources.layers,
                                       vk::to_string(info.pixel_format));
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(buffer.mapped_data.data()),
                      static_cast<std::streamsize>(bytes));
        }
        // And what the game's memory holds there, for comparison.
        {
            const auto path = dump_dir / fmt::format("image_{:x}_guest_{:x}_tile{}.bin",
                                                     info.guest_address, info.guest_size,
                                                     static_cast<u32>(info.tile_mode));
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(info.guest_address),
                      static_cast<std::streamsize>(info.guest_size));
        }
        LOG_INFO(Render_Vulkan,
                 "Dumped the texture at {:#x}: {} levels, {} layers, tile mode {}, {} bytes in "
                 "memory, flags {:#x}",
                 info.guest_address, info.resources.levels, info.resources.layers,
                 static_cast<u32>(info.tile_mode), info.guest_size, static_cast<u32>(image->flags));
    }
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (!frame) {
        return nullptr;
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_timeline = scheduler.GetMasterSemaphore();
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame) {
    static std::atomic_uint32_t present_traces{};
    const u32 trace_id = present_traces.fetch_add(1, std::memory_order_relaxed);
    if (!is_reusing_frame) {
        FrameStats::EndFrame();
    }

    // Bachata diagnostics: dump the guest output buffer every N presents when
    // the file <UserDir>/auto_shot_every exists (device black-screen triage;
    // env vars cannot be injected into the app-spawned runtime).
    static const u32 shot_every = [] {
        std::error_code ec;
        const auto shot_flag =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "auto_shot_every";
        if (std::filesystem::exists(shot_flag, ec)) {
            Common::FS::IOFile f(shot_flag, Common::FS::FileAccessMode::Read);
            std::array<char, 16> buf{};
            const auto n = f.ReadRaw<char>(buf.data(), buf.size() - 1);
            buf[n] = '\0';
            const u32 v = std::strtoul(buf.data(), nullptr, 10);
            LOG_INFO(Render_Vulkan, "BACHATA_AUTO_SHOT every={} presents", v);
            return v;
        }
        return 0u;
    }();
    if (shot_every > 0 && trace_id > 0 && trace_id % shot_every == 0) {
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
    }
    // SHADPS4_SHOT_SECONDS=<n>[,<from>]: a picture every n seconds by the clock instead (from
    // so many seconds in), so that runs at different frame rates can be compared moment by
    // moment.
    {
        using Clock = std::chrono::steady_clock;
        static double shot_from = 0.0;
        static const double shot_seconds = [] {
            const char* value = std::getenv("SHADPS4_SHOT_SECONDS");
            if (value == nullptr) {
                return 0.0;
            }
            if (const char* comma = std::strchr(value, ','); comma != nullptr) {
                shot_from = std::atof(comma + 1);
            }
            return std::atof(value);
        }();
        static const auto shot_start = Clock::now();
        static u32 shots_taken = 0;
        if (shot_seconds > 0.0 &&
            std::chrono::duration<double>(Clock::now() - shot_start).count() >=
                shot_from + shot_seconds * (shots_taken + 1)) {
            ++shots_taken;
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        }
    }
    // Log first 64, then every 16th, always around suspected 32-present boundary.
    const bool trace = trace_id < 64 || (trace_id % 16u) == 0u ||
                       (trace_id >= 28 && trace_id <= 40);
    // Free the frame for reuse. One that was shown again was taken out of the pool for that.
    const auto free_frame = [&] {
        std::scoped_lock fl{free_mutex};
        if (!is_reusing_frame) {
            last_submit_frame = frame;
        }
        free_queue.push(frame);
        free_cv.notify_one();
    };

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }
    if (trace) {
        LOG_INFO(Render_Vulkan, "BACHATA_PRESENT_TRACE id={} stage=acquire_done", trace_id);
        LOG_INFO(Render_Vulkan,
                 "FRAME_SLOT_ACQUIRE presentId={} slot={} frameW={} frameH={} "
                 "swapchainFrameIndex={} swapchainImageCount={} readyTick={}",
                 trace_id, frame ? int(frame->id) : -1, frame ? frame->width : 0,
                 frame ? frame->height : 0, swapchain.GetFrameIndex(), swapchain.GetImageCount(),
                 frame ? frame->ready_tick : 0);
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    if (IsSyncTraceEnabled()) {
        TraceSync("present frame %d reuse %d: reset fence %llx, waits for tick %llu", int(frame->id),
                  is_reusing_frame ? 1 : 0,
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(static_cast<VkFence>(frame->present_done))),
                  static_cast<unsigned long long>(frame->ready_tick));
    }
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    if (reset_result == vk::Result::eErrorDeviceLost) {
        LOG_CRITICAL(Render_Vulkan, "Device lost while resetting present done fence");
        static std::atomic<bool> device_lost_snapshot_logged{false};
        if (!device_lost_snapshot_logged.exchange(true)) {
            LOG_CRITICAL(Render_Vulkan,
                         "DEVICE_LOST_SNAPSHOT where=resetPresentFence presentId={} slot={}",
                         trace_id, frame ? int(frame->id) : -1);
        }
        free_frame();
        return;
    }
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_with_overlays_count > 0) {
        pending_screenshots.reserve(1);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        auto splash_path = Common::ElfInfo::Instance().GetSplashPath();
                        if (!splash_path.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngFile(splash_path);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());

        if (capture_with_overlays_count > 0) {
            pending_screenshots.emplace_back(
                instance, scheduler, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());
            auto& readback = pending_screenshots.back();

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    SubmitInfo info{};
    if (!swapchain.IsHeadless()) {
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
    }
#ifdef ENABLE_BACHATA_RUNTIME
    // The frame has to be finished before it is presented. Drivers that emulate timeline
    // semaphores (Turnip on the Adreno kernel driver) do that by holding the submission back
    // while the tick it waits for is still executing, and only look at it again the next time
    // the application calls them. The renderer may well be waiting for this very presentation
    // by then, so that call never comes and both stop for good. Waiting here costs the present
    // thread the time the GPU needs for the frame, and nobody anything else. (Through the
    // timeline's own wait: the driver must not be asked for ticks out of order.)
    if (frame->ready_timeline != nullptr) {
        frame->ready_timeline->Wait(frame->ready_tick);
    }
#else
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
#endif
    if (!swapchain.IsHeadless()) {
        info.AddSignal(swapchain.GetPresentReadySemaphore());
    }
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    if (trace) {
        LOG_INFO(Render_Vulkan, "BACHATA_PRESENT_TRACE id={} stage=flush_done", trace_id);
    }

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        const bool presented = swapchain.Present();
        if (trace) {
            LOG_INFO(Render_Vulkan, "BACHATA_PRESENT_TRACE id={} stage=present_done ok={}", trace_id,
                     presented);
        }
        if (!presented) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    const bool nonblock =
        liverpool && liverpool->IsGpuThread() && liverpool->InGfxTask();
    {
        std::unique_lock lock{free_mutex};
        if (nonblock) {
            if (free_queue.empty()) {
                return nullptr;
            }
        } else {
            free_cv.wait(lock, [this] { return !free_queue.empty(); });
        }
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    static std::atomic_uint32_t get_frame_traces{};
    const u32 get_id = get_frame_traces.fetch_add(1, std::memory_order_relaxed);
    if (get_id < 64 || (get_id % 16u) == 0u || (get_id >= 28 && get_id <= 40)) {
        LOG_INFO(Render_Vulkan,
                 "GET_RENDER_FRAME_WAIT frameGetId={} slot={} poolSize={} freeRemaining={} "
                 "readyTick={} (before waitForFences)",
                 get_id, frame ? int(frame->id) : -1, present_frames.size(), free_queue.size(),
                 frame ? frame->ready_tick : 0);
    }

    const u64 timeout_ns = AmdGpu::PresentFenceTimeoutNs(
        liverpool && liverpool->IsGpuThread(), liverpool && liverpool->InGfxTask());
    if (IsSyncTraceEnabled()) {
        TraceSync("get render frame %d: wait fence %llx, timeout %s", int(frame->id),
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(static_cast<VkFence>(frame->present_done))),
                  timeout_ns == 0 ? "none" : "forever");
    }
    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, timeout_ns);
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eTimeout) {
            if (nonblock) {
                static std::atomic<u32> defer_count{};
                if (defer_count.fetch_add(1, std::memory_order_relaxed) % 100 == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "GetRenderFrame: present fence busy on GpuComm; deferring flip");
                }
                std::scoped_lock relock{free_mutex};
                free_queue.push(frame);
                free_cv.notify_one();
                return nullptr;
            }
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Soft-fail on Mali/Vortek: ASSERT_MSG here was exit 133 after the first
            // real EOP flip. Return the frame so Present can soft-fail acquire/present
            // and the session can stop without an immediate trap.
            // Client-side snapshot header; server dumps live alloc map via
            // DEVICE_LOST_SNAPSHOT in libbachata_vortek_server (logcat Bachata.Vortek.GpuTrack).
            static std::atomic<bool> device_lost_snapshot_logged{false};
            const u32 flip_num = DebugState.GetFrameNum();
            LOG_CRITICAL(Render_Vulkan, "Device lost during waiting for a frame (GetRenderFrame)");
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=GetRenderFrame lastPresent={} "
                             "currentFrameId={} frameSize={}x{} hdr={} "
                             "flipFrameNum={} (server alloc dump in logcat tag "
                             "Bachata.Vortek.GpuTrack)",
                             flip_num, frame ? int(frame->id) : -1, frame ? frame->width : 0,
                             frame ? frame->height : 0, frame && frame->is_hdr ? 1 : 0, flip_num);
            }
            break;
        }
        LOG_ERROR(Render_Vulkan, "Unexpected waitForFences result in GetRenderFrame: {}",
                  vk::to_string(result));
        break;
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (expected_ratio) {
        if (ratio > *expected_ratio) {
            expected_frame_width = static_cast<s32>(height * *expected_ratio);
        } else {
            expected_frame_height = static_cast<s32>(width / *expected_ratio);
        }
    }
}

} // namespace Vulkan
