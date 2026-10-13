// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <cmath>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

namespace {

/// The bits of a 16-bit float (rounded to the nearest; small values as subnormals).
u32 HalfBits(float value) {
    const u32 bits = std::bit_cast<u32>(value);
    const u32 sign = (bits >> 16) & 0x8000u;
    const float magnitude = std::fabs(value);
    if (magnitude < 6.103515625e-05f) {
        return sign | static_cast<u32>(std::lround(magnitude * 16777216.0f));
    }
    const s32 exponent = static_cast<s32>((bits >> 23) & 0xffu) - 127 + 15;
    if (exponent >= 31) {
        return sign | 0x7c00u;
    }
    const u32 mantissa = bits & 0x7fffffu;
    u32 half = sign | (static_cast<u32>(exponent) << 10) | (mantissa >> 13);
    if ((mantissa & 0x1000u) != 0) {
        ++half;
    }
    return half;
}

/// Where the four texels nearest a place differ by no more than this share of the brightest of
/// them (and a little in the dark), what is read there stays the title's own: whatever the
/// filter made of them would lie between them as well, closer than the picture can show.
constexpr float Flatness = 1.0f / 512.0f;
constexpr float FlatnessInTheDark = 1.0f / 16384.0f;

} // namespace

// A known title's final pass enlarges its scene to the picture it hands to a headset by reading it
// bilinearly (SceneUpscale). Where that is asked for, what the pass read is handed to the function
// made here, which reads the scene again around the same place with AMD FidelityFX Super
// Resolution 1's edge adaptive filter (EASU: FsrEasuF of host_shaders/fsr/ffx_fsr1.h, written out
// operation by operation) and returns that instead. The rest of the pass is the title's, and no
// picture is made on the way.
//
// The filter runs where the scene is enlarged by SceneUpscale::least_scale or more (by how many
// texels of the scene the place read moves from one pixel of the picture to the next) and within
// SceneUpscale::fine_radius of the middle; elsewhere what the title read stands. With
// SceneUpscale::skip_flat it also stands where the four texels nearest the place hardly differ.
// The pass makes the TV's picture as well (from a scene wider than SceneUpscale::WidestScene):
// nothing shows that one while the headset is in use, and the filter leaves it alone.
//
// The scene holds linear light, brighter than white where it shines. EASU wants values from 0 to
// 1 encoded for display, so they are brought there and back: FSR 1's reversible tone map
// (c / (1 + max(r, g, b))), then the square root; and after the filter the square, then the tone
// map undone. Light is taken as 256 at most: the tone map of more comes so close to 1 that undoing
// it multiplies the other channels wildly (a bright red glint would turn white), and the title's
// final pass shows anything that bright as white anyway. With SceneUpscale::half the filter
// itself (not the places read, nor the way back to linear light) works in 16 bits, as FSR 1's
// own FsrEasuH does.
Id EmitContext::DefineSceneEasu() {
    const SceneUpscale& upscale = runtime_info.fs_info.scene_upscale;
    const TextureDefinition& texture = images[0];
    AddCapability(spv::Capability::ImageQuery);
    const bool half = upscale.half && profile.support_float16 && profile.support_int16;
    if (half) {
        AddCapability(spv::Capability::Float16);
        AddCapability(spv::Capability::Int16);
    }

    const Id f1 = F32[1];
    const Id f2 = F32[2];
    const Id f3 = F32[3];
    const Id f4 = F32[4];
    const Id zero = f32_zero_value;
    const Id one = ConstF32(1.0f);
    // The filter's own arithmetic, and an unsigned integer of its width.
    const Id h1 = half ? TypeFloat(16) : f1;
    const Id h2 = half ? TypeVector(h1, 2) : f2;
    const Id h3 = half ? TypeVector(h1, 3) : f3;
    const Id h4 = half ? TypeVector(h1, 4) : f4;
    const Id hu = half ? U16 : U32[1];
    const auto hk = [&](float value) {
        return half ? Constant(h1, HalfBits(value)) : ConstF32(value);
    };
    const auto hk3 = [&](float value) {
        const Id part = hk(value);
        return ConstantComposite(h3, std::array{part, part, part});
    };
    const auto hk4 = [&](float value) {
        const Id part = hk(value);
        return ConstantComposite(h4, std::array{part, part, part, part});
    };
    const auto to_h = [&](Id type, Id value) { return half ? OpFConvert(type, value) : value; };
    const auto to_f = [&](Id type, Id value) { return half ? OpFConvert(type, value) : value; };

    const Id func = OpFunction(f4, spv::FunctionControlMask::MaskNone, TypeFunction(f4, f2, f4));
    const Id uv = OpFunctionParameter(f2);
    const Id bilinear = OpFunctionParameter(f4);
    Name(func, "scene_easu");
    const Id entry_label = AddLabel();
    const Id easu_label = OpLabel();
    const Id full_label = OpLabel();
    const Id easu_merge_label = OpLabel();
    const Id merge_label = OpLabel();

    const auto at = [&](Id type, Id vector, u32 index) {
        return OpCompositeExtract(type, vector, index);
    };

    // Whether to filter here (the same for a whole picture but its outer ring).
    const Id image = OpLoad(texture.image_type, texture.id);
    const Id size = OpConvertUToF(f2, OpImageQuerySizeLod(U32[2], image, u32_zero_value));
    const Id step_x = OpFMul(f1, OpFAbs(f1, at(f1, OpDPdx(f2, uv), 0)), at(f1, size, 0));
    const Id step_y = OpFMul(f1, OpFAbs(f1, at(f1, OpDPdy(f2, uv), 1)), at(f1, size, 1));
    const Id enlarged = OpFOrdLessThanEqual(U1[1], OpFMax(f1, step_x, step_y),
                                            ConstF32(1.0f / upscale.least_scale));
    const Id from_middle = OpFma(f2, uv, ConstF32(2.0f, 2.0f), ConstF32(-1.0f, -1.0f));
    const Id fine = OpFOrdLessThan(U1[1], OpDot(f1, from_middle, from_middle),
                                   ConstF32(upscale.fine_radius * upscale.fine_radius));
    const Id for_eyes =
        OpFOrdLessThanEqual(U1[1], at(f1, size, 0),
                            OpFMul(f1, at(f1, size, 1), ConstF32(SceneUpscale::WidestScene)));
    // (Made before the merge, which has to come right before the branch.)
    const Id filter = OpLogicalAnd(U1[1], OpLogicalAnd(U1[1], enlarged, fine), for_eyes);
    OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(filter, easu_label, merge_label);

    AddLabel(easu_label);
    const Id sampler = OpLoad(sampler_type, samplers[0]);
    const Id texel = OpFDiv(f2, ConstF32(1.0f, 1.0f), size);
    // The place among the scene's texels: the texel 'f' up and to the left of it, and how far
    // beyond the middle of that one it is.
    const Id place = OpFma(f2, uv, size, ConstF32(-0.5f, -0.5f));
    const Id f_texel = OpFloor(f2, place);
    const Id pp = to_h(h2, OpFSub(f2, place, f_texel));
    const Id pp_x = at(h1, pp, 0);
    const Id pp_y = at(h1, pp, 1);

    // Twelve texels, read four by four (and channel by channel) around four places:
    //    b c      (0)
    //  e f g h  (1) (2)
    //  i j k l
    //    n o      (3)
    const Id p0 = OpFMul(f2, OpFAdd(f2, f_texel, ConstF32(1.0f, -1.0f)), texel);
    const Id p1 = OpFma(f2, ConstF32(-1.0f, 2.0f), texel, p0);
    const Id p2 = OpFma(f2, ConstF32(1.0f, 2.0f), texel, p0);
    const Id p3 = OpFma(f2, ConstF32(0.0f, 4.0f), texel, p0);
    struct Four {
        Id r;
        Id g;
        Id b;
    };
    const Id zero4 = ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    const Id brightest4 = ConstF32(256.0f, 256.0f, 256.0f, 256.0f);
    // (A sampled image is made in each block that reads with it: one may not be used in
    // another.)
    const auto gather = [&](Id scene, Id place_of_four) {
        const auto channel = [&](u32 component) {
            const Id texels = OpImageGather(f4, scene, place_of_four, ConstU32(component));
            return OpFMin(f4, OpFMax(f4, texels, zero4), brightest4);
        };
        const Id r = channel(0);
        const Id g = channel(1);
        const Id b = channel(2);
        return Four{.r = r, .g = g, .b = b};
    };
    // The four nearest (f g / j k) first: they bound what the filter makes.
    const Id scene_near = OpSampledImage(texture.sampled_type, image, sampler);
    const Four ijfe_linear = gather(scene_near, p1);
    const Four klhg_linear = gather(scene_near, p2);
    if (upscale.skip_flat) {
        const auto colour = [&](const Four& four, u32 index) {
            return OpCompositeConstruct(f3, at(f1, four.r, index), at(f1, four.g, index),
                                        at(f1, four.b, index));
        };
        const Id f_c = colour(ijfe_linear, 2);
        const Id g_c = colour(klhg_linear, 3);
        const Id j_c = colour(ijfe_linear, 1);
        const Id k_c = colour(klhg_linear, 0);
        const Id lowest = OpFMin(f3, OpFMin(f3, f_c, g_c), OpFMin(f3, j_c, k_c));
        const Id highest = OpFMax(f3, OpFMax(f3, f_c, g_c), OpFMax(f3, j_c, k_c));
        const Id allowed =
            OpFma(f3, highest, ConstF32(Flatness, Flatness, Flatness),
                  ConstF32(FlatnessInTheDark, FlatnessInTheDark, FlatnessInTheDark));
        const Id flat =
            OpAll(U1[1], OpFOrdLessThanEqual(U1[3], OpFSub(f3, highest, lowest), allowed));
        OpSelectionMerge(easu_merge_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(flat, easu_merge_label, full_label);
    } else {
        OpBranch(full_label);
    }

    AddLabel(full_label);
    const Id scene_far = OpSampledImage(texture.sampled_type, image, sampler);
    const Four bczz_linear = gather(scene_far, p0);
    const Four zzon_linear = gather(scene_far, p3);
    const Id one4 = ConstF32(1.0f, 1.0f, 1.0f, 1.0f);
    const auto encode = [&](const Four& four) {
        const Id k =
            OpFDiv(f4, one4, OpFAdd(f4, one4, OpFMax(f4, four.r, OpFMax(f4, four.g, four.b))));
        const auto channel = [&](Id linear) {
            return OpSqrt(h4, to_h(h4, OpFMul(f4, linear, k)));
        };
        return Four{.r = channel(four.r), .g = channel(four.g), .b = channel(four.b)};
    };
    const Four bczz = encode(bczz_linear);
    const Four ijfe = encode(ijfe_linear);
    const Four klhg = encode(klhg_linear);
    const Four zzon = encode(zzon_linear);

    // Twice a luma, roughly.
    const Id half4 = hk4(0.5f);
    const auto luma = [&](const Four& four) {
        return OpFma(h4, four.b, half4, OpFma(h4, four.r, half4, four.g));
    };
    const Id bczz_l = luma(bczz);
    const Id ijfe_l = luma(ijfe);
    const Id klhg_l = luma(klhg);
    const Id zzon_l = luma(zzon);
    const Id b_l = at(h1, bczz_l, 0);
    const Id c_l = at(h1, bczz_l, 1);
    const Id i_l = at(h1, ijfe_l, 0);
    const Id j_l = at(h1, ijfe_l, 1);
    const Id f_l = at(h1, ijfe_l, 2);
    const Id e_l = at(h1, ijfe_l, 3);
    const Id k_l = at(h1, klhg_l, 0);
    const Id l_l = at(h1, klhg_l, 1);
    const Id h_l = at(h1, klhg_l, 2);
    const Id g_l = at(h1, klhg_l, 3);
    const Id o_l = at(h1, zzon_l, 2);
    const Id n_l = at(h1, zzon_l, 3);

    // FSR's own approximations (ffx_a.h APrxLoRcpF1/H1 here, APrxLoRsqF1 below): what the
    // filter is tuned with.
    const Id rcp_magic = half ? Constant(U16, 0x7784u) : ConstU32(0x7ef07ebbu);
    const auto rcp_approx = [&](Id a) {
        return OpBitcast(h1, OpISub(hu, rcp_magic, OpBitcast(hu, a)));
    };
    const Id h_zero = hk(0.0f);
    const Id h_one = hk(1.0f);
    const auto saturate = [&](Id a) { return OpFMin(h1, OpFMax(h1, a, h_zero), h_one); };

    // The direction of an edge and how much of one there is, from the four texels around the
    // place, each weighed as bilinear would and seen with the cross of lumas around it
    // (FsrEasuSetF):
    //    a
    //  b c d
    //    e
    Id dir_x = h_zero;
    Id dir_y = h_zero;
    Id len = h_zero;
    const auto set = [&](Id weight, Id l_a, Id l_b, Id l_c, Id l_d, Id l_e) {
        const auto axis = [&](Id l_before, Id l_after, Id& dir) {
            const Id after = OpFSub(h1, l_after, l_c);
            const Id before = OpFSub(h1, l_c, l_before);
            const Id across = OpFSub(h1, l_after, l_before);
            dir = OpFma(h1, across, weight, dir);
            const Id steeper = rcp_approx(OpFMax(h1, OpFAbs(h1, after), OpFAbs(h1, before)));
            const Id length = saturate(OpFMul(h1, OpFAbs(h1, across), steeper));
            len = OpFma(h1, OpFMul(h1, length, length), weight, len);
        };
        axis(l_b, l_d, dir_x);
        axis(l_a, l_e, dir_y);
    };
    const Id rest_x = OpFSub(h1, h_one, pp_x);
    const Id rest_y = OpFSub(h1, h_one, pp_y);
    set(OpFMul(h1, rest_x, rest_y), b_l, e_l, f_l, g_l, j_l);
    set(OpFMul(h1, pp_x, rest_y), c_l, f_l, g_l, h_l, k_l);
    set(OpFMul(h1, rest_x, pp_y), f_l, i_l, j_l, k_l, n_l);
    set(OpFMul(h1, pp_x, pp_y), g_l, j_l, k_l, l_l, o_l);

    // The direction made a unit (along x where there is none), in 32 bits: in 16 the squares of
    // a short one fall below what a GPU that flushes the smallest halves keeps. The amount of
    // edge from 0 to 1, shaped.
    {
        const Id x32 = to_f(f1, dir_x);
        const Id y32 = to_f(f1, dir_y);
        const Id dir_r2 = OpFAdd(f1, OpFMul(f1, x32, x32), OpFMul(f1, y32, y32));
        const Id flat_dir = OpFOrdLessThan(U1[1], dir_r2, ConstF32(1.0f / 32768.0f));
        const Id rsq = OpBitcast(
            f1, OpISub(U32[1], ConstU32(0x5f347d74u),
                       OpShiftRightLogical(U32[1], OpBitcast(U32[1], dir_r2), ConstU32(1u))));
        const Id dir_r = OpSelect(f1, flat_dir, one, rsq);
        dir_x = to_h(h1, OpFMul(f1, OpSelect(f1, flat_dir, one, x32), dir_r));
        dir_y = to_h(h1, OpFMul(f1, y32, dir_r));
    }
    len = OpFMul(h1, len, hk(0.5f));
    len = OpFMul(h1, len, len);
    // The kernel: stretched along the edge (1 across x or y, the square root of 2 diagonally),
    // squeezed across it, its negative lobe and window wider the more edge there is.
    const Id stretch =
        OpFMul(h1, OpFAdd(h1, OpFMul(h1, dir_x, dir_x), OpFMul(h1, dir_y, dir_y)),
               rcp_approx(OpFMax(h1, OpFAbs(h1, dir_x), OpFAbs(h1, dir_y))));
    const Id len2_x = OpFma(h1, OpFSub(h1, stretch, h_one), len, h_one);
    const Id len2_y = OpFma(h1, hk(-0.5f), len, h_one);
    const Id lob = OpFma(h1, hk(1.0f / 4.0f - 0.04f - 0.5f), len, hk(0.5f));
    const Id clp = rcp_approx(lob);

    // The colours of the twelve texels; the four nearest bound the result (no ringing).
    const auto colour = [&](const Four& four, u32 index) {
        return OpCompositeConstruct(h3, at(h1, four.r, index), at(h1, four.g, index),
                                    at(h1, four.b, index));
    };
    const Id f_c = colour(ijfe, 2);
    const Id g_c = colour(klhg, 3);
    const Id j_c = colour(ijfe, 1);
    const Id k_c = colour(klhg, 0);
    const Id min4 = OpFMin(h3, OpFMin(h3, f_c, g_c), OpFMin(h3, j_c, k_c));
    const Id max4 = OpFMax(h3, OpFMax(h3, f_c, g_c), OpFMax(h3, j_c, k_c));

    // Each texel weighed by where it lies in the kernel (FsrEasuTapF).
    Id sum = hk3(0.0f);
    Id weights = h_zero;
    const Id minus_dir_y = OpFNegate(h1, dir_y);
    const Id lanczos_b = hk(2.0f / 5.0f);
    const Id minus_one = hk(-1.0f);
    const Id lanczos_a = hk(25.0f / 16.0f);
    const Id lanczos_c = hk(-(25.0f / 16.0f - 1.0f));
    const auto tap = [&](float offset_x, float offset_y, Id texel_colour) {
        const Id off_x = OpFSub(h1, hk(offset_x), pp_x);
        const Id off_y = OpFSub(h1, hk(offset_y), pp_y);
        const Id v_x = OpFMul(h1, OpFma(h1, off_x, dir_x, OpFMul(h1, off_y, dir_y)), len2_x);
        const Id v_y =
            OpFMul(h1, OpFma(h1, off_x, minus_dir_y, OpFMul(h1, off_y, dir_x)), len2_y);
        const Id d2 = OpFMin(h1, OpFma(h1, v_x, v_x, OpFMul(h1, v_y, v_y)), clp);
        // Lanczos 2 without sin, sqrt or division:
        // (25/16 (2/5 d2 - 1)^2 - (25/16 - 1)) (lob d2 - 1)^2.
        Id base = OpFma(h1, lanczos_b, d2, minus_one);
        Id window = OpFma(h1, lob, d2, minus_one);
        base = OpFMul(h1, base, base);
        window = OpFMul(h1, window, window);
        base = OpFma(h1, lanczos_a, base, lanczos_c);
        const Id weight = OpFMul(h1, base, window);
        sum = OpFAdd(h3, sum, OpVectorTimesScalar(h3, texel_colour, weight));
        weights = OpFAdd(h1, weights, weight);
    };
    tap(0.0f, -1.0f, colour(bczz, 0)); // b
    tap(1.0f, -1.0f, colour(bczz, 1)); // c
    tap(-1.0f, 1.0f, colour(ijfe, 0)); // i
    tap(0.0f, 1.0f, j_c);
    tap(0.0f, 0.0f, f_c);
    tap(-1.0f, 0.0f, colour(ijfe, 3)); // e
    tap(1.0f, 1.0f, k_c);
    tap(2.0f, 1.0f, colour(klhg, 1)); // l
    tap(2.0f, 0.0f, colour(klhg, 2)); // h
    tap(1.0f, 0.0f, g_c);
    tap(1.0f, 2.0f, colour(zzon, 2)); // o
    tap(0.0f, 2.0f, colour(zzon, 3)); // n
    Id c = OpVectorTimesScalar(h3, sum, OpFDiv(h1, h_one, weights));
    c = to_f(f3, OpFMin(h3, max4, OpFMax(h3, min4, c)));

    // Back to linear light (in 32 bits: near white, 16 would be too coarse for it).
    c = OpFMul(f3, c, c);
    const Id brightest =
        OpFMax(f1, at(f1, c, 0), OpFMax(f1, at(f1, c, 1), at(f1, c, 2)));
    c = OpVectorTimesScalar(
        f3, c,
        OpFDiv(f1, one, OpFMax(f1, ConstF32(1.0f / 32768.0f), OpFSub(f1, one, brightest))));
    const Id filtered = OpCompositeConstruct(f4, at(f1, c, 0), at(f1, c, 1), at(f1, c, 2),
                                             at(f1, bilinear, 3));
    OpBranch(easu_merge_label);

    AddLabel(easu_merge_label);
    Id read = filtered;
    if (upscale.skip_flat) {
        const std::array<Id, 4> incoming{filtered, full_label, bilinear, easu_label};
        read = OpPhi(f4, incoming);
    }
    OpBranch(merge_label);

    AddLabel(merge_label);
    const std::array<Id, 4> incoming{read, easu_merge_label, bilinear, entry_label};
    OpReturnValue(OpPhi(f4, incoming));
    OpFunctionEnd();
    return func;
}

} // namespace Shader::Backend::SPIRV
