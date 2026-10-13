// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#include "common/logging/log.h"
#include "core/known_title.h"
#include "video_core/renderer_vulkan/vk_foveation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_title_upscale.h"

namespace Vulkan::TitleUpscale {

namespace {

/// Mode 1: enlarged by this much at least on both axes, or the title's bilinear and the eye
/// copy's sharpening do as well.
constexpr float LeastScale = 1.3f;
/// Mode 2: enlarged at all (a scene of the picture's own size is read as it is).
constexpr float AnyScale = 1.01f;

float Least() {
    return Mode() == 1 ? LeastScale : AnyScale;
}

/// A setting that is on unless set to 0.
bool Unless0(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] != '0';
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* value = std::getenv("SHADPS4_TITLE_FSR");
        return value != nullptr ? std::clamp(std::atoi(value), 0, 2) : 0;
    }();
    return mode;
}

float FineRadius(const Instance& instance) {
    // Whole texels of the shading rate map are coarse where their middle is beyond the radius,
    // and EASU reads two scene texels around a pixel: it stops a little before.
    return Foveation::Level() > 0 && instance.IsFoveationSupported()
               ? std::max(Foveation::CoarseFrom() - 0.03f, 0.0f)
               : 2.0f;
}

Shader::SceneUpscale For(u64 pgm_hash, const Instance& instance) {
    if (Mode() == 0 || !Core::KnownTitle::IsSceneEnlargement(pgm_hash) ||
        FrameStats::AbOff("fsr")) {
        return {};
    }
    // (Under the measuring aid, "fsrhalf" and "fsrflat" take their part away.)
    static const bool half = Unless0("SHADPS4_TITLE_FSR_HALF");
    static const bool skip_flat = Unless0("SHADPS4_TITLE_FSR_FLAT");
    const Shader::SceneUpscale upscale{
        .least_scale = Least(),
        .fine_radius = FineRadius(instance),
        .half = half && !FrameStats::AbOff("fsrhalf"),
        .skip_flat = skip_flat && !FrameStats::AbOff("fsrflat"),
    };
    static std::atomic_flag logged;
    if (!logged.test_and_set()) {
        LOG_INFO(Render_Vulkan,
                 "Title upscale: the title's final pass reads its scene with FSR 1 EASU ({}) "
                 "where it is enlarged{}{}{}",
                 upscale.half && instance.IsShaderFloat16Supported() &&
                         instance.IsShaderInt16Supported()
                     ? "16-bit"
                     : "32-bit",
                 Mode() == 1 ? fmt::format(" by {:.1f} or more", LeastScale) : std::string{},
                 upscale.fine_radius < 1.42f
                     ? fmt::format(", within {:.0f}% of the way to the edges, bilinear beyond",
                                   upscale.fine_radius * 100.0f)
                     : std::string{},
                 upscale.skip_flat ? ", and bilinear where it is flat" : "");
    }
    return upscale;
}

bool Enlarges(u32 scene_width, u32 scene_height, u32 width, u32 height) {
    if (Mode() == 0 || FrameStats::AbOff("fsr") || ForTv(scene_width, scene_height)) {
        return false;
    }
    const float least = Least();
    return static_cast<float>(width) >= least * static_cast<float>(scene_width) &&
           static_cast<float>(height) >= least * static_cast<float>(scene_height);
}

bool ForTv(u32 scene_width, u32 scene_height) {
    return static_cast<float>(scene_width) >
           Shader::SceneUpscale::WidestScene * static_cast<float>(scene_height);
}

} // namespace Vulkan::TitleUpscale
