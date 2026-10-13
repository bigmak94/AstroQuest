// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core

// Stands in for the resolve of a multisampled target that the host holds with one sample a
// pixel: there is nothing to average, so edges are smoothed from the picture itself instead,
// the way FXAA does it. A pixel on an edge is found by the contrast with its neighbours; the
// edge is followed both ways to where it ends, and how far along it the pixel sits says how
// much of the pixel across the edge belongs into it.
//
// The picture may hold light far brighter than white (scenes are resolved before they are
// tone mapped), so contrast is judged, and pixels are mixed, in terms that keep one glaring
// pixel from deciding everything around it.

layout (binding = 0, set = 0) uniform sampler2D scene;

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 out_color;

// The least contrast that makes an edge, as a share of the brightest pixel around and as such.
const float EdgeShare = 0.166;
const float EdgeLeast = 0.0625;
// How much a pixel that stands out from all of its neighbours is evened out.
const float Speck = 0.5;
// How far the edge is followed, in pixels a step.
const int Steps = 7;
const float StepLength[Steps] = float[Steps](1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);

float Brightness(vec3 rgb) {
    const float light = min(dot(max(rgb, vec3(0.0)), vec3(0.299, 0.587, 0.114)), 65000.0);
    return sqrt(light / (1.0 + light));
}

float BrightnessAt(vec2 at) {
    return Brightness(textureLod(scene, at, 0.0).rgb);
}

float Peak(vec3 rgb) {
    return min(max(max(rgb.r, rgb.g), max(rgb.b, 0.0)), 65000.0);
}

void main() {
    const vec2 texel = 1.0 / vec2(textureSize(scene, 0));
    const vec4 centre = textureLod(scene, uv, 0.0);
#ifdef FINE_RADIUS
    // Outside the middle of an eye's scene, which foveated rendering shades in blocks of
    // pixels anyway, edges are left as they were drawn: a copy.
    const vec2 from_middle = uv * 2.0 - 1.0;
    if (dot(from_middle, from_middle) >= FINE_RADIUS * FINE_RADIUS) {
        out_color = centre;
        return;
    }
#endif
    const float m = Brightness(centre.rgb);
    const float n = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(0, -1)).rgb);
    const float s = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(0, 1)).rgb);
    const float w = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(-1, 0)).rgb);
    const float e = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(1, 0)).rgb);
    const float brightest = max(max(max(n, s), max(w, e)), m);
    const float darkest = min(min(min(n, s), min(w, e)), m);
    const float contrast = brightest - darkest;
    if (contrast < max(EdgeLeast, brightest * EdgeShare)) {
        out_color = centre;
        return;
    }
    const float nw = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(-1, -1)).rgb);
    const float ne = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(1, -1)).rgb);
    const float sw = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(-1, 1)).rgb);
    const float se = Brightness(textureLodOffset(scene, uv, 0.0, ivec2(1, 1)).rgb);

    // Which way the edge runs: along the rows if brightness changes more from row to row.
    const float across_rows = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) +
                              abs(ne + se - 2.0 * e);
    const float across_columns = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) +
                                 abs(sw + se - 2.0 * s);
    const bool along_rows = across_rows >= across_columns;
    // The neighbours on either side of the edge, and the one the pixel differs from more.
    const float before = along_rows ? n : w;
    const float after = along_rows ? s : e;
    const float rise_before = abs(before - m);
    const float rise_after = abs(after - m);
    const bool steeper_before = rise_before >= rise_after;
    const float rise = max(rise_before, rise_after) * 0.25;
    const float side = steeper_before ? -1.0 : 1.0;
    const float middle = 0.5 * ((steeper_before ? before : after) + m);
    const bool centre_below = m < middle;

    // From half a pixel towards that neighbour, where both sides of the edge are in what is
    // read, along the edge to where it stops being one.
    const vec2 across = along_rows ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    const vec2 along = along_rows ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    const vec2 start = uv + across * (side * 0.5);
    float back = 0.0;
    float forth = 0.0;
    float end_back = 0.0;
    float end_forth = 0.0;
    bool done_back = false;
    bool done_forth = false;
    for (int i = 0; i < Steps; ++i) {
        if (!done_back) {
            back += StepLength[i];
            end_back = BrightnessAt(start - along * back) - middle;
            done_back = abs(end_back) >= rise;
        }
        if (!done_forth) {
            forth += StepLength[i];
            end_forth = BrightnessAt(start + along * forth) - middle;
            done_forth = abs(end_forth) >= rise;
        }
        if (done_back && done_forth) {
            break;
        }
    }
    // The nearer end says how much of the neighbour belongs here, if the edge ends there the
    // way that makes this pixel the one that sticks out.
    const bool nearer_back = back < forth;
    const float nearer = min(back, forth);
    const bool sticks_out = ((nearer_back ? end_back : end_forth) < 0.0) != centre_below;
    const float by_edge = sticks_out ? 0.5 - nearer / (back + forth) : 0.0;

    // A pixel unlike everything around it is evened out a little whatever the edge says.
    const float around = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;
    float speck = clamp(abs(around - m) / contrast, 0.0, 1.0);
    speck = speck * speck * (3.0 - 2.0 * speck);
    const float share = max(by_edge, speck * speck * Speck);

    const vec4 other = textureLod(scene, uv + across * side, 0.0);
    const float weight_centre = (1.0 - share) / (1.0 + Peak(centre.rgb));
    const float weight_other = share / (1.0 + Peak(other.rgb));
    out_color = (centre * weight_centre + other * weight_other) / (weight_centre + weight_other);
}
