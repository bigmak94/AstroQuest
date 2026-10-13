// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocation_T;

namespace Vulkan {

class Instance;
class Scheduler;

/// SHADPS4_VR_FOVEATION=<1..3>: what a title draws for an eye is shaded in blocks of 2x2 pixels
/// towards its edges instead of pixel by pixel, where the headset's lenses show less of it
/// anyway (and at 3 in blocks of 4x4 at the far edges). The shapes are drawn as before, every
/// pixel with its own depth; only the colour comes for a block at a time there. It takes from
/// the GPU what shading pixels costs in that part of the picture.
///
/// The GPU does it with a small map of how coarsely to shade each part of the picture (a
/// fragment shading rate attachment, VK_KHR_fragment_shading_rate) given to the render passes
/// that draw a title's scene for an eye. Which passes those are is the title's business
/// (KnownTitle::IsSceneSize); the picture is taken to look straight ahead from its middle,
/// which is what a symmetric projection (SHADPS4_VR_FOV_SYMMETRIC) makes it.
class Foveation {
public:
    /// 1 shades the outer half of the picture coarsely (by area: its corners are a lot of it),
    /// 2 seven tenths, 3 four fifths and the far corners coarser still, 4 nine tenths with
    /// three fifths of the picture in blocks of 4x4. In level 3-2 of Astro Bot, with the models
    /// lighter (SHADPS4_TITLE_LOD=0.333), a frame took 17.0 ms of the headset's GPU without,
    /// 15.1, 14.3 and 12.8 ms with levels 1 to 3; level 4 took 5.5% less than level 3 there
    /// (at 960x1080, the two taking turns in one run), 3% less in the first level.
    static constexpr int Strongest = 4;

    /// The level asked for, 0 when none is.
    static int Level();

    /// From how far out (as a share of the way from the middle of an eye's scene to its edge,
    /// counted on both axes) the level asked for shades in blocks; above 1 where it does not.
    static float CoarseFrom();

    /// Shading rate maps are texels of this many pixels square, as close to it as the GPU
    /// allows.
    static constexpr u32 WantedTexel = 16;

    Foveation(const Instance& instance, Scheduler& scheduler);
    ~Foveation();

    Foveation(const Foveation&) = delete;
    Foveation& operator=(const Foveation&) = delete;

    /// The map for a render area of that size, made the first time it is asked for (which
    /// ends the render pass being recorded). Null where the GPU cannot shade coarsely. (With
    /// SHADPS4_DBG_AB_ONLY=fov, the level below the one asked for while that is left out.)
    vk::ImageView MapFor(u32 width, u32 height);

    /// Pixels a texel of the maps covers on each side.
    u32 Texel() const {
        return texel;
    }

private:
    struct Map {
        u32 width{};
        u32 height{};
        int level{};
        vk::Image image{};
        VmaAllocation_T* allocation{};
        vk::ImageView view{};
    };

    bool Make(Map& map);

    const Instance& instance;
    Scheduler& scheduler;
    int level{};
    u32 texel{};
    std::vector<Map> maps;
};

} // namespace Vulkan
