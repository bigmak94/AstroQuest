// Checks what the emulator makes of ASTRO BOT Rescue Mission's scene parameter files when it is
// to draw with less detail (Core::KnownTitle::Scenes::Adjust).
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src
//       tools/tests/known_title_scenes_test.cpp /Fe:build/tests/known_title_scenes_test.exe
//   build/tests/known_title_scenes_test.exe [scene parameter file ...]
//
// Without arguments it works on texts made up here, laid out as the title's are. Given files,
// it also shows what would be made of them.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/known_title_scenes.h"

using namespace Core::KnownTitle::Scenes;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

static bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int main(int argc, char** argv) {
    const std::string base_text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n\n<SceneParameter>\n"
        "  <DirShadowFar>10.0</DirShadowFar>\n"
        "  <DirShadowMapSize>2048</DirShadowMapSize>\n"
        "  <SpotShadowMapSize>1024</SpotShadowMapSize>\n"
        "  <Lod>\n    <Type>Distance</Type>\n"
        "    <Distance0>15.0</Distance0>\n    <Distance1>20.0</Distance1>\n"
        "    <Distance2>25.0</Distance2>\n    <Distance3>1000.0</Distance3>\n  </Lod>\n"
        "  <Pipeline>\n    <HeightFog>\n      <DistanceMax>100.0</DistanceMax>\n"
        "    </HeightFog>\n  </Pipeline>\n</SceneParameter>\n";

    Check(Adjust(base_text, {}, true) == base_text, "nothing asked: the text as it is");

    const std::string third = Adjust(base_text, {.lod_scale = 1.0 / 3.0}, true);
    Check(Has(third, "<Distance0>5.000</Distance0>") && Has(third, "<Distance1>6.667</Distance1>") &&
              Has(third, "<Distance2>8.333</Distance2>"),
          "the first three distances a third of the title's");
    Check(Has(third, "<Distance3>1000.0</Distance3>"), "the last distance the title's");
    Check(Has(third, "<DistanceMax>100.0</DistanceMax>"), "other distances untouched");
    Check(Has(third, "<DirShadowMapSize>2048</DirShadowMapSize>"), "shadows untouched");
    Check(!Has(third, "DepthPrePass"), "the depth pre-pass untouched");

    const std::string shadows = Adjust(base_text, {.shadow_map_size = 1024}, true);
    Check(Has(shadows, "<DirShadowMapSize>1024</DirShadowMapSize>"), "a smaller shadow map");
    Check(Has(Adjust("<DirShadowMapSize>1024</DirShadowMapSize>", {.shadow_map_size = 1536}, false),
              "<DirShadowMapSize>1024<"),
          "a shadow map no larger than the title's");

    const std::string no_prepass = Adjust(base_text, {.depth_prepass = false}, true);
    Check(Has(no_prepass, "<SpotShadowMapSize>1024</SpotShadowMapSize>\n"
                          "  <DepthPrePassEnable>false</DepthPrePassEnable>\n  <Lod>"),
          "the pre-pass turned off in the base file, with its indentation");
    Check(Adjust("<SceneParameter>\n  <Lod/>\n</SceneParameter>\n", {.depth_prepass = false},
                 false) == "<SceneParameter>\n  <Lod/>\n</SceneParameter>\n",
          "a level's file without the setting keeps the base file's");
    Check(Has(Adjust("<SceneParameter>\r\n\t<DepthPrePassEnable>true</DepthPrePassEnable>\r\n",
                     {.depth_prepass = false}, false),
              "\t<DepthPrePassEnable>false</DepthPrePassEnable>\r\n"),
          "a file's own setting turned off");
    const std::string crlf =
        Adjust("<SceneParameter>\r\n\t<SpotShadowMapSize>512</SpotShadowMapSize>\r\n</SceneParameter>",
               {.depth_prepass = false}, true);
    Check(Has(crlf, "</SpotShadowMapSize>\r\n\t<DepthPrePassEnable>false</DepthPrePassEnable>\r\n"),
          "a file with CR LF line ends keeps them");
    Check(Has(Adjust("<SceneParameter>\n</SceneParameter>", {.depth_prepass = false}, true),
              "<SceneParameter>\n  <DepthPrePassEnable>false</DepthPrePassEnable>\n"),
          "a base file without shadow settings gets it after its first line");

    Check(Has(Adjust("<Distance0>0.0</Distance0>", {.lod_scale = 0.5}, false),
              "<Distance0>0.000</Distance0>"),
          "a distance of nothing stays nothing");
    Check(Adjust("<Distance0>near</Distance0>", {.lod_scale = 0.5}, false) ==
              "<Distance0>near</Distance0>",
          "what is not a number is left alone");
    Check(Adjust("<Distance0>15.0", {.lod_scale = 0.5}, false) == "<Distance0>15.0",
          "a tag that is never closed is left alone");

    // The sun's shadow maps where a level's sun is black.
    const std::string dark = Adjust(base_text, {}, false, "forest_castle_a.spx");
    Check(Has(dark, "<DirShadowFar>0.01</DirShadowFar>") &&
              Has(dark, "<DirShadowMapSize>1024</DirShadowMapSize>") &&
              !Has(dark, "DirShadowLightRange"),
          "a dark level's sun shadows: a centimetre's reach, small maps");
    Check(Has(Adjust("<SceneParameter>\n  <Lod/>\n</SceneParameter>", {}, false, "forest_castle_a.spx"),
              "<SceneParameter>\n  <DirShadowMapSize>1024</DirShadowMapSize>\n"),
          "a setting the file lacks is added at its start, with its indentation");
    Check(Adjust(base_text, {.dark_sun = 0}, false, "forest_castle_a.spx") == base_text,
          "a dark level's sun shadows kept when asked");
    Check(Adjust(base_text, {}, false, "indycave1.spx") == base_text &&
              Has(Adjust(base_text, {.dark_sun = 2}, false, "indycave1.spx"),
                  "<DirShadowFar>0.01</DirShadowFar>"),
          "3-2's sun shadows only left out when asked for (its entrance is sunlit)");
    Check(Adjust(base_text, {}, false, "default_hmd.spx") == base_text,
          "a lit level's file untouched");
    const std::string empty =
        Adjust("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n\n<SceneParameter />\n", {}, false,
               "challenge_watershooting.spx");
    Check(Has(empty, "<SceneParameter>\n") && Has(empty, "  <DirShadowFar>0.01</DirShadowFar>\n") &&
              Has(empty, "  <DirShadowMapSize>1024</DirShadowMapSize>\n") &&
              Has(empty, "</SceneParameter>") && !Has(empty, "<SceneParameter />"),
          "an empty file opened up for the settings");
    Check(Has(Adjust("<SceneParameter>\r\n\t<Lod/>\r\n</SceneParameter>", {}, false,
                     "graveyard_a.spx"),
              "<SceneParameter>\r\n\t<DirShadow"),
          "a dark level's file with CR LF and tabs keeps them");

    // How far the depth pre-pass reaches: only the headset's base file, which lacks both.
    const std::string reach =
        Adjust(base_text, {.prepass_distance = 10.0, .prepass_radius = 0.5}, true);
    Check(Has(reach, "<DepthPrePassMaxDistance>10.000</DepthPrePassMaxDistance>") &&
              Has(reach, "<DepthPrePassMinRadius>0.500</DepthPrePassMinRadius>"),
          "the depth pre-pass's reach set in the base file");
    Check(Adjust(base_text, {.prepass_distance = 10.0}, false, "default_hmd.spx") ==
              Adjust(base_text, {}, false, "default_hmd.spx"),
          "the depth pre-pass's reach left alone in other files");
    Check(!Detail{.prepass_distance = 10.0}.IsTitles() && Detail{.dark_sun = 0}.IsTitles(),
          "a changed reach is not the title's own");

    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        const std::string name = argv[i];
        const bool base = name.find("default_hmd") != std::string::npos;
        const size_t slash = name.find_last_of("/\\");
        const std::string file_name = slash == std::string::npos ? name : name.substr(slash + 1);
        std::printf("---- %s\n%s\n", argv[i],
                    Adjust(text.str(), {.lod_scale = 1.0 / 3.0, .shadow_map_size = 1024,
                                        .depth_prepass = false},
                           base, file_name)
                        .c_str());
    }

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
