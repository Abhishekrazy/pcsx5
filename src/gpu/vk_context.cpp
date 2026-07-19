// See vk_context.h for the design notes.
#define _CRT_SECURE_NO_WARNINGS
#include "vk_context.h"
#include "../common/log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef CreateSemaphore // windows.h macro collides with VkFunctions::CreateSemaphore
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <vector>
#include <cstring>

namespace GPU {

namespace {

#define VK_LOAD_GLOBAL(ctx, name)                                                        \
    ctx->fn.name = reinterpret_cast<PFN_vk##name>(                                       \
        ctx->fn.GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name));                        \
    if (!ctx->fn.name) {                                                                 \
        LOG_ERROR(GPU, "Vulkan: failed to resolve vk" #name);                            \
        return false;                                                                    \
    }

#define VK_LOAD_INSTANCE(ctx, name)                                                      \
    ctx->fn.name = reinterpret_cast<PFN_vk##name>(                                       \
        ctx->fn.GetInstanceProcAddr(ctx->instance, "vk" #name));                         \
    if (!ctx->fn.name) {                                                                 \
        LOG_ERROR(GPU, "Vulkan: failed to resolve vk" #name);                            \
        return false;                                                                    \
    }

#define VK_LOAD_DEVICE(ctx, name)                                                        \
    ctx->fn.name = reinterpret_cast<PFN_vk##name>(                                       \
        ctx->fn.GetDeviceProcAddr(ctx->device, "vk" #name));                             \
    if (!ctx->fn.name) {                                                                 \
        LOG_ERROR(GPU, "Vulkan: failed to resolve vk" #name);                            \
        return false;                                                                    \
    }

bool LoadRuntime(VkContext* ctx) {
    ctx->dll = reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
    if (!ctx->dll) {
        LOG_WARN(GPU, "Vulkan: vulkan-1.dll not found — staying on GDI fallback.");
        return false;
    }
    ctx->fn.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(reinterpret_cast<HMODULE>(ctx->dll), "vkGetInstanceProcAddr"));
    if (!ctx->fn.GetInstanceProcAddr) {
        LOG_ERROR(GPU, "Vulkan: vkGetInstanceProcAddr not exported by vulkan-1.dll.");
        return false;
    }
    VK_LOAD_GLOBAL(ctx, CreateInstance);
    return true;
}

bool CreateInstanceForWindow(VkContext* ctx) {
    u32 ext_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&ext_count);
    if (!extensions || ext_count == 0) {
        LOG_ERROR(GPU, "Vulkan: glfwGetRequiredInstanceExtensions returned nothing.");
        return false;
    }

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "pcsx5";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "pcsx5";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = extensions;

    const VkResult r = ctx->fn.CreateInstance(&ci, nullptr, &ctx->instance);
    if (r != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan: vkCreateInstance failed (%d).", static_cast<int>(r));
        return false;
    }
    LOG_INFO(GPU, "Vulkan: instance created (%u extensions).", ext_count);
    return true;
}

bool CreateSurfaceForWindow(VkContext* ctx, GLFWwindow* window) {
    // glfwCreateWindowSurface resolves vkCreateWin32SurfaceKHR from our
    // instance; GLFW_NO_API windows carry no client-API surface of their own.
    const VkResult r = glfwCreateWindowSurface(ctx->instance, window, nullptr, &ctx->surface);
    if (r != VK_SUCCESS || ctx->surface == VK_NULL_HANDLE) {
        LOG_ERROR(GPU, "Vulkan: glfwCreateWindowSurface failed (%d).", static_cast<int>(r));
        return false;
    }
    LOG_INFO(GPU, "Vulkan: Win32 surface created.");
    return true;
}

bool PickDeviceAndQueue(VkContext* ctx) {
    VK_LOAD_INSTANCE(ctx, EnumeratePhysicalDevices);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceProperties);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceMemoryProperties);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceSurfaceSupportKHR);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceSurfaceFormatsKHR);
    VK_LOAD_INSTANCE(ctx, GetPhysicalDeviceSurfacePresentModesKHR);
    VK_LOAD_INSTANCE(ctx, DestroySurfaceKHR);
    VK_LOAD_INSTANCE(ctx, CreateDevice);
    VK_LOAD_INSTANCE(ctx, GetDeviceProcAddr);

    u32 count = 0;
    ctx->fn.EnumeratePhysicalDevices(ctx->instance, &count, nullptr);
    if (count == 0) {
        LOG_ERROR(GPU, "Vulkan: no physical devices.");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    ctx->fn.EnumeratePhysicalDevices(ctx->instance, &count, devices.data());

    for (const VkPhysicalDevice dev : devices) {
        u32 qf_count = 0;
        ctx->fn.GetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qf_count);
        ctx->fn.GetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qfs.data());
        for (u32 i = 0; i < qf_count; ++i) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            ctx->fn.GetPhysicalDeviceSurfaceSupportKHR(dev, i, ctx->surface, &present);
            if (!present) continue;

            VkPhysicalDeviceProperties props;
            ctx->fn.GetPhysicalDeviceProperties(dev, &props);
            ctx->phys = dev;
            ctx->queue_family = i;
            std::strncpy(ctx->device_name, props.deviceName, sizeof(ctx->device_name) - 1);
            LOG_INFO(GPU, "Vulkan: selected device '%s' (queue family %u).",
                     ctx->device_name, i);
            return true;
        }
    }
    LOG_ERROR(GPU, "Vulkan: no device with a graphics+present queue family.");
    return false;
}

bool CreateLogicalDevice(VkContext* ctx) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = ctx->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &swapchain_ext;

    const VkResult r = ctx->fn.CreateDevice(ctx->phys, &dci, nullptr, &ctx->device);
    if (r != VK_SUCCESS) {
        LOG_ERROR(GPU, "Vulkan: vkCreateDevice failed (%d).", static_cast<int>(r));
        return false;
    }

    VK_LOAD_DEVICE(ctx, DestroyDevice);
    VK_LOAD_DEVICE(ctx, GetDeviceQueue);
    VK_LOAD_DEVICE(ctx, DeviceWaitIdle);
    VK_LOAD_DEVICE(ctx, CreateSwapchainKHR);
    VK_LOAD_DEVICE(ctx, DestroySwapchainKHR);
    VK_LOAD_DEVICE(ctx, GetSwapchainImagesKHR);
    VK_LOAD_DEVICE(ctx, AcquireNextImageKHR);
    VK_LOAD_DEVICE(ctx, QueuePresentKHR);
    VK_LOAD_DEVICE(ctx, CreateImageView);
    VK_LOAD_DEVICE(ctx, DestroyImageView);
    VK_LOAD_DEVICE(ctx, CreateImage);
    VK_LOAD_DEVICE(ctx, DestroyImage);
    VK_LOAD_DEVICE(ctx, GetImageMemoryRequirements);
    VK_LOAD_DEVICE(ctx, AllocateMemory);
    VK_LOAD_DEVICE(ctx, FreeMemory);
    VK_LOAD_DEVICE(ctx, BindImageMemory);
    VK_LOAD_DEVICE(ctx, CreateBuffer);
    VK_LOAD_DEVICE(ctx, DestroyBuffer);
    VK_LOAD_DEVICE(ctx, GetBufferMemoryRequirements);
    VK_LOAD_DEVICE(ctx, BindBufferMemory);
    VK_LOAD_DEVICE(ctx, MapMemory);
    VK_LOAD_DEVICE(ctx, UnmapMemory);
    VK_LOAD_DEVICE(ctx, CreateCommandPool);
    VK_LOAD_DEVICE(ctx, DestroyCommandPool);
    VK_LOAD_DEVICE(ctx, AllocateCommandBuffers);
    VK_LOAD_DEVICE(ctx, BeginCommandBuffer);
    VK_LOAD_DEVICE(ctx, EndCommandBuffer);
    VK_LOAD_DEVICE(ctx, ResetCommandBuffer);
    VK_LOAD_DEVICE(ctx, CmdPipelineBarrier);
    VK_LOAD_DEVICE(ctx, CmdCopyBufferToImage);
    VK_LOAD_DEVICE(ctx, CmdBlitImage);
    VK_LOAD_DEVICE(ctx, CmdClearColorImage);
    VK_LOAD_DEVICE(ctx, CreateSemaphore);
    VK_LOAD_DEVICE(ctx, DestroySemaphore);
    VK_LOAD_DEVICE(ctx, CreateFence);
    VK_LOAD_DEVICE(ctx, DestroyFence);
    VK_LOAD_DEVICE(ctx, WaitForFences);
    VK_LOAD_DEVICE(ctx, ResetFences);
    VK_LOAD_DEVICE(ctx, QueueSubmit);
    // Phase 5 M3: draw executor entry points.
    VK_LOAD_DEVICE(ctx, CreateShaderModule);
    VK_LOAD_DEVICE(ctx, DestroyShaderModule);
    VK_LOAD_DEVICE(ctx, CreateGraphicsPipelines);
    VK_LOAD_DEVICE(ctx, DestroyPipeline);
    VK_LOAD_DEVICE(ctx, CreatePipelineLayout);
    VK_LOAD_DEVICE(ctx, DestroyPipelineLayout);
    VK_LOAD_DEVICE(ctx, CreateDescriptorSetLayout);
    VK_LOAD_DEVICE(ctx, DestroyDescriptorSetLayout);
    VK_LOAD_DEVICE(ctx, CreateDescriptorPool);
    VK_LOAD_DEVICE(ctx, DestroyDescriptorPool);
    VK_LOAD_DEVICE(ctx, AllocateDescriptorSets);
    VK_LOAD_DEVICE(ctx, FreeDescriptorSets);
    VK_LOAD_DEVICE(ctx, UpdateDescriptorSets);
    VK_LOAD_DEVICE(ctx, CreateSampler);
    VK_LOAD_DEVICE(ctx, DestroySampler);
    VK_LOAD_DEVICE(ctx, CreateRenderPass);
    VK_LOAD_DEVICE(ctx, DestroyRenderPass);
    VK_LOAD_DEVICE(ctx, CreateFramebuffer);
    VK_LOAD_DEVICE(ctx, DestroyFramebuffer);
    VK_LOAD_DEVICE(ctx, CmdBeginRenderPass);
    VK_LOAD_DEVICE(ctx, CmdEndRenderPass);
    VK_LOAD_DEVICE(ctx, CmdBindPipeline);
    VK_LOAD_DEVICE(ctx, CmdBindDescriptorSets);
    VK_LOAD_DEVICE(ctx, CmdBindVertexBuffers);
    VK_LOAD_DEVICE(ctx, CmdBindIndexBuffer);
    VK_LOAD_DEVICE(ctx, CmdDraw);
    VK_LOAD_DEVICE(ctx, CmdDrawIndexed);
    VK_LOAD_DEVICE(ctx, CmdSetViewport);
    VK_LOAD_DEVICE(ctx, CmdSetScissor);
    VK_LOAD_DEVICE(ctx, CmdSetBlendConstants);
    VK_LOAD_DEVICE(ctx, CmdCopyBuffer);
    VK_LOAD_DEVICE(ctx, CmdCopyImage);

    ctx->fn.GetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
    ctx->fn.GetPhysicalDeviceMemoryProperties(ctx->phys, &ctx->mem_props);
    LOG_INFO(GPU, "Vulkan: logical device + graphics/present queue ready.");
    return true;
}

} // namespace

VkContext* VkContextCreate(GLFWwindow* window) {
    if (!window) return nullptr;
    auto* ctx = new VkContext();
    const bool ok = LoadRuntime(ctx) &&
                    CreateInstanceForWindow(ctx) &&
                    CreateSurfaceForWindow(ctx, window) &&
                    PickDeviceAndQueue(ctx) &&
                    CreateLogicalDevice(ctx);
    if (!ok) {
        VkContextDestroy(ctx);
        return nullptr;
    }
    return ctx;
}

void VkContextDestroy(VkContext* ctx) {
    if (!ctx) return;
    if (ctx->device && ctx->fn.DeviceWaitIdle) {
        ctx->fn.DeviceWaitIdle(ctx->device);
    }
    if (ctx->device && ctx->fn.DestroyDevice) {
        ctx->fn.DestroyDevice(ctx->device, nullptr);
    }
    if (ctx->instance && ctx->surface && ctx->fn.DestroySurfaceKHR) {
        ctx->fn.DestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
    }
    if (ctx->instance && ctx->fn.DestroyInstance) {
        ctx->fn.DestroyInstance(ctx->instance, nullptr);
    }
    if (ctx->dll) {
        FreeLibrary(reinterpret_cast<HMODULE>(ctx->dll));
    }
    delete ctx;
}

bool VkFindMemoryType(VkContext* ctx, u32 type_bits, VkMemoryPropertyFlags required,
                      u32* out_index) {
    for (u32 i = 0; i < ctx->mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (ctx->mem_props.memoryTypes[i].propertyFlags & required) == required) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

} // namespace GPU
