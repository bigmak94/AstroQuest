// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/runtime_info.h"

namespace Vulkan {

class Instance;

/// The known title draws its scene at one of four sizes and enlarges it itself, in its final
/// pass, to the picture it hands to the headset (Astro Bot: 1440x1536 an eye), sampling it
/// bilinearly. Where asked for, that pass's pixel shader reads its scene with FSR 1's edge
/// adaptive filter (EASU) instead: the shader is compiled with the filter in place of its one
/// read of the scene (Shader::SceneUpscale; the SPIR-V backend's scene_easu), and the rest of the
/// pass is the title's (its tone, its levels, its sRGB encoding). Edges come out crisper than
/// bilinear makes them; no detail is added, and no picture is made on the way.
///
/// Only where the picture is shaded finely (Foveation::CoarseFrom): EASU makes the blocks of the
/// coarse shading beyond it stand out.
namespace TitleUpscale {

/// SHADPS4_TITLE_FSR: 0 (the default; the Quest app asks for 2) off, 1 where the scene is
/// enlarged by 1.3 or more on both axes (816x870 and 960x1080 to 1440x1536), 2 wherever it is
/// enlarged. SHADPS4_TITLE_FSR_HALF=0 has the filter work in 32 bits, SHADPS4_TITLE_FSR_FLAT=0
/// run it where the scene is flat as well.
int Mode();

/// How a pixel shader program (its hash) is to read the scene where it is compiled now: for the
/// title's final pass as Mode asks (unless the measuring aid SHADPS4_DBG_AB leaves out "fsr"),
/// for anything else as it asks itself.
Shader::SceneUpscale For(u64 pgm_hash, const Instance& instance);

/// Where EASU stops, as a share of the way from the middle of the picture to its edge (2: it
/// does not).
float FineRadius(const Instance& instance);

/// Whether a final pass that enlarges a scene of the first size to a picture of the second
/// reads it with EASU (in its middle) when it reads it as For asks.
bool Enlarges(u32 scene_width, u32 scene_height, u32 width, u32 height);

/// Whether a final pass reading a scene of this size makes the TV's picture
/// (Shader::SceneUpscale::WidestScene), which is read as the title asks, rather than an eye's.
bool ForTv(u32 scene_width, u32 scene_height);

} // namespace TitleUpscale

} // namespace Vulkan
