// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/known_title.h"
#include "core/libraries/system/systemservice.h"
#include "core/vr/headset_fov_cache.h"
#include "core/vr/vr_host_link.h"
#include "core/vr/vr_runtime.h"
#ifdef ENABLE_OPENXR_HOST
#include "core/vr/openxr_host.h"
#endif

namespace Core::Vr {

Quat Multiply(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

Quat FromYawPitch(float yaw, float pitch) {
    const Quat qy{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const Quat qp{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    return Multiply(qy, qp);
}

Quat FromYawPitchRoll(float yaw, float pitch, float roll) {
    const Quat qr{0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)};
    return Multiply(FromYawPitch(yaw, pitch), qr);
}

Quat YawOnly(const Quat& q) {
    // Which way it faces along the ground. Something that looks steeply up or down hardly
    // faces any way; its right-hand side still points somewhere along the ground then.
    const Vec3 forward = Rotate(q, {0.0f, 0.0f, -1.0f});
    float yaw = std::atan2(-forward.x, -forward.z);
    if (forward.x * forward.x + forward.z * forward.z < 0.25f) {
        const Vec3 right = Rotate(q, {1.0f, 0.0f, 0.0f});
        yaw = std::atan2(-right.z, right.x);
    }
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

namespace {

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// The rotation about `axis` (unit length) by `angle`.
Quat FromAxisAngle(const Vec3& axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

enum class HeadsetMode { Auto, On, Off };

struct FileConfig {
    HeadsetMode mode{HeadsetMode::Auto};
    Config config;
};

bool EnvFlag(const char* name, bool& out) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    out = value[0] != '0';
    return true;
}

std::filesystem::path OwnPadPlacePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr_controller.json";
}

/// How far from the standard place the player's own may be: within reach, and before them.
Vec3 WithinReach(const Vec3& offset) {
    return {std::clamp(offset.x, -0.40f, 0.40f), std::clamp(offset.y, -0.70f, 0.30f),
            std::clamp(offset.z, -0.90f, -0.15f)};
}

FileConfig LoadFileConfig() {
    FileConfig result;
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr.json";
    std::ifstream file{path};
    if (file) {
        const auto json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_discarded()) {
            LOG_ERROR(Core_Vr, "Ignoring malformed {}", path.string());
        } else {
            const std::string mode = json.value("headset", "auto");
            result.mode = mode == "on"    ? HeadsetMode::On
                          : mode == "off" ? HeadsetMode::Off
                                          : HeadsetMode::Auto;
            auto& config = result.config;
            config.ipd = json.value("ipd_mm", config.ipd * 1000.0f) / 1000.0f;
            config.refresh_rate = json.value("refresh_rate", config.refresh_rate);
            config.demo_motion = json.value("demo_motion", config.demo_motion);
            if (const auto it = json.find("origin_offset");
                it != json.end() && it->is_array() && it->size() == 3) {
                config.origin_offset = {(*it)[0].get<float>(), (*it)[1].get<float>(),
                                        (*it)[2].get<float>()};
            }
            if (const auto it = json.find("pad_offset");
                it != json.end() && it->is_array() && it->size() == 3) {
                config.pad_offset = {(*it)[0].get<float>(), (*it)[1].get<float>(),
                                     (*it)[2].get<float>()};
            }
            config.pad_follows_head = json.value("pad_anchor", "head") != "seat";
            if (const auto it = json.find("fov_tan");
                it != json.end() && it->is_array() && it->size() == 4) {
                config.fov = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(),
                              (*it)[3].get<float>()};
            }
        }
    }

    bool flag = false;
    if (EnvFlag("SHADPS4_VR", flag)) {
        result.mode = flag ? HeadsetMode::On : HeadsetMode::Off;
    }
    if (EnvFlag("SHADPS4_VR_DEMO", flag)) {
        result.config.demo_motion = flag;
    }
    if (const char* anchor = std::getenv("SHADPS4_VR_PAD_ANCHOR"); anchor != nullptr) {
        result.config.pad_follows_head = std::string_view{anchor} != "seat";
    }
    // A host whose display refreshes at another rate than the headset the title was made for
    // says so: the title then sees refreshes come at that rate, and has its frames follow them
    // the way it would have with the real thing (it takes two refreshes for one frame).
    if (const char* rate = std::getenv("SHADPS4_VR_REFRESH_RATE"); rate != nullptr) {
        result.config.refresh_rate = static_cast<u32>(std::atoi(rate));
    }
    if (result.config.refresh_rate < 60 || result.config.refresh_rate > 120) {
        result.config.refresh_rate = 120;
    }
    // SHADPS4_VR_FOV=<percent>: how much of a PlayStation VR's field of view (100 by 103
    // degrees an eye) the title is told its headset has. It draws that much into the same
    // picture, and the headset shows it over that much: fewer degrees for the same pixels is
    // a sharper picture, with a border around it where the rest would have been.
    // SHADPS4_VR_FOV_OF=headset: the percent is of what the host's headset shows (its whole
    // view at 100), not of a PlayStation VR's.
    if (const char* of = std::getenv("SHADPS4_VR_FOV_OF"); of != nullptr) {
        result.config.fov_from_headset = std::string_view{of} == "headset";
    }
    if (const char* symmetric = std::getenv("SHADPS4_VR_FOV_SYMMETRIC"); symmetric != nullptr) {
        result.config.fov_symmetric = std::string_view{symmetric} == "1";
    }
    if (const char* fov = std::getenv("SHADPS4_VR_FOV"); fov != nullptr && *fov != '\0') {
        result.config.fov_scale =
            std::clamp(static_cast<float>(std::atof(fov)) / 100.0f, 0.5f, 1.2f);
    }
    const float scale = result.config.fov_scale;
    result.config.fov.tan_out *= scale;
    result.config.fov.tan_in *= scale;
    result.config.fov.tan_top *= scale;
    result.config.fov.tan_bottom *= scale;
    return result;
}

} // namespace

Runtime::Runtime() = default;

Runtime& Runtime::Instance() {
    static Runtime instance;
    return instance;
}

void Runtime::Configure(bool psvr_supported, bool psvr_required) {
    const FileConfig file_config = LoadFileConfig();
    config = file_config.config;
    if (std::ifstream file{OwnPadPlacePath()}; file) {
        const auto json = nlohmann::json::parse(file, nullptr, false);
        if (const auto it = json.is_object() ? json.find("own_place") : json.end();
            it != json.end() && it->is_array() && it->size() == 3 && (*it)[0].is_number() &&
            (*it)[1].is_number() && (*it)[2].is_number()) {
            own_pad_offset = WithinReach(
                {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()});
        }
    }
    // An external host can measure its headset before starting this process. Seed the same
    // optics path the PC OpenXR host uses, so the title's first FOV query cannot race IPC or
    // accidentally fall back to a PlayStation VR's projection on the first launch.
    if (const char* optics = std::getenv("SHADPS4_VR_HEADSET_FOV_TAN"); optics != nullptr) {
        Fov fov{};
        char trailing{};
        const int count = std::sscanf(optics, "%f,%f,%f,%f %c", &fov.tan_out, &fov.tan_in,
                                      &fov.tan_top, &fov.tan_bottom, &trailing);
        const auto usable = [](float value) {
            return std::isfinite(value) && value > 0.1f && value < 10.0f;
        };
        if (count == 4 && usable(fov.tan_out) && usable(fov.tan_in) && usable(fov.tan_top) &&
            usable(fov.tan_bottom)) {
            NoteHeadsetFov(fov);
        } else {
            LOG_WARNING(Core_Vr, "Ignoring invalid SHADPS4_VR_HEADSET_FOV_TAN");
        }
    }
    switch (file_config.mode) {
    case HeadsetMode::On:
        config.headset_connected = true;
        break;
    case HeadsetMode::Off:
        config.headset_connected = false;
        break;
    case HeadsetMode::Auto:
        // Titles that merely support PSVR boot fine flat, so only plug the headset in by
        // default when the title cannot run without one.
        config.headset_connected = psvr_required;
        break;
    }
    // An application driving a real headset on the other end of SHADPS4_VR_SOCKET is as
    // connected as a headset gets.
    if (file_config.mode != HeadsetMode::Off && HostLink::Instance().Start()) {
        config.headset_connected = true;
    }
#ifdef ENABLE_OPENXR_HOST
    // So is the headset of the machine itself, for a title that can do something with one. A
    // title that can do without only gets it if it is there already: one that comes later
    // would change what the title is in the middle of it.
    else if (file_config.mode != HeadsetMode::Off && (psvr_supported || psvr_required) &&
             OpenXrHost::Instance().Connect() &&
             (config.headset_connected || OpenXrHost::Instance().HasHeadset())) {
        config.headset_connected = true;
    }
#endif
    LOG_INFO(Core_Vr,
             "Virtual headset {} (title supports PSVR: {}, requires PSVR: {}), refresh {} Hz, "
             "ipd {:.1f} mm, demo motion {}",
             config.headset_connected ? "connected" : "not connected", psvr_supported,
             psvr_required, config.refresh_rate, config.ipd * 1000.0f, config.demo_motion);
}

Vec3 Runtime::PositionToTracker(const Vec3& host) const {
    const Vec3 seated = Rotate(Conjugate(seat_yaw), {host.x - seat_position.x,
                                                     host.y - seat_position.y,
                                                     host.z - seat_position.z});
    return {seated.x + config.origin_offset.x, seated.y + config.origin_offset.y,
            seated.z + config.origin_offset.z};
}

Vec3 Runtime::DirectionToTracker(const Vec3& host) const {
    return Rotate(Conjugate(seat_yaw), host);
}

void Runtime::PlaceHead() {
    const u64 sequence = head.sequence + 1;
    head = host_head;
    head.pose.position = PositionToTracker(host_head.pose.position);
    head.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), host_head.pose.orientation));
    head.linear_velocity = DirectionToTracker(host_head.linear_velocity);
    head.angular_velocity = DirectionToTracker(host_head.angular_velocity);
    head.sequence = sequence;
    head.tracked = true;
}

void Runtime::RecenterSeatLocked() {
    if (!host_head.tracked) {
        return;
    }
    const Quat yaw = YawOnly(host_head.pose.orientation);
    if (seat_valid && pad_attitude_valid) {
        // The controller keeps pointing where it points: its heading is counted from the new
        // straight ahead.
        const Quat turned = Multiply(Conjugate(yaw), seat_yaw);
        pad_attitude = Normalize(Multiply(turned, pad_attitude));
        pad.pose.orientation = pad_attitude;
    }
    if (view_turn != 0.0f && !pad_attitude_valid && !pad_position_tracked) {
        // A controller that nothing knows anything of was turned with the player (TurnView):
        // it points straight ahead again, as they face.
        pad.pose.orientation = {};
    }
    previous_seat_position = seat_position;
    previous_seat_yaw = seat_yaw;
    seat_changed = std::chrono::steady_clock::now();
    seat_position = host_head.pose.position;
    seat_yaw = yaw;
    seat_valid = true;
    view_turn = 0.0f;
    PlaceHead();
    // Where the controller was seen is of the old seat; the host says where it is every frame.
    pad_seen = false;
    pad_seen_offset_valid = false;
    pad_anchor_valid = false;
    pad_shown_valid = false;
    pad_yaw_reference_valid = false;
    LOG_INFO(Core_Vr,
             "Seat recentred: the head rests at {:.2f} {:.2f} {:.2f} of the headset's space, "
             "facing {:.0f} degrees to the left of its straight ahead",
             seat_position.x, seat_position.y, seat_position.z,
             2.0f * std::atan2(seat_yaw.y, seat_yaw.w) * 57.29578f);
}

void Runtime::RecenterSeat() {
    std::scoped_lock lock{mutex};
    RecenterSeatLocked();
}

void Runtime::FixSeat() {
    std::scoped_lock lock{mutex};
    seat_position = {};
    seat_yaw = {};
    seat_valid = true;
    title_asked = true;
}

void Runtime::TurnView(int steps) {
    static const float step = [] {
        float degrees = 30.0f;
        if (const char* value = std::getenv("SHADPS4_VR_TURN"); value != nullptr && *value != '\0') {
            degrees = std::clamp(static_cast<float>(std::atof(value)), 0.0f, 90.0f);
        }
        return degrees / 57.29578f;
    }();
    if (steps == 0 || step == 0.0f) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!seat_valid || !host_head.tracked) {
        return;
    }
    // (Angles about +Y count to the left.)
    const float left = -step * static_cast<float>(steps);
    // What the title sees of the player turns by this; the seat, in the headset's own
    // space, by as much the other way.
    const Quat turn = FromAxisAngle({0.0f, 1.0f, 0.0f}, left);
    const Quat seat_turn = Conjugate(turn);
    previous_seat_position = seat_position;
    previous_seat_yaw = seat_yaw;
    seat_changed = std::chrono::steady_clock::now();
    // The seat turns about the head: the head stays where it is in the title's world.
    const Vec3& at = host_head.pose.position;
    const Vec3 from_seat = Rotate(
        seat_turn, {at.x - seat_position.x, at.y - seat_position.y, at.z - seat_position.z});
    seat_position = {at.x - from_seat.x, at.y - from_seat.y, at.z - from_seat.z};
    seat_yaw = Normalize(Multiply(seat_turn, seat_yaw));
    view_turn += left;
    PlaceHead();
    // The controller is in the player's hands and turns with them. One that a host locates is
    // placed anew with every pose; one that is not keeps its place before the player (see
    // GetPad) and, by its own sensors, the way it points from there.
    if (pad_attitude_valid) {
        pad_attitude = Normalize(Multiply(turn, pad_attitude));
    }
    if (!pad_position_tracked) {
        // (Also one that nothing has ever said anything of: it points straight ahead of the
        // player, whichever way that is.)
        pad.pose.orientation =
            pad_attitude_valid ? pad_attitude : Normalize(Multiply(turn, pad.pose.orientation));
        ++pad.sequence;
    }
    pad_seen_offset = Rotate(turn, pad_seen_offset);
    pad_seen = false;
    pad_shown_valid = false;
    pad_yaw_reference_valid = false;
    LOG_INFO(Core_Vr,
             "The view turns a step to the {}: the head stays at {:.2f} {:.2f} {:.2f} of the "
             "title's space and faces {:.0f} degrees to the left of where it faced when the "
             "view was last reset",
             steps > 0 ? "right" : "left", head.pose.position.x, head.pose.position.y,
             head.pose.position.z, view_turn * 57.29578f);
}

void Runtime::RequestRecenter() {
    RecenterSeat();
    // What the console sends a title when the player asks for the view to be reset: the
    // title then counts positions from where the head is now.
    Libraries::SystemService::OrbisSystemServiceEvent event{};
    event.event_type = Libraries::SystemService::OrbisSystemServiceEventType::ResetVrPosition;
    Libraries::SystemService::PushSystemServiceEvent(event);
}

void Runtime::UpdateHead(const DeviceState& host_state) {
    std::scoped_lock lock{mutex};
    // How fast the head moves, by where it is from one time to the next: for a host that does
    // not say. A title takes a head that pushes into something for a push only when it comes
    // at some speed.
    const auto now = std::chrono::steady_clock::now();
    if (head_seen) {
        const float elapsed = std::chrono::duration<float>(now - head_seen_time).count();
        if (elapsed > 0.002f && elapsed < 0.1f) {
            static constexpr float Blend = 0.5f;
            const Vec3& at = host_state.pose.position;
            head_speed.x += ((at.x - head_seen_position.x) / elapsed - head_speed.x) * Blend;
            head_speed.y += ((at.y - head_seen_position.y) / elapsed - head_speed.y) * Blend;
            head_speed.z += ((at.z - head_seen_position.z) / elapsed - head_speed.z) * Blend;
        } else if (elapsed >= 0.1f) {
            head_speed = {};
        }
    }
    if (!head_seen || now - head_seen_time > std::chrono::milliseconds{2}) {
        head_seen_position = host_state.pose.position;
        head_seen_time = now;
        head_seen = true;
    }
    host_head = host_state;
    if (Length(host_state.linear_velocity) == 0.0f) {
        host_head.linear_velocity = head_speed;
    }
    host_head.tracked = true;
    if (!seat_valid) {
        RecenterSeatLocked();
        return;
    }
    PlaceHead();
}

void Runtime::UpdatePad(const DeviceState& host_state) {
    std::scoped_lock lock{mutex};
    const u64 sequence = pad.sequence + 1;
    pad = host_state;
    pad.pose.position = PositionToTracker(host_state.pose.position);
    pad.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), host_state.pose.orientation));
    pad.linear_velocity = DirectionToTracker(host_state.linear_velocity);
    pad.angular_velocity = DirectionToTracker(host_state.angular_velocity);
    pad.sequence = sequence;
    pad.tracked = true;
    pad_position_tracked = true;
    pad_host_pose_time = std::chrono::steady_clock::now();
}

void Runtime::ReleasePad() {
    std::scoped_lock lock{mutex};
    if (!pad_position_tracked) {
        return;
    }
    pad_position_tracked = false;
    // From where it was, it glides to where it is assumed to be; it points the way its own
    // sensors say, or straight ahead if it has none.
    pad_shown_position = pad.pose.position;
    pad_shown_valid = true;
    pad_shown_time = std::chrono::steady_clock::now();
    pad.pose.orientation =
        pad_attitude_valid ? pad_attitude : FromAxisAngle({0.0f, 1.0f, 0.0f}, view_turn);
    pad.linear_velocity = {};
    pad.angular_velocity = {};
    ++pad.sequence;
}

void Runtime::UpdatePadOrientation(const Quat& orientation, const Vec3& angular_velocity) {
    std::scoped_lock lock{mutex};
    pad.pose.orientation = orientation;
    pad.angular_velocity = angular_velocity;
    ++pad.sequence;
    pad.tracked = true;
    pad_position_tracked = false;
}

void Runtime::UpdatePadHeldOrientation(const Quat& host_orientation) {
    std::scoped_lock lock{mutex};
    pad.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), host_orientation));
    pad.angular_velocity = {};
    ++pad.sequence;
    pad.tracked = true;
    pad_position_tracked = false;
}

void Runtime::UpdatePadAcceleration(const Vec3& acceleration) {
    std::scoped_lock lock{mutex};
    pad_acceleration = acceleration;
    pad_acceleration_valid = true;
}

void Runtime::UpdatePadGyro(const Vec3& angular_velocity) {
    static constexpr float Gravity = 9.80665f;
    // How fast the estimate leans towards what the accelerometer calls "up", per second.
    static constexpr float TiltGain = 2.0f;
    static constexpr Vec3 Up{0.0f, 1.0f, 0.0f};

    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    float elapsed = 0.0f;
    if (pad_attitude_valid) {
        elapsed = std::clamp(std::chrono::duration<float>(now - pad_motion_time).count(), 0.0f,
                             0.05f);
    }
    pad_motion_time = now;

    const float gravity = pad_acceleration_valid ? Length(pad_acceleration) : 0.0f;
    // Only trust the accelerometer for the direction of gravity while the controller is held
    // reasonably still.
    const bool steady = gravity > 0.8f * Gravity && gravity < 1.2f * Gravity;
    const Vec3 measured_up = steady ? Vec3{pad_acceleration.x / gravity,
                                           pad_acceleration.y / gravity,
                                           pad_acceleration.z / gravity}
                                    : Up;

    if (!pad_attitude_valid) {
        if (!steady) {
            return;
        }
        // Start level with the horizon and facing straight ahead.
        const Vec3 axis = Cross(measured_up, Up);
        const float sine = Length(axis);
        pad_attitude = sine > 1e-6f ? FromAxisAngle({axis.x / sine, axis.y / sine, axis.z / sine},
                                                    std::atan2(sine, measured_up.y))
                                    : Quat{};
        pad_attitude_valid = true;
    } else {
        // The gyroscope reports the turn in the controller's own frame.
        const float rate = Length(angular_velocity);
        if (rate > 1e-6f) {
            const Vec3 axis{angular_velocity.x / rate, angular_velocity.y / rate,
                            angular_velocity.z / rate};
            pad_attitude = Multiply(pad_attitude, FromAxisAngle(axis, rate * elapsed));
        }
        if (steady) {
            // Its drift in pitch and roll is taken out with gravity; heading has no reference.
            const Vec3 correction = Cross(Rotate(pad_attitude, measured_up), Up);
            const float error = Length(correction);
            if (error > 1e-6f) {
                const Vec3 axis{correction.x / error, correction.y / error, correction.z / error};
                pad_attitude = Multiply(
                    FromAxisAngle(axis, std::asin(std::min(error, 1.0f)) * TiltGain * elapsed),
                    pad_attitude);
            }
        }
        // Heading has no reference in the sensors. When the host can see which way the
        // controller points, lean towards that; not while it points nearly straight up or
        // down, where heading means little.
        if (pad_yaw_reference_valid &&
            now - pad_yaw_reference_time < std::chrono::milliseconds{500}) {
            const Vec3 forward = Rotate(pad_attitude, {0.0f, 0.0f, -1.0f});
            if (std::abs(forward.y) < 0.9f) {
                static constexpr float YawGain = 1.5f;
                static constexpr float Pi = 3.14159265f;
                float error = pad_yaw_reference - std::atan2(-forward.x, -forward.z);
                error -= 2.0f * Pi * std::round(error / (2.0f * Pi));
                pad_attitude = Multiply(
                    FromAxisAngle(Up, error * std::min(YawGain * elapsed, 1.0f)), pad_attitude);
            }
        }
        pad_attitude = Normalize(pad_attitude);
    }

    // A host that says where the controller is and how it is turned (the headset's own
    // controller, standing in for it) is believed over the sensors of a gamepad that is
    // there as well, on a desk: those only keep count of how that one is turned.
    if (pad_position_tracked && now - pad_host_pose_time < std::chrono::milliseconds{200}) {
        return;
    }
    pad.pose.orientation = pad_attitude;
    pad.angular_velocity = Rotate(pad_attitude, angular_velocity);
    ++pad.sequence;
    pad.tracked = true;
    pad_position_tracked = false;
}

void Runtime::ResetPadYaw() {
    std::scoped_lock lock{mutex};
    if (!pad_attitude_valid) {
        return;
    }
    pad_attitude = Normalize(Multiply(Conjugate(YawOnly(pad_attitude)), pad_attitude));
    pad.pose.orientation = pad_attitude;
}

void Runtime::EnableViewGestures(bool enabled) {
    std::scoped_lock lock{gesture_mutex};
    if (enabled && !view_gestures.load(std::memory_order_relaxed)) {
        // A headset that has just come up: the player still has to settle.
        gesture_seat_taken = false;
    }
    view_gestures.store(enabled, std::memory_order_relaxed);
}

void Runtime::NotePadButton(PadButton button, bool pressed) {
    if (!view_gestures.load(std::memory_order_relaxed)) {
        return;
    }
    bool reset_seat = false;
    bool reset_view = false;
    {
        std::scoped_lock lock{gesture_mutex};
        switch (button) {
        case PadButton::Cross:
            if (pressed && !gesture_seat_taken) {
                gesture_seat_taken = true;
                reset_seat = true;
            }
            break;
        case PadButton::Options:
            if (pressed && !gesture_options_down) {
                gesture_options_since = std::chrono::steady_clock::now();
                gesture_options_fired = false;
            }
            gesture_options_down = pressed;
            break;
        case PadButton::Home:
            reset_view = pressed;
            break;
        }
    }
    if (reset_seat) {
        LOG_INFO(Core_Vr, "First press of X: the player's seat is where they are now");
        RequestRecenter();
    }
    if (reset_view) {
        LOG_INFO(Core_Vr, "PS button: view reset");
        RequestRecenter();
        ResetPadYaw();
    }
}

void Runtime::PollViewGestures() {
    if (!view_gestures.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::scoped_lock lock{gesture_mutex};
        if (!gesture_options_down || gesture_options_fired ||
            std::chrono::steady_clock::now() - gesture_options_since <
                std::chrono::milliseconds{1000}) {
            return;
        }
        gesture_options_fired = true;
        // Whoever resets the view has settled.
        gesture_seat_taken = true;
    }
    LOG_INFO(Core_Vr, "OPTIONS held: view reset");
    RequestRecenter();
    ResetPadYaw();
}

void Runtime::UpdatePadPosition(const Vec3& host_position, const Vec3& linear_velocity) {
    std::scoped_lock lock{mutex};
    pad_seen_position = PositionToTracker(host_position);
    pad_seen_velocity = DirectionToTracker(linear_velocity);
    pad_seen = true;
    pad_seen_time = std::chrono::steady_clock::now();
}

void Runtime::ClearPadPosition() {
    std::scoped_lock lock{mutex};
    pad_seen = false;
}

void Runtime::SetPadOffset(const Vec3& offset) {
    std::scoped_lock lock{mutex};
    config.pad_offset = offset;
    pad_seen_offset_valid = false;
}

void Runtime::MoveOwnPadPlace(const Vec3& by) {
    Vec3 place;
    {
        std::scoped_lock lock{mutex};
        own_pad_offset =
            WithinReach({own_pad_offset.x + by.x, own_pad_offset.y + by.y, own_pad_offset.z + by.z});
        own_pad_place = true;
        place = own_pad_offset;
    }
    LOG_INFO(Core_Vr,
             "The controller, while nothing sees where it is, is held to be at the player's own "
             "place: {:.2f} m to the right of the eyes, {:.2f} below and {:.2f} ahead",
             place.x, -place.y, -place.z);
    std::ofstream file{OwnPadPlacePath()};
    file << nlohmann::json{{"own_place", {place.x, place.y, place.z}}}.dump() << "\n";
}

void Runtime::SwitchPadPlace() {
    bool own;
    Vec3 place;
    {
        std::scoped_lock lock{mutex};
        own_pad_place = !own_pad_place;
        own = own_pad_place;
        place = own ? own_pad_offset : config.pad_offset;
    }
    LOG_INFO(Core_Vr,
             "The controller, while nothing sees where it is, is held to be at {} place: {:.2f} "
             "m to the right of the eyes, {:.2f} below and {:.2f} ahead",
             own ? "the player's own" : "the standard", place.x, -place.y, -place.z);
}

void Runtime::UpdatePadYawReference(float yaw) {
    std::scoped_lock lock{mutex};
    // The host counts the heading from its own straight ahead.
    pad_yaw_reference = yaw - 2.0f * std::atan2(seat_yaw.y, seat_yaw.w);
    pad_yaw_reference_valid = true;
    pad_yaw_reference_time = std::chrono::steady_clock::now();
}

void Runtime::SetPadVibration(u8 small_motor, u8 large_motor) {
    std::function<void(const PadFeedback&)> listener;
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        if (pad_feedback.small_motor == small_motor && pad_feedback.large_motor == large_motor) {
            return;
        }
        pad_feedback.small_motor = small_motor;
        pad_feedback.large_motor = large_motor;
        feedback = pad_feedback;
        listener = pad_feedback_listener;
    }
    if (listener) {
        listener(feedback);
    }
}

void Runtime::SetPadLight(u8 red, u8 green, u8 blue) {
    std::function<void(const PadFeedback&)> listener;
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        if (pad_feedback.red == red && pad_feedback.green == green && pad_feedback.blue == blue) {
            return;
        }
        pad_feedback.red = red;
        pad_feedback.green = green;
        pad_feedback.blue = blue;
        feedback = pad_feedback;
        listener = pad_feedback_listener;
    }
    if (listener) {
        listener(feedback);
    }
}

void Runtime::SetPadFeedbackListener(std::function<void(const PadFeedback&)> listener) {
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        pad_feedback_listener = listener;
        feedback = pad_feedback;
    }
    // Whoever starts listening late still learns the current state.
    if (listener) {
        listener(feedback);
    }
}

void Runtime::UpdateOptics(const Fov& fov, float ipd) {
    std::scoped_lock lock{mutex};
    config.fov = fov;
    config.ipd = ipd;
}

void Runtime::SetHeadsetIdentity(const HeadsetIdentity& identity) {
    std::scoped_lock lock{mutex};
    if (headset_identity != identity) {
        headset_identity = identity;
        has_headset_fov = false;
    }
}

namespace {

std::filesystem::path HeadsetFovPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr_headset_fov.json";
}

float Degrees(float tangent) {
    return std::atan(tangent) * 57.29578f;
}

} // namespace

void Runtime::NoteHeadsetFov(const Fov& fov) {
    bool changed;
    HeadsetIdentity identity;
    {
        std::scoped_lock lock{mutex};
        const auto differs = [](float a, float b) { return std::abs(Degrees(a) - Degrees(b)) > 0.5f; };
        changed = !has_headset_fov || differs(fov.tan_out, headset_fov.tan_out) ||
                  differs(fov.tan_in, headset_fov.tan_in) ||
                  differs(fov.tan_top, headset_fov.tan_top) ||
                  differs(fov.tan_bottom, headset_fov.tan_bottom);
        headset_fov = fov;
        has_headset_fov = true;
        identity = headset_identity;
    }
    headset_fov_known.notify_all();
    if (!changed) {
        return;
    }
    LOG_INFO(Core_Vr,
             "The headset shows {:.1f}/{:.1f}/{:.1f}/{:.1f} degrees an eye (out, in, up, down): "
             "{:.0f} by {:.0f}",
             Degrees(fov.tan_out), Degrees(fov.tan_in), Degrees(fov.tan_top),
             Degrees(fov.tan_bottom), Degrees(fov.tan_out) + Degrees(fov.tan_in),
             Degrees(fov.tan_top) + Degrees(fov.tan_bottom));
    if (identity.runtime.empty() || identity.system.empty()) {
        return;
    }
    std::ofstream file{HeadsetFovPath()};
    file << nlohmann::json{{"runtime", identity.runtime},
                          {"system", identity.system},
                          {"vendor_id", identity.vendor_id},
                          {"fov_tan", {fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom}}}
                .dump()
         << "\n";
}

Fov Runtime::TitleFov() {
    if (!config.fov_from_headset) {
        std::scoped_lock lock{mutex};
        return config.fov;
    }
    // The title asks once, when it opens the headset: what it is told is what it draws for
    // the rest of the session. The host learns what its headset shows once its session runs.
    // (Only for a headset that is there: a game on the monitor has nothing to wait for.)
    bool headset_there = false;
#ifdef ENABLE_OPENXR_HOST
    headset_there = OpenXrHost::Instance().IsAvailable();
#endif
    const auto longest_wait = headset_there ? std::chrono::seconds{10} : std::chrono::seconds{0};
    Fov base{};
    const char* from = "the headset's own";
    {
        std::unique_lock lock{mutex};
        if (!headset_fov_known.wait_for(lock, longest_wait, [&] { return has_headset_fov; })) {
            from = nullptr;
        } else {
            base = headset_fov;
        }
    }
    if (from == nullptr) {
        std::ifstream file{HeadsetFovPath()};
        const auto json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json{};
        std::scoped_lock lock{mutex};
        if (has_headset_fov) {
            base = headset_fov;
            from = "the headset's own";
        } else if (const auto cached = CachedHeadsetFov(json, headset_identity)) {
            base = *cached;
            from = "the headset's, as it was the last time (it has not said yet)";
        } else {
            from = "a PlayStation VR's (the headset has not said what it shows)";
        }
    }
    const float scale = config.fov_scale;
    // A build of a known title that leaves effects out of an eye whose view reaches further to
    // one side than to the other (issue #18) is told the further of the two for both: it draws
    // each eye wider than the headset shows on its inner side, and the headset shows the part
    // it has. (Up and down are the same for both eyes, and stay as they are.)
    bool evened = false;
    const float headset_out_in_sum = base.tan_out + base.tan_in;
    if (!config.fov_symmetric && Core::KnownTitle::WantsEvenView() &&
        std::abs(base.tan_out - base.tan_in) > 0.05f * std::max(base.tan_out, base.tan_in)) {
        base.tan_out = base.tan_in = std::max(base.tan_out, base.tan_in);
        evened = true;
    }
    if (config.fov_symmetric) {
        // Scaling asymmetric per-eye bounds also shrinks the binocular overlap: directions
        // visible on one eye's temple side become black on the other's nose side. Render an
        // encompassing, centred frustum in both eyes instead. At 100% it covers the entire
        // real headset; the compositor clips the extra area to the actual lens bounds.
        base.tan_out = base.tan_in = std::max(base.tan_out, base.tan_in);
        base.tan_top = base.tan_bottom = std::max(base.tan_top, base.tan_bottom);
    }
    const Fov fov{base.tan_out * scale, base.tan_in * scale, base.tan_top * scale,
                  base.tan_bottom * scale};
    {
        std::scoped_lock lock{mutex};
        config.fov = fov;
    }
    LOG_INFO(Core_Vr,
             "The title draws {:.0f}% of {}{}: {:.1f}/{:.1f}/{:.1f}/{:.1f} degrees an eye (out, in, "
             "up, down)",
             scale * 100.0f, config.fov_symmetric ? "the symmetric render envelope of " : "", from,
             Degrees(fov.tan_out), Degrees(fov.tan_in), Degrees(fov.tan_top),
             Degrees(fov.tan_bottom));
    if (evened) {
        LOG_INFO(Core_Vr,
                 "This version of the title leaves effects out of an eye whose view is wider to "
                 "one side: it is told the wider side for both, and draws {:.0f}% as wide as the "
                 "headset shows (the last update of the title does not need this)",
                 100.0f * 2.0f * fov.tan_out / (headset_out_in_sum * scale));
    }
    return fov;
}

void Runtime::NoteDisplayRefresh(float rate, u64 time) {
    // SHADPS4_VR_FOLLOW_DISPLAY=0: the emulated headset keeps to its own clock.
    static const bool follow = [] {
        const char* value = std::getenv("SHADPS4_VR_FOLLOW_DISPLAY");
        return value == nullptr || value[0] != '0';
    }();
    if (!follow) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    // A time that is not of this clock, or not of this moment, is no use.
    const Clock::time_point told{std::chrono::nanoseconds{static_cast<s64>(time)}};
    const bool usable = time != 0 && told <= now && now - told < std::chrono::milliseconds{100};
    std::scoped_lock lock{mutex};
    ++display_refresh.sequence;
    display_refresh.time = usable ? told : now;
    display_refresh.rate = rate;
}

Runtime::DisplayRefresh Runtime::GetDisplayRefresh() const {
    std::scoped_lock lock{mutex};
    return display_refresh;
}

float Runtime::HeadsetRefreshRate() const {
    std::scoped_lock lock{mutex};
    if (display_refresh.sequence != 0 && display_refresh.rate > 30.0f &&
        std::chrono::steady_clock::now() - display_refresh.time < std::chrono::milliseconds{500}) {
        return display_refresh.rate;
    }
    return static_cast<float>(config.refresh_rate);
}

DeviceState Runtime::DemoHead() const {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const float t = duration<float>(steady_clock::now() - start).count();

    DeviceState state;
    const float yaw = config.demo_motion ? 0.45f * std::sin(t * 0.50f) : 0.0f;
    const float pitch = config.demo_motion ? 0.20f * std::sin(t * 0.31f) : 0.0f;
    state.pose.orientation = FromYawPitch(yaw, pitch);
    state.pose.position = config.origin_offset;
    if (config.demo_motion) {
        state.pose.position.x += 0.10f * std::sin(t * 0.23f);
        state.angular_velocity = {0.20f * 0.31f * std::cos(t * 0.31f),
                                  0.45f * 0.50f * std::cos(t * 0.50f), 0.0f};
        state.linear_velocity = {0.10f * 0.23f * std::cos(t * 0.23f), 0.0f, 0.0f};
    }
    state.tracked = true;
    return state;
}

DeviceState Runtime::GetHead() {
    std::scoped_lock lock{mutex};
    if (head.sequence == 0) {
        return DemoHead();
    }
    if (!title_asked) {
        // A title takes where the head is when it first looks for the place the player sits
        // at, and that is some time after the headset was put on.
        title_asked = true;
        RecenterSeatLocked();
    }
    return head;
}

DeviceState Runtime::GetPad() {
    std::scoped_lock lock{mutex};
    if (pad.sequence != 0 && pad_position_tracked) {
        return pad;
    }

    // Nothing locates the controller, so hold it at a fixed reach. Hanging it off the head
    // position (but not the head rotation, hands do not swing around when the player looks
    // about) keeps it in front of the player wherever they sit; the smoothing stops it from
    // mirroring every nod.
    const DeviceState current_head = head.sequence == 0 ? DemoHead() : head;
    const Vec3 target = config.pad_follows_head ? current_head.pose.position : config.origin_offset;
    const auto now = std::chrono::steady_clock::now();
    if (!pad_anchor_valid) {
        pad_anchor = target;
        pad_anchor_valid = true;
    } else {
        static constexpr float FollowTime = 0.25f;
        const float elapsed = std::chrono::duration<float>(now - pad_anchor_time).count();
        const float blend = 1.0f - std::exp(-std::clamp(elapsed, 0.0f, 1.0f) / FollowTime);
        pad_anchor.x += (target.x - pad_anchor.x) * blend;
        pad_anchor.y += (target.y - pad_anchor.y) * blend;
        pad_anchor.z += (target.z - pad_anchor.z) * blend;
    }
    pad_anchor_time = now;

    // The host seeing the controller beats assuming where it is. Either way the reported
    // position glides to its target: sight of the hands comes and goes, and a controller that
    // teleports is worse than one that lags a little.
    const bool seen = pad_seen && now - pad_seen_time < std::chrono::milliseconds{400};
    if (seen) {
        // People keep a controller where they hold it: out of sight, that is the best guess.
        pad_seen_offset = {pad_seen_position.x - pad_anchor.x, pad_seen_position.y - pad_anchor.y,
                           pad_seen_position.z - pad_anchor.z};
        pad_seen_offset_valid = true;
    }
    // (The player's own place is theirs to say: it counts for more than where the controller
    // was last seen.)
    // (Its own place and the standard one are before the player whichever way TurnView has
    // them face; where it was seen is where it was seen.)
    const Vec3 assumed =
        !own_pad_place && pad_seen_offset_valid
            ? pad_seen_offset
            : Rotate(FromAxisAngle({0.0f, 1.0f, 0.0f}, view_turn),
                     own_pad_place ? own_pad_offset : config.pad_offset);
    const Vec3 goal = seen ? pad_seen_position
                           : Vec3{pad_anchor.x + assumed.x, pad_anchor.y + assumed.y,
                                  pad_anchor.z + assumed.z};
    if (!pad_shown_valid) {
        pad_shown_position = goal;
        pad_shown_valid = true;
    } else {
        const float follow_time = seen ? 0.035f : 0.30f;
        const float elapsed = std::chrono::duration<float>(now - pad_shown_time).count();
        const float blend = 1.0f - std::exp(-std::clamp(elapsed, 0.0f, 1.0f) / follow_time);
        pad_shown_position.x += (goal.x - pad_shown_position.x) * blend;
        pad_shown_position.y += (goal.y - pad_shown_position.y) * blend;
        pad_shown_position.z += (goal.z - pad_shown_position.z) * blend;
    }
    pad_shown_time = now;

    DeviceState state;
    state.pose.position = pad_shown_position;
    if (seen) {
        state.linear_velocity = pad_seen_velocity;
    }
    if (pad.sequence != 0) {
        state.pose.orientation = pad.pose.orientation;
        state.angular_velocity = pad.angular_velocity;
    }
    state.sequence = pad.sequence;
    state.tracked = true;
    return state;
}

Pose Runtime::ToHostSpace(const Pose& tracker_pose) const {
    std::scoped_lock lock{mutex};
    const auto from_seat = [&](const Vec3& position, const Quat& yaw) {
        const Vec3 seated = Rotate(yaw, {tracker_pose.position.x - config.origin_offset.x,
                                         tracker_pose.position.y - config.origin_offset.y,
                                         tracker_pose.position.z - config.origin_offset.z});
        Pose pose;
        pose.position = {seated.x + position.x, seated.y + position.y, seated.z + position.z};
        pose.orientation = Normalize(Multiply(yaw, tracker_pose.orientation));
        return pose;
    };
    const Pose pose = from_seat(seat_position, seat_yaw);
    // A pose the title was given just before the view was reset was counted from the seat
    // before: taken from the new one it would point somewhere the head never was. The head
    // does not turn far in the time a frame takes, so the seat that puts the pose nearer to
    // where the head is now is the one it was counted from.
    if (host_head.tracked &&
        std::chrono::steady_clock::now() - seat_changed < std::chrono::milliseconds{250}) {
        const Pose before = from_seat(previous_seat_position, previous_seat_yaw);
        const auto nearness = [&](const Quat& q) {
            const Quat& head_now = host_head.pose.orientation;
            return std::abs(q.x * head_now.x + q.y * head_now.y + q.z * head_now.z +
                            q.w * head_now.w);
        };
        if (nearness(before.orientation) > nearness(pose.orientation)) {
            return before;
        }
    }
    return pose;
}

void Runtime::SetFrameListener(std::function<void(const PresentedFrame&)> listener) {
    std::scoped_lock lock{mutex};
    frame_listener = std::move(listener);
}

void Runtime::NotifyFramePresented(const PresentedFrame& frame) {
    std::function<void(const PresentedFrame&)> listener;
    {
        std::scoped_lock lock{mutex};
        listener = frame_listener;
    }
    if (listener) {
        listener(frame);
    }
}

} // namespace Core::Vr
