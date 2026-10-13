// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <variant>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    /// What a draw that was given a permutation came with, as far as which permutation it gets
    /// goes (PipelineCache::Recognize). The next draw that comes with the same gets the same,
    /// without its specialization being worked out and compared again: what most draws of a
    /// frame do, and what was a sixth of the time the GPU thread spends on a draw.
    struct Seen {
        Shader::RuntimeInfo runtime_info;
        Shader::Backend::Bindings start;
        /// Where the fetch shader was, and its code (memory gets reused).
        const u32* fetch_code{};
        std::vector<u32> fetch_words;
        /// What each resource is, without where it is (SharpWords).
        boost::container::small_vector<u32, 96> sharp_words;
        size_t perm_idx{};
    };
    static constexpr size_t MaxSeen = 8;

    Shader::Info info;
    ModuleList modules{};
    /// The most recent first.
    boost::container::small_vector<Seen, 2> seen;

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline();

    /// The colour targets (a bit each) that the graphics pipelines asked for from now on leave
    /// out, as if the draw had nothing bound there. For a draw that has one surface bound as
    /// more than one of its targets, which is drawn once for each (Rasterizer::Draw).
    void LeaveTargetsOut(u8 targets) {
        targets_left_out = targets;
    }

    const ComputePipeline* GetComputePipeline();

    /// The stage's info, its module, the fetch shader the module was made for (it stays where
    /// it is for as long as the pipeline is being looked up) and the permutation's hash.
    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              const std::optional<Shader::Gcn::FetchShaderData>*, u64>;
    Result GetProgram(Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::Stage stage, Shader::LogicalStage l_stage);

    /// The permutation of `program` an earlier draw that came with the same was given, if one
    /// was; Remember notes what a draw came with and was given.
    std::optional<size_t> Recognize(Program& program, const Shader::RuntimeInfo& runtime_info,
                                    const Shader::Backend::Bindings& start);
    void Remember(Program& program, const Shader::RuntimeInfo& runtime_info,
                  const Shader::Backend::Bindings& start, size_t perm_idx);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    /// The fetch shader of the pipeline being looked up, where it is kept (GetProgram).
    const Shader::Gcn::FetchShaderData* stage_fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    /// The pipeline the last draw got, and for what: most draws get the one the draw before
    /// them did.
    GraphicsPipelineKey last_key{};
    const GraphicsPipeline* last_pipeline{};
    /// Scratch room for Recognize.
    boost::container::small_vector<u32, 96> sharp_words;
    u8 targets_left_out{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
