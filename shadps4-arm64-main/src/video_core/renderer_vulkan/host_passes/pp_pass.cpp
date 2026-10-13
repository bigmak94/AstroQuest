//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/pp_pass.h"

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/post_process_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <boost/container/static_vector.hpp>

namespace Vulkan::HostPasses {

void PostProcessingPass::Create(const Instance& instance, MasterSemaphore* master_semaphore,
                                const vk::Format surface_format) {
    device = instance.GetDevice();
    uses_push_descriptors = instance.IsPushDescriptorSupported();
    desc_heap = DescriptorHeap{instance, master_semaphore, pool_sizes, 64};
    static const std::array pp_shaders{
        HostShaders::FS_TRI_VERT,
        HostShaders::POST_PROCESS_FRAG,
    };

    boost::container::static_vector<vk::DescriptorSetLayoutBinding, 2> bindings{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };

    const vk::DescriptorSetLayoutCreateFlags layout_flags =
        uses_push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                              : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = layout_flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    desc_set_layout = Check<"create pp descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Settings),
    };

    const auto& vs_module = Compile(pp_shaders[0], vk::ShaderStageFlagBits::eVertex, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = Compile(pp_shaders[1], vk::ShaderStageFlagBits::eFragment, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "post_process.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create pp pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const std::array pp_color_formats{
        surface_format,
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = pp_color_formats.size(),
        .pColorAttachmentFormats = pp_color_formats.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &pipeline_rendering_ci,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    pipeline = Check<"create post process pipeline">(device.createGraphicsPipelineUnique(
        /*pipeline_cache*/ {}, pipeline_info));

    // Once pipeline is compiled, we don't need the shader module anymore
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // Create sampler resource
    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create pp sampler">(device.createSamplerUnique(sampler_ci));
}

void PostProcessingPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                                vk::Extent2D input_size, Frame& frame, Settings settings) {
    const std::array regions{
        Region{
            .input = input,
            .area{.extent{.width = frame.width, .height = frame.height}},
        },
    };
    Render(cmdbuf, regions, frame, settings);
}

void PostProcessingPass::Render(vk::CommandBuffer cmdbuf, std::span<const Region> regions,
                                Frame& frame, Settings settings, std::optional<u32> marker) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/Post processing",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    // Where the regions draw every pixel of the frame between them (the two eyes side by side
    // of a headset's frame), clearing it first only costs time: a frame the size of a
    // headset's is memory the GPU writes through without compression or tiles.
    const auto drawn = [&](const Region& region) {
        const vk::Rect2D& clip = region.clip.value_or(region.area);
        const s64 left = std::max(region.area.offset.x, clip.offset.x);
        const s64 top = std::max(region.area.offset.y, clip.offset.y);
        const s64 right = std::min<s64>(s64{region.area.offset.x} + region.area.extent.width,
                                        s64{clip.offset.x} + clip.extent.width);
        const s64 bottom = std::min<s64>(s64{region.area.offset.y} + region.area.extent.height,
                                         s64{clip.offset.y} + clip.extent.height);
        return std::array{std::max<s64>(left, 0), std::max<s64>(top, 0),
                          std::min<s64>(right, frame.width), std::min<s64>(bottom, frame.height)};
    };
    s64 covered = 0;
    bool overlap = false;
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto a = drawn(regions[i]);
        covered += std::max<s64>(a[2] - a[0], 0) * std::max<s64>(a[3] - a[1], 0);
        for (size_t j = 0; j < i; ++j) {
            const auto b = drawn(regions[j]);
            overlap |= a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
        }
    }
    // (Under the A/B measuring aid as "ppcopy", the frame is cleared all the same.)
    const bool covers = !overlap && covered == s64{frame.width} * frame.height &&
                        !FrameStats::AbOff("ppcopy");
    const std::array<vk::RenderingAttachmentInfo, 1> attachments{{
        {
            .imageView = frame.image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = covers ? vk::AttachmentLoadOp::eDontCare : vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    }};
    const vk::RenderingInfo rendering_info{
        .renderArea{
            .extent{
                .width = frame.width,
                .height = frame.height,
            },
        },
        .layerCount = 1,
        .colorAttachmentCount = attachments.size(),
        .pColorAttachments = attachments.data(),
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Settings),
                         &settings);

    // Without push descriptors the sets have to be written before rendering begins.
    boost::container::static_vector<vk::DescriptorSet, 4> desc_sets;
    if (!uses_push_descriptors) {
        for (const Region& region : regions) {
            const vk::DescriptorImageInfo image_info{
                .sampler = *sampler,
                .imageView = region.input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            };
            const auto desc_set = desc_heap.Commit(*desc_set_layout);
            device.updateDescriptorSets(
                vk::WriteDescriptorSet{
                    .dstSet = desc_set,
                    .dstBinding = 0,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                    .pImageInfo = &image_info,
                },
                {});
            desc_sets.push_back(desc_set);
        }
    }

    cmdbuf.beginRendering(rendering_info);
    for (size_t i = 0; i < regions.size(); ++i) {
        const Region& region = regions[i];
        cmdbuf.setViewport(0, vk::Viewport{
                                  .x = static_cast<float>(region.area.offset.x),
                                  .y = static_cast<float>(region.area.offset.y),
                                  .width = static_cast<float>(region.area.extent.width),
                                  .height = static_cast<float>(region.area.extent.height),
                                  .minDepth = 0.0f,
                                  .maxDepth = 1.0f,
                              });
        cmdbuf.setScissor(0, region.clip.value_or(region.area));

        if (uses_push_descriptors) {
            const vk::DescriptorImageInfo image_info{
                .sampler = *sampler,
                .imageView = region.input,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            };
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                        vk::WriteDescriptorSet{
                                            .dstSet = VK_NULL_HANDLE,
                                            .dstBinding = 0,
                                            .dstArrayElement = 0,
                                            .descriptorCount = 1,
                                            .descriptorType =
                                                vk::DescriptorType::eCombinedImageSampler,
                                            .pImageInfo = &image_info,
                                        });
        } else {
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                      desc_sets[i], {});
        }
        cmdbuf.draw(3, 1, 0, 0);
    }

    if (marker && frame.width >= MarkerBlocks * MarkerBlockSize &&
        frame.height >= MarkerBlockSize) {
        const u32 id = *marker & ((1u << MarkerIdBits) - 1);
        // Sync blocks first (white, black), then the id and its check value.
        const u64 bits = 0b01ULL | (u64{id} << MarkerSyncBlocks) |
                         (u64{MarkerCheck(id)} << (MarkerSyncBlocks + MarkerIdBits));
        for (u32 block = 0; block < MarkerBlocks; ++block) {
            const float level = ((bits >> block) & 1) != 0 ? 1.0f : 0.0f;
            const vk::ClearAttachment clear{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .colorAttachment = 0,
                .clearValue{.color{std::array{level, level, level, 1.0f}}},
            };
            const vk::ClearRect rect{
                .rect{
                    .offset{.x = static_cast<s32>(block * MarkerBlockSize), .y = 0},
                    .extent{.width = MarkerBlockSize, .height = MarkerBlockSize},
                },
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            cmdbuf.clearAttachments(clear, rect);
        }
    }
    cmdbuf.endRendering();

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = simple_subresource,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

} // namespace Vulkan::HostPasses
