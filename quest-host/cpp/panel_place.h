// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>

#include "held_pad.h"

/// Where the panel the app shows before the game's first picture (the start-up menu, then what
/// the app says while the emulator starts) stands: still in the room, in front of where the head
/// faces when it is placed. Everything is in the headset's seated space (LOCAL): metres, +X
/// right, +Y up, -Z forward, origin and heading wherever the system's last reset of the view
/// put them.
namespace PanelPlace {

struct Pose {
    HeldPad::Vec position;
    /// Upright and turned about the vertical only: its front (+Z, the side of a quad layer the
    /// compositor shows) faces the head.
    HeldPad::Quat orientation;
    /// The way it was put from the head: 0 straight ahead (-Z), positive to the left, radians.
    float heading{};
};

/// `distance` metres from the head along the ground, the way the face is turned, and `drop`
/// below the eyes. How far the head is tilted up or down, or to a side, does not count: a player
/// who looks at the controller in their lap does not get the panel on the floor. Beyond 87
/// degrees up or down, where the way the face points is lost in the tracking's noise, the way
/// the top of the head (looking down) or the chin (looking up) points is the way the face is
/// turned. An orientation that is no rotation (NaN) puts it straight ahead.
inline Pose InFront(const HeldPad::Vec& head, const HeldPad::Quat& head_orientation,
                    float distance, float drop) {
    HeldPad::Vec ahead = HeldPad::Rotate(head_orientation, {0.0f, 0.0f, -1.0f});
    float along_ground = std::hypot(ahead.x, ahead.z);
    if (!(along_ground >= 0.05f)) {
        ahead = HeldPad::Rotate(head_orientation, {0.0f, ahead.y < 0.0f ? 1.0f : -1.0f, 0.0f});
        along_ground = std::hypot(ahead.x, ahead.z);
    }
    Pose pose;
    pose.heading = along_ground > 1e-3f ? std::atan2(-ahead.x, -ahead.z) : 0.0f;
    pose.orientation = {0.0f, std::sin(0.5f * pose.heading), 0.0f, std::cos(0.5f * pose.heading)};
    pose.position = {head.x - std::sin(pose.heading) * distance, head.y - drop,
                     head.z - std::cos(pose.heading) * distance};
    return pose;
}

} // namespace PanelPlace
