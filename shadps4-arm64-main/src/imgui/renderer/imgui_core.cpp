// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "font_data.h"
#include "font_stack.h"
#include "imgui/imgui_layer.h"
#include "imgui_core.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "sdl_window.h"
#include "texture_manager.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

static void CheckVkResult(const vk::Result err) {
    LOG_ERROR(ImGui, "Vulkan error {}", vk::to_string(err));
}

static std::vector<ImGui::Layer*> layers;

// Update layers before rendering to allow layer changes to be applied during rendering.
// Using deque to keep the order of changes in case a Layer is removed then added again between
// frames.
static std::deque<std::pair<bool, ImGui::Layer*>> change_layers{};
static std::mutex change_layers_mutex{};

static ImGuiID dock_id;
static std::atomic<std::uint32_t> force_gamepad_input_capture_count{0};

// ImGui builds the text of imgui.ini in one buffer of its context. Two threads saving at once
// (the save at quick_exit on the thread that ends the emulator, ImGui's own timed save inside
// NewFrame on the render thread) append into that buffer together, and the file then holds
// every section twice, interleaved. Its docking nodes are then registered twice when the file
// is read at the next start, which ImGui asserts on: the emulator stopped at every start with
// signal 5 (Issue #56). One save at a time; the one at exit gives up rather than wait for a
// render thread that may never come back.
static std::recursive_timed_mutex ini_mutex;

// The text of an imgui.ini with each docking node kept once: the first line that names an ID
// stays, later lines naming the same ID are left out. `removed` counts what was left out.
static std::string WithoutRepeatedDockNodes(const std::string& text, int& removed) {
    std::string out;
    out.reserve(text.size());
    std::unordered_set<std::string> seen;
    bool in_docking = false;
    size_t at = 0;
    removed = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1;
        const std::string_view line{text.data() + at, end - at};
        at = end;
        if (!line.empty() && line.front() == '[') {
            in_docking = line.starts_with("[Docking][Data]");
        } else if (in_docking) {
            const size_t first = line.find_first_not_of(' ');
            const size_t id_at = line.find(" ID=");
            if (first != std::string_view::npos && line.substr(first).starts_with("Dock") &&
                id_at != std::string_view::npos) {
                const size_t id_end = line.find_first_of(" \r\n", id_at + 4);
                std::string id{line.substr(id_at + 4, id_end == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : id_end - (id_at + 4))};
                if (!seen.insert(std::move(id)).second) {
                    ++removed;
                    continue;
                }
            }
        }
        out.append(line);
    }
    return out;
}

// Reads imgui.ini before ImGui does, when a docking node is named in it more than once: ImGui
// is then given the text with each node once, and saves it so the next start reads it clean.
static void LoadIniWithoutRepeatedDockNodes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return;
    }
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    int removed = 0;
    const std::string clean = WithoutRepeatedDockNodes(text, removed);
    if (removed == 0) {
        return; // ImGui reads the file itself at its first frame, as always.
    }
    LOG_WARNING(ImGui,
                "imgui.ini named {} docking node(s) a second time (the file had been saved by "
                "two threads at once): read without the repeats, and saved again clean",
                removed);
    ImGui::LoadIniSettingsFromMemory(clean.data(), clean.size());
    ImGui::MarkIniSettingsDirty();
}

namespace ImGui {

namespace Core {

void SaveIniSettings() {
    std::scoped_lock lock{ini_mutex};
    SaveIniSettingsToDisk(GetIO().IniFilename);
}

void AcquireGamepadInputCapture() {
    force_gamepad_input_capture_count.fetch_add(1, std::memory_order_relaxed);
}

void ReleaseGamepadInputCapture() {
    std::uint32_t expected = force_gamepad_input_capture_count.load(std::memory_order_relaxed);
    while (expected != 0) {
        if (force_gamepad_input_capture_count.compare_exchange_weak(
                expected, expected - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
    LOG_WARNING(ImGui, "ReleaseGamepadInputCapture called with no active capture");
}

bool IsGamepadInputCaptured() {
    return force_gamepad_input_capture_count.load(std::memory_order_relaxed) > 0;
}

void Initialize(const ::Vulkan::Instance& instance, const Frontend::WindowSDL& window,
                const u32 image_count, vk::Format surface_format,
                const vk::AllocationCallbacks* allocator) {

    const auto config_path = GetUserPath(Common::FS::PathType::UserDir) / "imgui.ini";
    const auto log_path = GetUserPath(Common::FS::PathType::LogDir) / "imgui_log.txt";

    std::fprintf(stderr, "BACHATA_IMGUI_CTX\n");
    CreateContext();
    ImGuiIO& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
    PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); // Makes the window edges rounded

    auto path = config_path.u8string();
    char* config_file_buf = new char[path.size() + 1]();
    std::memcpy(config_file_buf, path.c_str(), path.size());
    io.IniFilename = config_file_buf;

    path = log_path.u8string();
    char* log_file_buf = new char[path.size() + 1]();
    std::memcpy(log_file_buf, path.c_str(), path.size());
    io.LogFilename = log_file_buf;

    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    const int console_language = EmulatorSettings.GetConsoleLanguage();
    // Nobody reads this interface when there is no window, and the full set of fonts makes for
    // an atlas of over a hundred megabytes, twice while it is uploaded. That is memory a headset
    // does not have to spare, so there the same fonts are built small and without the large
    // East Asian character sets. Their number and order stay: other code refers to them by index.
    const bool headless = window.GetWindowInfo().type == Frontend::WindowSystemType::Headless;
    io.FontDefault =
        FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, console_language, font_cfg, !headless);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);

    // Avoid exploding atlas size on Metal/MoltenVK when CJK fallback is enabled.
    FontStack::AddPrimaryUiFont(io.Fonts, headless ? 32.0f : 128.0f, console_language, font_cfg,
                                false);

    // Big Picture
    FontStack::AddPrimaryUiFont(ImGui::GetIO().Fonts, headless ? 32.0f : 64.0f,
                                EmulatorSettings.GetConsoleLanguage(), font_cfg, !headless);

    std::fprintf(stderr, "BACHATA_IMGUI_FONTS_BUILD\n");
    io.Fonts->Build();
    std::fprintf(stderr, "BACHATA_IMGUI_FONTS_READY\n");

    io.FontGlobalScale = 0.5f;

    StyleColorsDark();

    std::fprintf(stderr, "BACHATA_IMGUI_DEVTOOLS\n");
    ::Core::Devtools::Layer::SetupSettings();
    // After the settings handlers exist, so that the layer's own sections are read too.
    LoadIniWithoutRepeatedDockNodes(config_path);
    std::fprintf(stderr, "BACHATA_IMGUI_SDL\n");
    Sdl::Init(window.GetSDLWindow());

    const Vulkan::InitInfo vk_info{
        .instance = instance.GetInstance(),
        .physical_device = instance.GetPhysicalDevice(),
        .device = instance.GetDevice(),
        .queue_family = instance.GetPresentQueueFamilyIndex(),
        .queue = instance.GetPresentQueue(),
        .image_count = image_count,
        .min_allocation_size = 1024 * 1024,
        .pipeline_rendering_create_info{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &surface_format,
        },
        .allocator = allocator,
        .check_vk_result_fn = &CheckVkResult,
    };
    std::fprintf(stderr, "BACHATA_IMGUI_VK\n");
    Vulkan::Init(vk_info);
    std::fprintf(stderr, "BACHATA_IMGUI_VK_READY\n");

    std::fprintf(stderr, "BACHATA_IMGUI_TEX\n");
    TextureManager::StartWorker();
    std::fprintf(stderr, "BACHATA_IMGUI_TEX_READY\n");

    char label[32];
    ImFormatString(label, IM_ARRAYSIZE(label), "WindowOverViewport_%08X", GetMainViewport()->ID);
    dock_id = ImHashStr(label);

    if (const auto dpi = SDL_GetWindowDisplayScale(window.GetSDLWindow()); dpi > 0.0f) {
        GetIO().FontGlobalScale *= dpi;
    }

    std::at_quick_exit([] {
        std::unique_lock lock{ini_mutex, std::defer_lock};
        if (lock.try_lock_for(std::chrono::milliseconds{500})) {
            SaveIniSettingsToDisk(GetIO().IniFilename);
        }
    });
}

void OnResize() {
    Sdl::OnResize();
}

void OnSurfaceFormatChange(vk::Format surface_format) {
    Vulkan::OnSurfaceFormatChange(surface_format);
}

void Shutdown(const vk::Device& device) {
    auto result = device.waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on shutdown: {}",
                    vk::to_string(result));
    }

    TextureManager::StopWorker();

    const ImGuiIO& io = GetIO();
    const auto ini_filename = (void*)io.IniFilename;
    const auto log_filename = (void*)io.LogFilename;

    Vulkan::Shutdown();
    Sdl::Shutdown();
    DestroyContext();

    delete[] (char*)ini_filename;
    delete[] (char*)log_filename;
}

bool ProcessEvent(SDL_Event* event) {
    Sdl::ProcessEvent(event);
    switch (event->type) {
    // Don't block release/up events
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const auto& io = GetIO();
        return io.WantCaptureMouse && io.Ctx->NavWindow != nullptr &&
               (io.Ctx->NavWindow->Flags & ImGuiWindowFlags_NoNav) == 0;
    }
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN: {
        if (IsGamepadInputCaptured()) {
            // Keep keyboard events flowing through the regular input-binding path while IME/OSK
            // captures gamepad input, so keyboard equivalents of pad buttons still update the
            // virtual controller state.
            return false;
        }
        const auto& io = GetIO();
        return io.WantCaptureKeyboard && io.Ctx->NavWindow != nullptr &&
               io.Ctx->NavWindow->ID != dock_id;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (IsGamepadInputCaptured()) {
            // Let controller events continue to input bindings so OSK shortcuts and virtual
            // controller state keep updating; game-side pad reads are blocked in libScePad.
            return false;
        }
        const auto& io = GetIO();
        return io.NavActive && io.Ctx->NavWindow != nullptr && io.Ctx->NavWindow->ID != dock_id;
    }
    default:
        return false;
    }
}

ImGuiID NewFrame(bool is_reusing_frame) {
    {
        std::scoped_lock lock{change_layers_mutex};
        while (!change_layers.empty()) {
            const auto [to_be_added, layer] = change_layers.front();
            if (to_be_added) {
                layers.push_back(layer);
            } else {
                const auto [begin, end] = std::ranges::remove(layers, layer);
                layers.erase(begin, end);
            }
            change_layers.pop_front();
        }
    }

    Sdl::NewFrame(is_reusing_frame);
    {
        // ImGui saves imgui.ini from here when its settings changed a few seconds ago.
        std::scoped_lock lock{ini_mutex};
        ImGui::NewFrame();
    }

    ImGuiWindowFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    if (!DebugState.IsShowingDebugMenuBar()) {
        flags |= ImGuiDockNodeFlags_NoTabBar;
    }
    ImGuiID dockId = DockSpaceOverViewport(0, GetMainViewport(), flags);

    for (auto* layer : layers) {
        layer->Draw();
    }

    return dockId;
}

void Render(const vk::CommandBuffer& cmdbuf, const vk::ImageView& image_view,
            const vk::Extent2D& extent) {
    ImGui::Render();
    ImDrawData* draw_data = GetDrawData();
    if (draw_data->CmdListsCount == 0) {
        return;
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "ImGui Render",
        });
    }

    vk::RenderingAttachmentInfo color_attachments[1]{
        {
            .imageView = image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    };
    vk::RenderingInfo render_info{};
    render_info.renderArea = vk::Rect2D{
        .offset = {0, 0},
        .extent = extent,
    };
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = color_attachments;
    cmdbuf.beginRendering(render_info);
    Vulkan::RenderDrawData(*draw_data, cmdbuf);
    cmdbuf.endRendering();
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

bool MustKeepDrawing() {
    return layers.size() > 1 || change_layers.size() > 1 || DebugState.IsShowingDebugMenuBar();
}

} // namespace Core

void Layer::AddLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(true, layer);
}

void Layer::RemoveLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(false, layer);
}

} // namespace ImGui
