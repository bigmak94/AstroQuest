// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string_view>

#include "common/types.h"

namespace Core::Vr {
struct Vec3;
}

/// What is known about the inner workings of particular titles, and what the emulator does with
/// it. Everything here applies to exact builds of one title, told apart by what their images
/// hold when loaded (known_title_builds.h), and does nothing for any other title or build.
///
/// Astro Bot Rescue Mission (CUSA12392):
///  - It advances its world by one sixtieth of a second for every frame it draws, however long
///    the frame took: where frames take longer than that, on anything slower than the console
///    it was made for, the whole game runs in slow motion (half speed at 30 frames a second).
///    Its engine keeps the step in three places, written by the one function that sets its
///    frame rate, and reads them wherever time passes. The emulator writes them, frame by
///    frame, with what frames really take: the game then runs by the clock.
///  - Its tracking manager keeps the point it counts head positions from, and the position
///    counted from it, which is what a view that is off shows up in.
///  - It moves the collisions of what moves by a speed worked out with one frame's time step
///    and applied for the next frame's: the same thing on the console, not with a time step
///    that follows the frames. Its physics are changed to step with the time step the bodies
///    were moved for (known_title_builds.h, PhysicsStepChanges).
namespace Core::KnownTitle {

/// Called when the title has handed in a frame (sceGnmSubmitDone): sets the title's time step
/// to what its frames take. SHADPS4_TITLE_TIMESTEP=0 leaves the title to its own; a number of
/// frames a second instead of 0 is the slowest rate still made up for (20 unless told).
/// SHADPS4_TITLE_RESOLUTION=<3..6> holds the title to one size for its scene.
/// SHADPS4_VR_PACE=<2..6> gives every frame that many refreshes of the display;
/// SHADPS4_VR_STEADY_FPS=<fps> as many as make that many frames a second, throughout.
/// SHADPS4_VR_LARGEST_SIZE=<3..6>: where the size is chosen here, none larger than that.
void OnFrameSubmitted();

/// How many refreshes of the display a frame of the title is given at the moment, 0 where
/// nothing is known about that. The title waits for two refreshes of its headset before it
/// goes on to the next frame, and goes on at once when a frame took longer: it is then on a
/// pace of its own that the display knows nothing of. Given three, the emulated headset
/// refreshes one and a half times as slowly as the display, and the title keeps in step with
/// the display again, at a frame for every three refreshes.
u32 FramePace();

/// Called with the head position just handed to the title: what the title makes of it goes to
/// the log every now and then, and whenever the title takes stock of the player's seat anew.
void NoteView(const Vr::Vec3& tracker_head);

/// Called when the title reads its controller, which it does once a frame on the thread it
/// runs on. SHADPS4_TITLE_PHYSICS_WATCH=1 has the log tell whether the collisions the title
/// moves arrive where it sends them.
void OnControllerRead();

/// Called once the title is known and before its memory is set up. A title that is to draw
/// larger pictures than on the console (SHADPS4_TITLE_EYE_WIDTH=<pixels>, the width of the
/// largest, 1440 on the console) needs more memory than the console has.
void Prepare();

/// Called when the title's executable is in memory, before any of it runs: the sizes the title
/// draws at, and the memory it sets aside for that, are written into it where they are larger,
/// and its physics are made to take each step with the time step its bodies were moved for
/// (SHADPS4_TITLE_PHYSICS_STEP=0 leaves them as the console has them).
void OnGameLoaded(VAddr base, u64 size);

/// Whether a render area is that of the pictures handed to the headset (the largest size of the
/// list), where the title is known.
bool IsEyeSize(u32 width, u32 height);

/// Whether a render pass of that size draws the title's scene for an eye: the size the title
/// draws that at right now (the coarser shading towards the edges of SHADPS4_VR_FOVEATION is
/// for those passes). False for every other title.
bool IsSceneSize(u32 width, u32 height);

/// Whether a size is one of those the title draws its scene at for the headset (whichever it
/// draws at now), where the title is known.
bool IsHeadsetSceneSize(u32 width, u32 height);

/// Whether a shader program of the title's (its hash) is the pixel shader of its final pass,
/// which enlarges the scene to the picture handed to the headset (and sets its tone and levels):
/// one full-screen draw for each eye's picture, sampling the scene bilinearly (TitleUpscale).
bool IsSceneEnlargement(u64 pgm_hash);

/// Called with the host file a path of the title's has been found at. A scene parameter file
/// of the title (known_title_scenes.h) is read from a copy that says what is to be drawn
/// otherwise than on the console, made from the title's own: by default the sun's shadow maps
/// of the levels whose sun is black are left out (SHADPS4_TITLE_DARK_SUN_SHADOWS=0 keeps them,
/// =2 leaves out 3-2's as well); and as asked, less detail (SHADPS4_TITLE_LOD=<share of the
/// distances at which models change to coarser forms>, SHADPS4_TITLE_SHADOW_MAP=<1024|1536>,
/// SHADPS4_TITLE_DEPTH_PREPASS=0) or the depth pre-pass's reach (SHADPS4_TITLE_PREPASS_DISTANCE,
/// _RADIUS). Any other file, and one with nothing to change, is itself.
std::filesystem::path SceneFile(std::string_view guest_path, std::filesystem::path host_path);

} // namespace Core::KnownTitle
