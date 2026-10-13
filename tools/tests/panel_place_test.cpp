// Checks where the Quest app stands the panel it shows before the game's first picture
// (quest-host/cpp/panel_place.h): along the ground the way the face is turned, at the eyes'
// height less the drop, upright and facing the head, whatever the head's tilt and roll.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I quest-host/cpp tools/tests/panel_place_test.cpp
//       /Fe:build/tests/panel_place_test.exe
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "panel_place.h"

using HeldPad::Quat;
using HeldPad::Vec;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

static bool Near(float a, float b, float within = 0.001f) {
    return std::abs(a - b) <= within;
}

static constexpr float Pi = 3.14159265f;

static float Radians(float degrees) {
    return degrees * Pi / 180.0f;
}

static Quat Multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/// A head turned `left` degrees to the left, then tilted `up` degrees up, then rolled `roll`.
static Quat HeadTurned(float left, float up, float roll) {
    const Quat turn{0.0f, std::sin(Radians(left) / 2), 0.0f, std::cos(Radians(left) / 2)};
    const Quat tilt{std::sin(Radians(up) / 2), 0.0f, 0.0f, std::cos(Radians(up) / 2)};
    const Quat lean{0.0f, 0.0f, std::sin(Radians(roll) / 2), std::cos(Radians(roll) / 2)};
    return Multiply(Multiply(turn, tilt), lean);
}

/// The panel for a head turned that way is put `heading` degrees to the left of straight ahead.
static void Expect(float left, float up, float roll, float heading) {
    const Vec head{0.1f, 0.2f, -0.3f};
    const PanelPlace::Pose panel =
        PanelPlace::InFront(head, HeadTurned(left, up, roll), 1.5f, 0.1f);
    char text[96];
    std::snprintf(text, sizeof(text), "head %.0f left, %.0f up, rolled %.0f: ", left, up, roll);
    const std::string name{text};
    const Vec front = HeldPad::Rotate(panel.orientation, {0.0f, 0.0f, 1.0f});
    const Vec top = HeldPad::Rotate(panel.orientation, {0.0f, 1.0f, 0.0f});
    const Vec to_head = HeldPad::Sub(head, panel.position);
    const float along_ground = std::hypot(to_head.x, to_head.z);
    const Quat& q = panel.orientation;
    Check(Near(std::remainder(panel.heading - Radians(heading), 2.0f * Pi), 0.0f),
          name + "put the way the face is turned");
    Check(Near(along_ground, 1.5f), name + "1.5 m away along the ground");
    Check(Near(panel.position.y, head.y - 0.1f), name + "0.1 m below the eyes");
    Check(Near(top.y, 1.0f), name + "upright");
    Check(Near((front.x * to_head.x + front.z * to_head.z) / along_ground, 1.0f),
          name + "its front faces the head");
    Check(Near(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1.0f),
          name + "a rotation");
}

int main() {
    Expect(0, 0, 0, 0);
    Expect(90, 0, 0, 90);
    Expect(-135, 0, 0, -135);
    Expect(180, 0, 0, 180);
    // Looking at the controller in the lap, or up: still ahead at the eyes' height.
    Expect(30, -60, 0, 30);
    Expect(30, 60, 0, 30);
    Expect(45, 40, 25, 45);
    Expect(-20, 80, 15, -20);
    Expect(-20, 86, 15, -20);
    // Straight down or up: the top of the head, or the chin, tells the way.
    Expect(-70, -90, 0, -70);
    Expect(-70, 90, 0, -70);
    Expect(25, -88, 0, 25);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const PanelPlace::Pose lost =
        PanelPlace::InFront({0.0f, 0.0f, 0.0f}, {nan, nan, nan, nan}, 1.5f, 0.1f);
    Check(std::isfinite(lost.position.x) && std::isfinite(lost.position.z) &&
              Near(lost.orientation.w, 1.0f) && Near(lost.position.z, -1.5f),
          "an orientation that is no rotation: straight ahead");

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
