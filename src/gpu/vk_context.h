// Dynamic Vulkan context (Phase 5 M1).
//
// Loads vulkan-1.dll at runtime (no Vulkan SDK / import library — headers
// only) and resolves every entry point through vkGetInstanceProcAddr /
// vkGetDeviceProcAddr, volk-style.  Brings up: instance (extensions from
// glfwGetRequiredInstanceExtensions), a Win32 surface (via
// glfwCreateWindowSurface, which works with GLFW_NO_API windows), one
// physical device with a combined graphics+present queue family, and a
// logical device.
//
// Headless safety: VkContextCreate is only ever called when a GLFW window
// exists; unit tests never touch this code path.
#pragma once
#include "../common/types.h"
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace GPU {

// Every Vulkan entry point we use, resolved dynamically.
struct VkFunctions {
    // Global / instance-level.
    PFN_vkGetInstanceProcAddr                       GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance                            CreateInstance = nullptr;
    PFN_vkDestroyInstance                           DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices                  EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties               GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties    GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties         GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR        GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR   GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR        GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR   GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkDestroySurfaceKHR                         DestroySurfaceKHR = nullptr;
    PFN_vkCreateDevice                              CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr                         GetDeviceProcAddr = nullptr;

    // Device-level (valid only after device creation).
    PFN_vkDestroyDevice                 DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue                GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle                DeviceWaitIdle = nullptr;
    PFN_vkCreateSwapchainKHR            CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR           DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR         GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR           AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR               QueuePresentKHR = nullptr;
    PFN_vkCreateImageView               CreateImageView = nullptr;
    PFN_vkDestroyImageView              DestroyImageView = nullptr;
    PFN_vkCreateImage                   CreateImage = nullptr;
    PFN_vkDestroyImage                  DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements    GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                AllocateMemory = nullptr;
    PFN_vkFreeMemory                    FreeMemory = nullptr;
    PFN_vkBindImageMemory               BindImageMemory = nullptr;
    PFN_vkCreateBuffer                  CreateBuffer = nullptr;
    PFN_vkDestroyBuffer                 DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements   GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory              BindBufferMemory = nullptr;
    PFN_vkMapMemory                     MapMemory = nullptr;
    PFN_vkUnmapMemory                   UnmapMemory = nullptr;
    PFN_vkCreateCommandPool             CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool            DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers        AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer            BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer              EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer            ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier            CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage          CmdCopyBufferToImage = nullptr;
    PFN_vkCmdBlitImage                  CmdBlitImage = nullptr;
    PFN_vkCmdClearColorImage            CmdClearColorImage = nullptr;
    PFN_vkCreateSemaphore               CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore              DestroySemaphore = nullptr;
    PFN_vkCreateFence                   CreateFence = nullptr;
    PFN_vkDestroyFence                  DestroyFence = nullptr;
    PFN_vkWaitForFences                 WaitForFences = nullptr;
    PFN_vkResetFences                   ResetFences = nullptr;
    PFN_vkQueueSubmit                   QueueSubmit = nullptr;
};

struct VkContext {
    void*                   dll = nullptr; // HMODULE for vulkan-1.dll
    VkInstance              instance = VK_NULL_HANDLE;
    VkSurfaceKHR            surface = VK_NULL_HANDLE;
    VkPhysicalDevice        phys = VK_NULL_HANDLE;
    VkDevice                device = VK_NULL_HANDLE;
    u32                     queue_family = 0;
    VkQueue                 queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props = {};
    VkFunctions             fn = {};
    char                    device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

// Creates a context for `window`.  Returns nullptr (with a clear log line) on
// any failure — the caller then stays on the GDI fallback path.
VkContext* VkContextCreate(GLFWwindow* window);
void VkContextDestroy(VkContext* ctx);

// Picks a memory type satisfying `type_bits` + `required` flags.
bool VkFindMemoryType(VkContext* ctx, u32 type_bits, VkMemoryPropertyFlags required,
                      u32* out_index);

} // namespace GPU
