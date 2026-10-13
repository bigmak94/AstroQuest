// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

/// The scene parameters of ASTRO BOT Rescue Mission: XML files (data/multi_platformer/spx/*.spx)
/// that the title reads when a level, or a part of one, comes up. One file holds what every
/// scene drawn for the headset starts from (default_hmd.spx), the others what a level changes
/// of it. Among much else they say at which distances the title's models change to their
/// coarser forms (<Lod>, <Distance0> to <Distance3>: the full form up to the first, the
/// coarsest beyond the third, nothing beyond the last), how large the sun's shadow maps are
/// (<DirShadowMapSize>) and whether the scene's depth is drawn on its own before its colours
/// (<DepthPrePassEnable>, on unless a file says otherwise).
///
/// On a GPU slower than the console's the same scene can be drawn with less work by having
/// models change to their coarser forms nearer by: what the emulator does with these files
/// when asked (KnownTitle::SceneFile). Nothing of the title's files is kept or shipped: the
/// title's own file is read and handed on changed.
namespace Core::KnownTitle::Scenes {

struct Detail {
    /// The distances at which models change to coarser forms, as a share of the title's own.
    /// The last distance, beyond which a model is not drawn at all, stays the title's.
    double lod_scale{1.0};
    /// The largest size of the sun's shadow maps (1024 or 1536), 0 to leave the title's.
    int shadow_map_size{0};
    /// false turns the title's depth pre-pass off.
    bool depth_prepass{true};
    /// How far away (metres) models still get the depth pre-pass, and how small they may be
    /// and still get it; below zero the title's own (the headset's base file sets neither, the
    /// television's has 10 and 0).
    double prepass_distance{-1.0};
    double prepass_radius{-1.0};
    /// Which levels whose sun is black get no sun shadow maps: 0 none, 1 those whose sun is
    /// black everywhere (DarkSunFiles), 2 those and 3-2 (INDY CAVE 1), whose sunlit entrance
    /// then loses its cast shadows (a measuring aid).
    int dark_sun{1};

    bool IsTitles() const {
        return lod_scale == 1.0 && shadow_map_size == 0 && depth_prepass && dark_sun == 0 &&
               prepass_distance < 0.0 && prepass_radius < 0.0;
    }
};

/// The scene parameter files of levels whose every directional light (the sun), switched on
/// or not, has a strength (intensity times its colour) of 0 or at most a hundredth: their
/// 2048x2048 sun shadow maps (a tenth of a frame's GPU time in a level like 3-2) shade
/// nothing that could be seen. Read from the levels' design.lvx (art_light_directional) on
/// 2026-10-09: FOREST CASTLE (forest_castle_a), INDY CAVE 2 (indycave2), GRAVEYARD
/// (graveyard_a), the spider boss (spider_boss_setings, two levels), and the challenges with
/// suns of 0.005 and 0.01 (challenge_light, challenge_watershooting). Left alone: CAVE &
/// CANNONS' file (trap_cave), which CANNON MANIA shares with a sun of 0.05 and of 1 at its goal;
/// the whale's (sea_whale_low), whose level switches a sun of 4 on; LAVA RUN's
/// (challenge_lava_run, a sun of 0.1); VOLCANO's (trap_lava), shared with FIRE TRAPS' lit one.
constexpr std::string_view DarkSunFiles[] = {
    "forest_castle_a.spx", "indycave2.spx",       "graveyard_a.spx",
    "spider_boss_setings.spx", "challenge_light.spx", "challenge_watershooting.spx",
};
constexpr std::string_view MixedSunFiles[] = {"indycave1.spx"};

namespace Detail_ {

/// Calls `change(value)` for the text between every <tag> and the </ that follows it, and puts
/// what it returns in its place.
template <typename Change>
std::string ReplaceValues(std::string text, std::string_view tag, Change&& change) {
    const std::string open = "<" + std::string{tag} + ">";
    size_t at = 0;
    while ((at = text.find(open, at)) != std::string::npos) {
        const size_t begin = at + open.size();
        const size_t end = text.find("</", begin);
        if (end == std::string::npos) {
            break;
        }
        const std::string value = change(std::string_view{text}.substr(begin, end - begin));
        text.replace(begin, end - begin, value);
        at = begin + value.size();
    }
    return text;
}

inline std::string FormatNumber(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

/// The indentation and line end of the file's settings.
inline std::pair<std::string, std::string> LineStyle(const std::string& text, size_t at) {
    const bool crlf = text.find("\r\n") != std::string::npos;
    std::string indent;
    const size_t line = text.rfind('\n', at);
    if (line != std::string::npos) {
        for (size_t i = line + 1; i < text.size() && (text[i] == ' ' || text[i] == '\t'); ++i) {
            indent += text[i];
        }
    }
    return {indent.empty() ? std::string{"  "} : indent, crlf ? "\r\n" : "\n"};
}

/// `text` with <tag> set to `value`: replaced where the file has it, added at the start of
/// <SceneParameter> where it does not (a file that is only "<SceneParameter />" is opened up).
inline std::string SetValue(std::string text, std::string_view tag, std::string_view value) {
    const std::string open = "<" + std::string{tag} + ">";
    if (text.find(open) != std::string::npos) {
        return ReplaceValues(std::move(text), tag,
                             [&](std::string_view) { return std::string{value}; });
    }
    const std::string element = open + std::string{value} + "</" + std::string{tag} + ">";
    const std::string_view empty = "<SceneParameter />";
    if (const size_t at = text.find(empty); at != std::string::npos) {
        const bool crlf = text.find("\r\n") != std::string::npos;
        const std::string end = crlf ? "\r\n" : "\n";
        text.replace(at, empty.size(),
                     "<SceneParameter>" + end + "  " + element + end + "</SceneParameter>");
        return text;
    }
    const std::string_view start = "<SceneParameter>";
    const size_t at = text.find(start);
    if (at == std::string::npos) {
        return text;
    }
    // Before the first setting, with its indentation.
    const size_t first = text.find('<', at + start.size());
    const auto [indent, end] = LineStyle(text, first != std::string::npos ? first : at);
    text.insert(at + start.size(), end + indent + element);
    return text;
}

} // namespace Detail_

/// Whether `name` (a scene parameter file's name) is one whose sun shadows `detail` leaves out.
inline bool LeavesSunShadowsOut(const Detail& detail, std::string_view name) {
    const auto listed = [&](const auto& files) {
        return std::ranges::find(files, name) != std::end(files);
    };
    return (detail.dark_sun >= 1 && listed(DarkSunFiles)) ||
           (detail.dark_sun >= 2 && listed(MixedSunFiles));
}

/// `text` (a scene parameter file) with `detail` applied. `base` is true for the file every
/// scene drawn for the headset starts from (default_hmd.spx): a setting that the title leaves
/// to its default is added there to be changed. `name` is the file's name.
inline std::string Adjust(std::string_view text, const Detail& detail, bool base,
                          std::string_view name = {}) {
    using namespace Detail_;
    std::string result{text};
    if (LeavesSunShadowsOut(detail, name)) {
        // What the developers did themselves for the demo disc's world select in the
        // title's update (demodisk.spx): the shadows' reach cut to a centimetre, small maps.
        // (A reach of 0, as in the whale's inside, means no limit to the title: in 3-2 that
        // drew more into the maps than before.)
        result = SetValue(std::move(result), "DirShadowFar", "0.01");
        result = SetValue(std::move(result), "DirShadowMapSize", "1024");
    }
    if (detail.lod_scale != 1.0) {
        for (const std::string_view tag : {"Distance0", "Distance1", "Distance2"}) {
            result = ReplaceValues(std::move(result), tag, [&](std::string_view value) {
                const std::string number{value};
                char* end = nullptr;
                const double distance = std::strtod(number.c_str(), &end);
                if (end == number.c_str()) {
                    return number;
                }
                return FormatNumber(distance * detail.lod_scale);
            });
        }
    }
    if (detail.shadow_map_size != 0) {
        result = ReplaceValues(std::move(result), "DirShadowMapSize", [&](std::string_view value) {
            const int size = std::atoi(std::string{value}.c_str());
            return size > detail.shadow_map_size ? std::to_string(detail.shadow_map_size)
                                                 : std::string{value};
        });
    }
    if (base && detail.prepass_distance >= 0.0) {
        result = SetValue(std::move(result), "DepthPrePassMaxDistance",
                          FormatNumber(detail.prepass_distance));
    }
    if (base && detail.prepass_radius >= 0.0) {
        result = SetValue(std::move(result), "DepthPrePassMinRadius",
                          FormatNumber(detail.prepass_radius));
    }
    if (!detail.depth_prepass) {
        const std::string_view tag = "<DepthPrePassEnable>";
        if (result.find(tag) != std::string::npos) {
            result = ReplaceValues(std::move(result), "DepthPrePassEnable",
                                   [](std::string_view) { return std::string{"false"}; });
        } else if (base) {
            // Where the other settings of the kind are, with the file's own line ends and
            // indentation.
            const std::string_view after = "</SpotShadowMapSize>";
            size_t at = result.find(after);
            at = at != std::string::npos ? at + after.size() : result.find("<SceneParameter>");
            if (at != std::string::npos) {
                if (result.compare(at, 16, "<SceneParameter>") == 0) {
                    at += 16;
                }
                const bool crlf = result.find("\r\n") != std::string::npos;
                const size_t line = result.rfind('\n', at);
                std::string indent;
                if (line != std::string::npos) {
                    for (size_t i = line + 1; i < result.size() &&
                                              (result[i] == ' ' || result[i] == '\t');
                         ++i) {
                        indent += result[i];
                    }
                }
                if (indent.empty()) {
                    indent = "  ";
                }
                result.insert(at, std::string{crlf ? "\r\n" : "\n"} + indent +
                                      "<DepthPrePassEnable>false</DepthPrePassEnable>");
            }
        }
    }
    return result;
}

} // namespace Core::KnownTitle::Scenes
