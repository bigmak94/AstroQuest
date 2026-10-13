// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_foveation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include "video_core/host_shaders/color_to_ms_depth_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/ms_image_blit_frag.h"
#include "video_core/host_shaders/resolve_aa_frag.h"

namespace VideoCore {

BlitHelper::BlitHelper(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      uses_push_descriptors{instance_.IsPushDescriptorSupported()},
      desc_heap{instance_, scheduler_.GetMasterSemaphore(), pool_sizes, 64} {
    CreateShaders();
    CreatePipelineLayouts();
    const char* setting = std::getenv("SHADPS4_RESOLVE_AA");
    smooths_resolves = !(setting != nullptr && setting[0] == '0');
}

BlitHelper::~BlitHelper() {
    const auto device = instance.GetDevice();
    device.destroy(fs_tri_vertex);
    device.destroy(color_to_ms_depth_frag);
    device.destroy(src_msaa_copy_frag);
    device.destroy(src_non_msaa_copy_frag);
    device.destroy(resolve_aa_frag);
    if (resolve_aa_fine_frag) {
        device.destroy(resolve_aa_fine_frag);
    }
}

void BlitHelper::ReinterpretColorAsMsDepth(u32 width, u32 height, vk::SampleCountFlagBits samples,
                                           vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                           vk::Image source, vk::Image dest) {
    const vk::ImageViewUsageCreateInfo color_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo color_view_ci = {
        .pNext = &color_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = src_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [color_view_result, color_view] =
        instance.GetDevice().createImageView(color_view_ci);
    ASSERT_MSG(color_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(color_view_result));
    const vk::ImageViewUsageCreateInfo depth_usage_ci{
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment};
    const vk::ImageViewCreateInfo depth_view_ci = {
        .pNext = &depth_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = dst_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [depth_view_result, depth_view] =
        instance.GetDevice().createImageView(depth_view_ci);
    ASSERT_MSG(depth_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(depth_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), color_view, depth_view] {
        device.destroyImageView(color_view);
        device.destroyImageView(depth_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.depth_stencil_attachment.image_view = depth_view;
    state.depth_stencil_attachment.image_layout = vk::ImageLayout::eDepthAttachmentOptimal;
    state.depth_stencil_attachment.has_depth = true;
    state.depth_stencil_attachment.depth_clear = true;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = color_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                    texture_write);
    } else {
        const auto desc_set = desc_heap.Commit(*single_texture_descriptor_set_layout);
        vk::WriteDescriptorSet set_write = texture_write;
        set_write.dstSet = desc_set;
        instance.GetDevice().updateDescriptorSets(set_write, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                  desc_set, {});
    }

    const MsPipelineKey key{samples, dst_pixel_format, false};
    auto it = std::ranges::find(color_to_ms_depth_pl, key, &MsPipeline::first);
    if (it == color_to_ms_depth_pl.end()) {
        CreateColorToMSDepthPipeline(key);
        it = --color_to_ms_depth_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::CopyBetweenMsImages(u32 width, u32 height, vk::SampleCountFlagBits samples,
                                     vk::Format pixel_format, bool src_msaa, vk::Image source,
                                     vk::Image dest) {
    const vk::ImageViewUsageCreateInfo src_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo src_view_ci = {
        .pNext = &src_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [src_view_result, src_view] = instance.GetDevice().createImageView(src_view_ci);
    ASSERT_MSG(src_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(src_view_result));

    const vk::ImageViewUsageCreateInfo dst_usage_ci{.usage =
                                                        vk::ImageUsageFlagBits::eColorAttachment};
    const vk::ImageViewCreateInfo dst_view_ci = {
        .pNext = &dst_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [dst_view_result, dst_view] = instance.GetDevice().createImageView(dst_view_ci);
    ASSERT_MSG(dst_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(dst_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), src_view, dst_view] {
        device.destroyImageView(src_view);
        device.destroyImageView(dst_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0].image_view = dst_view;
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    state.color_attachments[0].is_clear = true;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = src_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                    texture_write);
    } else {
        const auto desc_set = desc_heap.Commit(*single_texture_descriptor_set_layout);
        vk::WriteDescriptorSet set_write = texture_write;
        set_write.dstSet = desc_set;
        instance.GetDevice().updateDescriptorSets(set_write, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                  desc_set, {});
    }

    const MsPipelineKey key{samples, pixel_format, src_msaa};
    auto it = std::ranges::find(ms_image_copy_pl, key, &MsPipeline::first);
    if (it == ms_image_copy_pl.end()) {
        CreateMsCopyPipeline(key);
        it = --ms_image_copy_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::SmoothInto(u32 width, u32 height, vk::Format src_pixel_format,
                            vk::Format dst_pixel_format, vk::Image source, u32 src_layer,
                            vk::Image dest, u32 dst_layer, bool fine_only) {
    const auto device = instance.GetDevice();
    const vk::ImageViewUsageCreateInfo src_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo src_view_ci = {
        .pNext = &src_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = src_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = src_layer,
            .layerCount = 1U,
        },
    };
    const auto [src_view_result, src_view] = device.createImageView(src_view_ci);
    ASSERT_MSG(src_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(src_view_result));
    const vk::ImageViewUsageCreateInfo dst_usage_ci{.usage =
                                                        vk::ImageUsageFlagBits::eColorAttachment};
    const vk::ImageViewCreateInfo dst_view_ci = {
        .pNext = &dst_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = dst_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = dst_layer,
            .layerCount = 1U,
        },
    };
    const auto [dst_view_result, dst_view] = device.createImageView(dst_view_ci);
    ASSERT_MSG(dst_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(dst_view_result));
    scheduler.DeferOperation([device, src_view, dst_view] {
        device.destroyImageView(src_view);
        device.destroyImageView(dst_view);
    });

    // Every pixel is drawn. The pass is not begun with a clear for all that: where this runs
    // the driver draws straight into the image (see TU_DEBUG=sysmem), and a clear would be a
    // second pass over all of it for nothing.
    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0].image_view = dst_view;
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = *linear_sampler,
        .imageView = src_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_info,
    };
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *sampled_texture_pl_layout,
                                    0U, texture_write);
    } else {
        const auto desc_set = desc_heap.Commit(*sampled_texture_descriptor_set_layout);
        vk::WriteDescriptorSet set_write = texture_write;
        set_write.dstSet = desc_set;
        device.updateDescriptorSets(set_write, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *sampled_texture_pl_layout, 0U,
                                  desc_set, {});
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                        SmoothPipeline(dst_pixel_format, fine_only));

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::CreateShaders() {
    const auto device = instance.GetDevice();
    fs_tri_vertex =
        Vulkan::Compile(HostShaders::FS_TRI_VERT, vk::ShaderStageFlagBits::eVertex, device);
    color_to_ms_depth_frag = Vulkan::Compile(HostShaders::COLOR_TO_MS_DEPTH_FRAG,
                                             vk::ShaderStageFlagBits::eFragment, device);
    src_msaa_copy_frag = Vulkan::Compile(HostShaders::MS_IMAGE_BLIT_FRAG,
                                         vk::ShaderStageFlagBits::eFragment, device, {"SRC_MSAA"});
    src_non_msaa_copy_frag = Vulkan::Compile(HostShaders::MS_IMAGE_BLIT_FRAG,
                                             vk::ShaderStageFlagBits::eFragment, device);
    resolve_aa_frag =
        Vulkan::Compile(HostShaders::RESOLVE_AA_FRAG, vk::ShaderStageFlagBits::eFragment, device);
    if (const float fine = Vulkan::Foveation::CoarseFrom(); fine <= 1.0f) {
        resolve_aa_fine_frag =
            Vulkan::Compile(HostShaders::RESOLVE_AA_FRAG, vk::ShaderStageFlagBits::eFragment,
                            device, {fmt::format("FINE_RADIUS={:.3f}", fine)});
    }
}

void BlitHelper::CreatePipelineLayouts() {
    const vk::DescriptorSetLayoutBinding texture_binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    const vk::DescriptorSetLayoutCreateFlags layout_flags =
        uses_push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                              : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = layout_flags,
        .bindingCount = 1U,
        .pBindings = &texture_binding,
    };
    auto [desc_layout_result, desc_layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    single_texture_descriptor_set_layout = std::move(desc_layout);
    const vk::DescriptorSetLayout set_layout = *single_texture_descriptor_set_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, pipeline_layout] =
        instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline_layout, "Single texture pipeline layout");
    single_texture_pl_layout = std::move(pipeline_layout);

    // The same with a sampler to read the texture through.
    const vk::DescriptorSetLayoutBinding sampled_binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    const vk::DescriptorSetLayoutCreateInfo sampled_layout_ci = {
        .flags = layout_flags,
        .bindingCount = 1U,
        .pBindings = &sampled_binding,
    };
    auto [sampled_layout_result, sampled_layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(sampled_layout_ci);
    ASSERT_MSG(sampled_layout_result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}", vk::to_string(sampled_layout_result));
    sampled_texture_descriptor_set_layout = std::move(sampled_layout);
    const vk::DescriptorSetLayout sampled_set_layout = *sampled_texture_descriptor_set_layout;
    const vk::PipelineLayoutCreateInfo sampled_pl_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &sampled_set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [sampled_pl_result, sampled_pl_layout] =
        instance.GetDevice().createPipelineLayoutUnique(sampled_pl_info);
    ASSERT_MSG(sampled_pl_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(sampled_pl_result));
    Vulkan::SetObjectName(instance.GetDevice(), *sampled_pl_layout,
                          "Sampled texture pipeline layout");
    sampled_texture_pl_layout = std::move(sampled_pl_layout);

    const vk::SamplerCreateInfo sampler_ci = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    auto [sampler_result, sampler] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    linear_sampler = std::move(sampler);
}

vk::Pipeline BlitHelper::SmoothPipeline(vk::Format attachment_format, bool fine_only) {
    fine_only = fine_only && resolve_aa_fine_frag;
    auto& pipelines = fine_only ? resolve_aa_fine_pl : resolve_aa_pl;
    const auto it =
        std::ranges::find(pipelines, attachment_format, &decltype(resolve_aa_pl)::value_type::first);
    if (it != pipelines.end()) {
        return *it->second;
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = fine_only ? resolve_aa_fine_frag : resolve_aa_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 1u,
        .pColorAttachmentFormats = &attachment_format,
        .depthAttachmentFormat = vk::Format::eUndefined,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendAttachmentState attachment = {
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1u,
        .pAttachments = &attachment,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *sampled_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Smoothed resolve {}",
                          vk::to_string(attachment_format));

    return *pipelines.emplace_back(attachment_format, std::move(pipeline)).second;
}

void BlitHelper::CreateColorToMSDepthPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = key.samples,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = true,
        .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = color_to_ms_depth_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 0U,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = key.attachment_format,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Color to MS Depth {}",
                          vk::to_string(key.samples));

    color_to_ms_depth_pl.emplace_back(key, std::move(pipeline));
}

void BlitHelper::CreateMsCopyPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = key.samples,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = key.src_msaa ? src_msaa_copy_frag : src_non_msaa_copy_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 1u,
        .pColorAttachmentFormats = &key.attachment_format,
        .depthAttachmentFormat = vk::Format::eUndefined,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendAttachmentState attachment = {
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1u,
        .pAttachments = &attachment,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Non MS Image to MS Image {}",
                          vk::to_string(key.samples));

    ms_image_copy_pl.emplace_back(key, std::move(pipeline));
}

} // namespace VideoCore
