// Prints what a Vulkan driver offers where the emulator has to adapt to it: sample counts
// (device limits and per image format/usage), memory types, a few features and extensions.
//   vk_caps_probe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static void print_memory_types(VkPhysicalDevice physical) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        printf("memory type %u: heap %u%s%s%s%s%s\n", i, memory.memoryTypes[i].heapIndex,
               flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
               flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "",
               flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
               flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "",
               flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " lazy" : "");
    }
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        printf("memory heap %u: %llu MB\n", i,
               (unsigned long long)(memory.memoryHeaps[i].size >> 20));
    }
}

static void print_samples(VkPhysicalDevice physical, const char* name, VkFormat format,
                          VkImageUsageFlags usage, const char* usage_name) {
    VkImageFormatProperties properties;
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, &properties);
    if (result != VK_SUCCESS) {
        printf("  %-22s %-28s unsupported (%d)\n", name, usage_name, result);
        return;
    }
    printf("  %-22s %-28s samples 0x%x\n", name, usage_name, properties.sampleCounts);
}

int main(void) {
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &app};
    VkInstance instance;
    if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n");
        return 1;
    }
    uint32_t count = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (count == 0) {
        printf("no physical device\n");
        return 1;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    const VkPhysicalDeviceLimits* limits = &properties.limits;
    printf("device: %s, api %u.%u.%u, driver 0x%x\n", properties.deviceName,
           VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
           VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    printf("limits: color samples 0x%x, depth 0x%x, stencil 0x%x, sampled color 0x%x, "
           "sampled depth 0x%x, storage 0x%x\n",
           limits->framebufferColorSampleCounts, limits->framebufferDepthSampleCounts,
           limits->framebufferStencilSampleCounts, limits->sampledImageColorSampleCounts,
           limits->sampledImageDepthSampleCounts, limits->storageImageSampleCounts);
    printf("limits: max image 2D %u, max viewports %u, max framebuffer %ux%u, "
           "work group count %u, non-coherent atom %llu, uniform alignment %llu\n",
           limits->maxImageDimension2D, limits->maxViewports, limits->maxFramebufferWidth,
           limits->maxFramebufferHeight, limits->maxComputeWorkGroupCount[0],
           (unsigned long long)limits->nonCoherentAtomSize,
           (unsigned long long)limits->minUniformBufferOffsetAlignment);

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    printf("features: storage-image-multisample %u, sample-rate-shading %u, geometry %u, "
           "tessellation %u, BC %u, ASTC %u, ETC2 %u, multi-viewport %u, depth-bounds %u, "
           "dual-src-blend %u, logic-op %u, int64 %u, int16 %u, float64 %u, depth-clamp %u\n",
           features.shaderStorageImageMultisample, features.sampleRateShading,
           features.geometryShader, features.tessellationShader, features.textureCompressionBC,
           features.textureCompressionASTC_LDR, features.textureCompressionETC2,
           features.multiViewport, features.depthBounds, features.dualSrcBlend, features.logicOp,
           features.shaderInt64, features.shaderInt16, features.shaderFloat64,
           features.depthClamp);

    print_memory_types(physical);

    static const struct {
        const char* name;
        VkFormat format;
        int depth;
    } formats[] = {
        {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, 0},
        {"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, 0},
        {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, 0},
        {"B10G11R11_UFLOAT", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0},
        {"R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT, 0},
        {"R8_UNORM", VK_FORMAT_R8_UNORM, 0},
        {"R32_UINT", VK_FORMAT_R32_UINT, 0},
        {"D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT, 1},
        {"D32_SFLOAT", VK_FORMAT_D32_SFLOAT, 1},
        {"D16_UNORM", VK_FORMAT_D16_UNORM, 1},
    };
    const VkImageUsageFlags common = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    printf("image format sample counts (2D, optimal tiling, mutable + extended usage):\n");
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        if (formats[i].depth) {
            print_samples(physical, formats[i].name, formats[i].format,
                          common | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth attachment");
        } else {
            print_samples(physical, formats[i].name, formats[i].format,
                          common | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "color attachment");
            print_samples(physical, formats[i].name, formats[i].format,
                          common | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                          "color attachment + storage");
        }
    }

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL);
    VkExtensionProperties* extensions = calloc(extension_count, sizeof(*extensions));
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions);
    printf("%u device extensions:", extension_count);
    int has_shading_rate = 0;
    for (uint32_t i = 0; i < extension_count; ++i) {
        printf(" %s", extensions[i].extensionName + 3);
        has_shading_rate |= strcmp(extensions[i].extensionName,
                                   VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
    }
    printf("\n");

    // What coarser shading towards the edges of a picture (the emulator's foveation) can use.
    if (has_shading_rate) {
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                               .pNext = &rate_features};
        vkGetPhysicalDeviceFeatures2(physical, &features2);
        printf("fragment shading rate: pipeline %u, primitive %u, attachment %u\n",
               rate_features.pipelineFragmentShadingRate,
               rate_features.primitiveFragmentShadingRate,
               rate_features.attachmentFragmentShadingRate);
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR rate_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &rate_props};
        vkGetPhysicalDeviceProperties2(physical, &props2);
        printf("  attachment texel %ux%u .. %ux%u (aspect %u), layered %u, non-trivial combiners %u,"
               " max fragment %ux%u (%u samples), with sample mask %u, with shader depth/stencil "
               "writes %u, with sample interlock %u, with custom sample locations %u, strict "
               "multiply %u\n",
               rate_props.minFragmentShadingRateAttachmentTexelSize.width,
               rate_props.minFragmentShadingRateAttachmentTexelSize.height,
               rate_props.maxFragmentShadingRateAttachmentTexelSize.width,
               rate_props.maxFragmentShadingRateAttachmentTexelSize.height,
               rate_props.maxFragmentShadingRateAttachmentTexelSizeAspectRatio,
               rate_props.layeredShadingRateAttachments,
               rate_props.fragmentShadingRateNonTrivialCombinerOps,
               rate_props.maxFragmentSize.width, rate_props.maxFragmentSize.height,
               rate_props.maxFragmentSizeAspectRatio, rate_props.fragmentShadingRateWithSampleMask,
               rate_props.fragmentShadingRateWithShaderDepthStencilWrites,
               rate_props.fragmentShadingRateWithFragmentShaderInterlock,
               rate_props.fragmentShadingRateWithCustomSampleLocations,
               rate_props.fragmentShadingRateStrictMultiplyCombiner);
        PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR get_rates =
            (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)vkGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
        if (get_rates != NULL) {
            uint32_t rate_count = 0;
            get_rates(physical, &rate_count, NULL);
            VkPhysicalDeviceFragmentShadingRateKHR* rates = calloc(rate_count, sizeof(*rates));
            for (uint32_t i = 0; i < rate_count; ++i) {
                rates[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
            }
            get_rates(physical, &rate_count, rates);
            printf("  rates:");
            for (uint32_t i = 0; i < rate_count; ++i) {
                printf(" %ux%u (samples 0x%x)", rates[i].fragmentSize.width,
                       rates[i].fragmentSize.height, rates[i].sampleCounts);
            }
            printf("\n");
        }
        VkFormatProperties format_props;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UINT, &format_props);
        printf("  R8_UINT as a shading rate attachment: %s\n",
               (format_props.optimalTilingFeatures &
                VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)
                   ? "yes"
                   : "no");
    }
    return 0;
}
