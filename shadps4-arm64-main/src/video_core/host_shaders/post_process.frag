// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    // Above 0, the picture is sharpened by so much (up to 1): for one that is shown larger
    // than it was drawn, which blurs it.
    float sharpen;
    // The target encodes what it is given for display by itself (an sRGB image): it has to be
    // given linear light, or the picture would be encoded twice.
    bool linear_out;
    // The texture gives back what it holds, already encoded for display (an sRGB image read
    // through a UNORM view, gamma 1): nothing to encode.
    bool encoded_in;
    // Within centre_radius of the middle (a share of the way to the edge, on both axes) the
    // picture is sharpened by sharpen_centre instead: where FSR 1 enlarged the title's scene.
    float sharpen_centre;
    float centre_radius;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// What goes to the display for the picture at a place.
vec3 shown(vec2 at) {
    vec3 rgb = textureLod(texSampler, at, 0.0).rgb;
    return pp.hdr || pp.encoded_in ? rgb : gamma(rgb);
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    vec3 here = pp.hdr || pp.encoded_in ? color_linear.rgb : gamma(color_linear.rgb);
    vec2 from_middle = uv * 2.0 - 1.0;
    float sharpen = dot(from_middle, from_middle) < pp.centre_radius * pp.centre_radius
                        ? pp.sharpen_centre
                        : pp.sharpen;
    if (sharpen > 0.0) {
        // Contrast adaptive sharpening: a pixel is pushed away from the four next to it, the
        // more the less they differ already, and never beyond black or white.
        vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
        vec3 above = shown(uv - vec2(0.0, texel.y));
        vec3 below = shown(uv + vec2(0.0, texel.y));
        vec3 left = shown(uv - vec2(texel.x, 0.0));
        vec3 right = shown(uv + vec2(texel.x, 0.0));
        vec3 darkest = min(min(min(above, below), min(left, right)), here);
        vec3 brightest = max(max(max(above, below), max(left, right)), here);
        vec3 room = sqrt(clamp(min(darkest, 1.0 - brightest) / max(brightest, vec3(1e-5)),
                               0.0, 1.0));
        vec3 weight = room * (-1.0 / mix(8.0, 5.0, clamp(sharpen, 0.0, 1.0)));
        here = clamp(((above + below + left + right) * weight + here) / (1.0 + 4.0 * weight),
                     0.0, 1.0);
    }
    if (pp.linear_out && !pp.hdr) {
        // Back to linear light, by the curve the target encodes with.
        here = mix(pow((here + 0.055) / 1.055, vec3(2.4)), here / 12.92,
                   lessThanEqual(here, vec3(0.04045)));
    }
    color = vec4(here, color_linear.a);
}
