// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 32;
static constexpr u32 NUM_FMASKS = 8;

enum class BufferType : u32 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
};

struct Info;

/// Read-only guest buffers no larger than this many bytes are uniform buffers in the shaders
/// instead of storage buffers; 0 makes every buffer a storage buffer. The renderer sets it
/// before each draw, so that a draw's shaders, pipeline layout and descriptors agree.
/// (Adreno's driver can serve what shaders read from uniform buffers from its constant
/// registers or constant cache; every read of a storage buffer goes to memory.)
inline std::atomic<u32> uniform_buffer_limit{0};

struct BufferResource {
    u32 sharp_idx;
    IR::Type used_types;
    AmdGpu::Buffer inline_cbuf;
    BufferType buffer_type;
    u8 instance_attrib{};
    bool is_written{};
    bool is_formatted{};
    bool used_as_readconst{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    bool IsStorage([[maybe_unused]] const AmdGpu::Buffer buffer) const noexcept {
        // When using uniform buffers, a size is required at compilation time, so we need to
        // either compile a lot of shader specializations to handle each size or just force it to
        // the maximum possible size always. However, for some vendors the shader-supplied size is
        // used for bounds checking uniform buffer accesses, so the latter would effectively turn
        // off buffer robustness behavior. Instead, force storage buffers which are bounds checked
        // using the actual buffer size, unless the renderer asks for uniform buffers where they
        // may be (uniform_buffer_limit).
        // (Only by what a slot is, never by where its buffer lies or how long it is to the byte:
        // a slot whose kind changed from draw to draw would make another permutation of the
        // shader each time. Where a uniform buffer lies and how it ends is the business of
        // BufferCache::ObtainUniformBuffer.)
        const u32 limit = uniform_buffer_limit.load(std::memory_order_relaxed);
        return limit == 0 || is_written || buffer_type != BufferType::Guest ||
               buffer.GetSize() > limit;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer{};
        if (inline_cbuf) {
            buffer = inline_cbuf;
            if (inline_cbuf.base_address != 1) {
                buffer.base_address += info.pgm_base; // address fixup
            }
        } else {
            buffer = info.template ReadUdSharp<AmdGpu::Buffer>(sharp_idx);
        }
        if (!buffer.Valid()) {
            LOG_DEBUG(Render, "Encountered invalid buffer sharp");
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u32 { None, DynamicIndex, ConstantIndex };

struct ImageResource {
    static constexpr f32 Hosted2DSampleY = 0.5f;
    static constexpr u32 Hosted2DIntegerY = 0u;

    u32 sharp_idx;
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    MipStorageFallbackMode mip_fallback_mode{};
    u32 constant_mip_index{};

    bool Is1DHostedAs2D(const AmdGpu::Image& sharp) const noexcept {
        return !is_array && !is_written && !is_depth && !is_atomic && !is_r128 &&
               sharp.GetType() == AmdGpu::ImageType::Color1D && sharp.height == 0 &&
               sharp.NumLayers() == 1 && sharp.NumLevels() == 1 && sharp.NumSamples() == 1 &&
               sharp.GetTileMode() == AmdGpu::TileMode::Thin2DThin;
    }

    AmdGpu::ImageType GetHostViewType(const AmdGpu::Image& sharp) const noexcept {
        return Is1DHostedAs2D(sharp) ? AmdGpu::ImageType::Color2D
                                    : sharp.GetViewType(is_array);
    }

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (!is_r128) {
            image = info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
        } else {
            const auto raw = info.template ReadUdSharp<u128>(sharp_idx);
            std::memcpy(&image, &raw, sizeof(raw));
            image.pitch = image.width;
        }
        if (!image.Valid()) {
            LOG_DEBUG(Render_Vulkan, "Encountered invalid image sharp");
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                LOG_DEBUG(Render_Vulkan,
                          "Encountered non-depth image used with depth instruction!");
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    u32 NumBindings(const auto& info) const {
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::small_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    u32 sharp_idx;
    AmdGpu::Sampler inline_sampler;
    u32 is_inline_sampler : 1;
    u32 associated_image : 4;
    u32 disable_aniso : 1;

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        return is_inline_sampler ? inline_sampler
                                 : info.template ReadUdSharp<AmdGpu::Sampler>(sharp_idx);
    }
};
using SamplerResourceList = boost::container::small_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    u32 sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

} // namespace Shader
