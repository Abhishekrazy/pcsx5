#include <pcsx5/graphics/vulkan_renderer.h>
#include "frame_shaders.h"
#include "vulkan_renderer_test_api.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>

namespace pcsx5::graphics {
namespace {
struct backend_failure { graphics_error error; };
graphics_error normalize(VkResult result) noexcept {
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return graphics_error::out_of_memory;
    case VK_ERROR_DEVICE_LOST: return graphics_error::device_lost;
    case VK_ERROR_LAYER_NOT_PRESENT: case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT: case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return graphics_error::unsupported;
    case VK_ERROR_INCOMPATIBLE_DRIVER: return graphics_error::unavailable;
    default: return graphics_error::host_failure;
    }
}
void checked(VkResult result) {
    if (result != VK_SUCCESS) throw backend_failure{normalize(result)};
}
template<class T> T info(VkStructureType type) noexcept {
    T value{};
    value.sType = type;
    return value;
}
constexpr VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkShaderStageFlags draw_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
struct draw_constants { std::array<float, 4> positions01, position2_pad, color; };
static_assert(sizeof(draw_constants) == 48);
static_assert(sizeof(rgba8) == 4);

// Every handle is recorded immediately on acquisition, so partial construction
// follows exactly the same cleanup path. A submitted frame cannot be destroyed
// before completion (device loss is the only accepted failed idle result).
struct frame_resources {
    explicit frame_resources(VkDevice owner) noexcept : device(owner) {}
    frame_resources(const frame_resources&) = delete;
    frame_resources& operator=(const frame_resources&) = delete;
    frame_resources(frame_resources&&) = delete;
    frame_resources& operator=(frame_resources&&) = delete;
    ~frame_resources() {
        if (pending) {
            const auto status = vkDeviceWaitIdle(device);
            if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) std::terminate();
        }
        if (mapped) vkUnmapMemory(device, buffer_memory);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
        if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (pass) vkDestroyRenderPass(device, pass, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (image_memory) vkFreeMemory(device, image_memory, nullptr);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (buffer_memory) vkFreeMemory(device, buffer_memory, nullptr);
    }
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory image_memory{};
    VkImageView view{};
    VkBuffer buffer{};
    VkDeviceMemory buffer_memory{};
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    VkShaderModule vertex{}, fragment{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkCommandPool pool{};
    VkFence fence{};
    void* mapped{};
    bool pending{};
};

class vulkan_renderer final : public renderer {
public:
    explicit vulkan_renderer(detail::vulkan_failure_api failures = {}) noexcept : failures_(failures) {}
    ~vulkan_renderer() override {
        static_cast<void>(close());
        if (failures_.after_destroy) failures_.after_destroy(failures_.context, validation_errors());
    }
    vulkan_renderer(const vulkan_renderer&) = delete;
    vulkan_renderer& operator=(const vulkan_renderer&) = delete;
    vulkan_renderer(vulkan_renderer&&) = delete;
    vulkan_renderer& operator=(vulkan_renderer&&) = delete;
    void initialize(vulkan_options options);
    graphics_capabilities capabilities() const noexcept override { return caps_; }
    std::uint32_t validation_errors() const noexcept override { return errors_.load(); }
    graphics_result<rendered_image> render(const frame_commands& frame) noexcept override;
    graphics_result<void> close() noexcept override;
private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* context) noexcept {
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
            auto& count = static_cast<vulkan_renderer*>(context)->errors_;
            auto previous = count.load();
            while (previous != std::numeric_limits<std::uint32_t>::max() &&
                !count.compare_exchange_weak(previous, previous + 1)) {}
            if (data && data->pMessage) std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
        }
        return VK_FALSE;
    }
    std::uint32_t memory_type(std::uint32_t mask, VkMemoryPropertyFlags required) const;
    rendered_image draw_frame(const frame_commands& frame);
    void make_targets(frame_resources& resources, extent2d extent);
    void make_pipeline(frame_resources& resources, extent2d extent);
    VkInstance instance_{};
    VkDebugUtilsMessengerEXT messenger_{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t family_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    graphics_capabilities caps_{};
    std::atomic<std::uint32_t> errors_{};
    bool closed_{};
    detail::vulkan_failure_api failures_{};
    void before(detail::failure_site site) const {
        if (failures_.before) checked(failures_.before(failures_.context, site));
    }
};

void vulkan_renderer::initialize(vulkan_options options) {
    constexpr const char* layer_name = "VK_LAYER_KHRONOS_validation";
    std::vector<const char*> extension_names;
    std::uint32_t available_count{};
    checked(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr));
    std::vector<VkExtensionProperties> available(available_count);
    checked(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data()));
    const bool portability = std::any_of(available.begin(), available.begin() + available_count,
        [](const auto& entry) { return std::strcmp(entry.extensionName,
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0; });
    if (portability) extension_names.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (options.require_validation) {
        extension_names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extension_names.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        std::uint32_t count{};
        checked(vkEnumerateInstanceLayerProperties(&count, nullptr));
        if (!count) throw backend_failure{graphics_error::unsupported};
        std::vector<VkLayerProperties> layers(count);
        checked(vkEnumerateInstanceLayerProperties(&count, layers.data()));
        if (std::none_of(layers.begin(), layers.begin() + count, [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        })) throw backend_failure{graphics_error::unsupported};
        checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        if (!count) throw backend_failure{graphics_error::unsupported};
        std::vector<VkExtensionProperties> extensions(count);
        checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()));
        if (std::none_of(extensions.begin(), extensions.begin() + count, [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        })) throw backend_failure{graphics_error::unsupported};
        // Explicit layers' extensions are not necessarily in the global list.
        checked(vkEnumerateInstanceExtensionProperties(layer_name, &count, nullptr));
        if (!count) throw backend_failure{graphics_error::unsupported};
        extensions.resize(count);
        checked(vkEnumerateInstanceExtensionProperties(layer_name, &count, extensions.data()));
        if (std::none_of(extensions.begin(), extensions.begin() + count, [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0;
        })) throw backend_failure{graphics_error::unsupported};
    }
    auto debug = info<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_message;
    debug.pUserData = this;
    constexpr VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    auto validation = info<VkValidationFeaturesEXT>(VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
    validation.pNext = &debug;
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &synchronization;
    auto app = info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "PCSX5 synthetic offscreen HAL";
    app.apiVersion = VK_API_VERSION_1_1;
    auto create = info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    create.pApplicationInfo = &app;
    create.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    create.enabledExtensionCount = static_cast<std::uint32_t>(extension_names.size());
    create.ppEnabledExtensionNames = extension_names.data();
    if (options.require_validation) {
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = &layer_name;
        create.enabledExtensionCount = static_cast<std::uint32_t>(extension_names.size());
        create.ppEnabledExtensionNames = extension_names.data();
        create.pNext = &validation;
    }
    checked(vkCreateInstance(&create, nullptr, &instance_));
    if (options.require_validation) {
        const auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        destroy_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (!make || !destroy_messenger_) throw backend_failure{graphics_error::unsupported};
        checked(make(instance_, &debug, nullptr, &messenger_));
    }
    std::uint32_t count{};
    checked(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (options.device_index >= count) throw backend_failure{graphics_error::unavailable};
    std::vector<VkPhysicalDevice> devices(count);
    checked(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));
    if (options.device_index >= count) throw backend_failure{graphics_error::unavailable};
    physical_ = devices[options.device_index];
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    if (!count) throw backend_failure{graphics_error::unsupported};
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
    bool found{};
    for (std::uint32_t index = 0; index < count; ++index) {
        if (families[index].queueCount && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family_ = index; found = true; break;
        }
    }
    if (!found) throw backend_failure{graphics_error::unsupported};
    VkImageFormatProperties format{};
    checked(vkGetPhysicalDeviceImageFormatProperties(physical_, target_format, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &format));
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) throw backend_failure{graphics_error::unsupported};
    caps_ = {std::min({4096u, format.maxExtent.width, properties.limits.maxFramebufferWidth,
        properties.limits.maxViewportDimensions[0]}),
        std::min({4096u, format.maxExtent.height, properties.limits.maxFramebufferHeight,
        properties.limits.maxViewportDimensions[1]}), 4096};
    if (!caps_.max_width || !caps_.max_height || properties.limits.maxPushConstantsSize < sizeof(draw_constants) ||
        !(format.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) throw backend_failure{graphics_error::unsupported};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    const float priority = 1.0f;
    auto queue = info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue.queueFamilyIndex = family_; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    auto device = info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    checked(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> device_extensions(count);
    checked(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, device_extensions.data()));
    // The extension name is used without enabling beta feature structures.
    constexpr const char* subset = "VK_KHR_portability_subset";
    if (std::any_of(device_extensions.begin(), device_extensions.begin() + count,
        [](const auto& entry) { return std::strcmp(entry.extensionName, "VK_KHR_portability_subset") == 0; })) {
        device.enabledExtensionCount = 1;
        device.ppEnabledExtensionNames = &subset;
    }
    checked(vkCreateDevice(physical_, &device, nullptr, &device_));
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    if (validation_errors()) throw backend_failure{graphics_error::validation_failure};
}

std::uint32_t vulkan_renderer::memory_type(std::uint32_t mask, VkMemoryPropertyFlags required) const {
    for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & required) == required) return i;
    }
    throw backend_failure{graphics_error::unsupported};
}

void vulkan_renderer::make_targets(frame_resources& r, extent2d extent) {
    auto image = info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image.imageType = VK_IMAGE_TYPE_2D; image.format = target_format;
    image.extent = {extent.width, extent.height, 1}; image.mipLevels = 1; image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checked(vkCreateImage(device_, &image, nullptr, &r.image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, r.image, &requirements);
    auto allocation = info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, 0);
    before(detail::failure_site::image_allocation);
    checked(vkAllocateMemory(device_, &allocation, nullptr, &r.image_memory));
    checked(vkBindImageMemory(device_, r.image, r.image_memory, 0));
    auto view = info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = r.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = target_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    checked(vkCreateImageView(device_, &view, nullptr, &r.view));
    auto buffer = info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer.size = static_cast<VkDeviceSize>(extent.width) * extent.height * sizeof(rgba8);
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checked(vkCreateBuffer(device_, &buffer, nullptr, &r.buffer));
    vkGetBufferMemoryRequirements(device_, r.buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    checked(vkAllocateMemory(device_, &allocation, nullptr, &r.buffer_memory));
    checked(vkBindBufferMemory(device_, r.buffer, r.buffer_memory, 0));
    VkAttachmentDescription attachment{};
    attachment.format = target_format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    auto pass = info<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    pass.attachmentCount = 1; pass.pAttachments = &attachment; pass.subpassCount = 1; pass.pSubpasses = &subpass;
    pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size()); pass.pDependencies = dependencies.data();
    checked(vkCreateRenderPass(device_, &pass, nullptr, &r.pass));
    auto framebuffer = info<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer.renderPass = r.pass; framebuffer.attachmentCount = 1; framebuffer.pAttachments = &r.view;
    framebuffer.width = extent.width; framebuffer.height = extent.height; framebuffer.layers = 1;
    checked(vkCreateFramebuffer(device_, &framebuffer, nullptr, &r.framebuffer));
}

void vulkan_renderer::make_pipeline(frame_resources& r, extent2d extent) {
    auto shader = info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = std::size(detail::frame_vert_spv) * sizeof(std::uint32_t); shader.pCode = std::data(detail::frame_vert_spv);
    checked(vkCreateShaderModule(device_, &shader, nullptr, &r.vertex));
    shader.codeSize = std::size(detail::frame_frag_spv) * sizeof(std::uint32_t); shader.pCode = std::data(detail::frame_frag_spv);
    checked(vkCreateShaderModule(device_, &shader, nullptr, &r.fragment));
    const VkPushConstantRange range{draw_stages, 0, sizeof(draw_constants)};
    auto layout = info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
    checked(vkCreatePipelineLayout(device_, &layout, nullptr, &r.layout));
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = r.vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = r.fragment;
    const auto vertices = info<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = info<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {extent.width, extent.height}};
    auto viewport_state = info<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_state.viewportCount = 1; viewport_state.pViewports = &viewport; viewport_state.scissorCount = 1; viewport_state.pScissors = &scissor;
    auto raster = info<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    auto samples = info<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    auto blend = info<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
    auto pipeline = info<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipeline.stageCount = 2; pipeline.pStages = stages.data(); pipeline.pVertexInputState = &vertices;
    pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport_state;
    pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &samples; pipeline.pColorBlendState = &blend;
    pipeline.layout = r.layout; pipeline.renderPass = r.pass;
    checked(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &r.pipeline));
}

rendered_image vulkan_renderer::draw_frame(const frame_commands& frame) {
    rendered_image output{frame.extent, std::vector<rgba8>(static_cast<std::size_t>(frame.extent.width) * frame.extent.height)};
    frame_resources r(device_);
    make_targets(r, frame.extent);
    make_pipeline(r, frame.extent);
    auto pool = info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pool.queueFamilyIndex = family_;
    checked(vkCreateCommandPool(device_, &pool, nullptr, &r.pool));
    auto allocate = info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocate.commandPool = r.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    VkCommandBuffer command{};
    checked(vkAllocateCommandBuffers(device_, &allocate, &command));
    auto begin = info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(command, &begin));
    const auto normalized = [](rgba8 color) {
        return std::array<float, 4>{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    };
    VkClearValue clear{};
    const auto clear_color = normalized(frame.clear);
    std::copy(clear_color.begin(), clear_color.end(), clear.color.float32);
    auto pass = info<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    pass.renderPass = r.pass; pass.framebuffer = r.framebuffer; pass.renderArea.extent = {frame.extent.width, frame.extent.height};
    pass.clearValueCount = 1; pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    for (const auto& triangle : frame.triangles) {
        const auto coordinate = [&](point2d point) {
            return std::array<float, 2>{2.0f * static_cast<float>(point.x) / static_cast<float>(frame.extent.width) - 1.0f,
                2.0f * static_cast<float>(point.y) / static_cast<float>(frame.extent.height) - 1.0f};
        };
        const auto a = coordinate(triangle.vertices[0]);
        const auto b = coordinate(triangle.vertices[1]);
        const auto c = coordinate(triangle.vertices[2]);
        const draw_constants constants{{a[0], a[1], b[0], b[1]}, {c[0], c[1], 0, 0}, normalized(triangle.color)};
        vkCmdPushConstants(command, r.layout, draw_stages, 0, sizeof(constants), &constants);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {frame.extent.width, frame.extent.height, 1};
    vkCmdCopyImageToBuffer(command, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &copy);
    auto barrier = info<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = r.buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
        0, nullptr, 1, &barrier, 0, nullptr);
    checked(vkEndCommandBuffer(command));
    const auto fence = info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    checked(vkCreateFence(device_, &fence, nullptr, &r.fence));
    auto submit = info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    before(detail::failure_site::queue_submission);
    r.pending = true; // Also drain conservatively if native submission fails.
    checked(vkQueueSubmit(queue_, 1, &submit, r.fence));
    checked(vkWaitForFences(device_, 1, &r.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()));
    r.pending = false;
    checked(vkMapMemory(device_, r.buffer_memory, 0, VK_WHOLE_SIZE, 0, &r.mapped));
    // Whole dedicated allocation satisfies nonCoherentAtomSize alignment, and
    // invalidation is legal for coherent memory too. Fence+barrier precede it.
    auto mapped = info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    mapped.memory = r.buffer_memory; mapped.size = VK_WHOLE_SIZE;
    checked(vkInvalidateMappedMemoryRanges(device_, 1, &mapped));
    std::memcpy(output.pixels.data(), r.mapped, output.pixels.size() * sizeof(rgba8));
    return output;
}

graphics_result<rendered_image> vulkan_renderer::render(const frame_commands& frame) noexcept {
    if (closed_ || !device_) return std::unexpected(graphics_error::invalid_state);
    const auto valid = validate_frame(frame, caps_);
    if (!valid) return std::unexpected(valid.error());
    if (validation_errors()) return std::unexpected(graphics_error::validation_failure);
    try {
        auto output = draw_frame(frame); // frame RAII teardown completes before checking diagnostics
        if (validation_errors()) return std::unexpected(graphics_error::validation_failure);
        return output;
    } catch (const backend_failure& failure) { return std::unexpected(failure.error); }
    catch (const std::bad_alloc&) { return std::unexpected(graphics_error::out_of_memory); }
    catch (const std::length_error&) { return std::unexpected(graphics_error::out_of_memory); }
}

graphics_result<void> vulkan_renderer::close() noexcept {
    closed_ = true;
    VkResult idle = VK_SUCCESS;
    if (device_) {
        idle = vkDeviceWaitIdle(device_);
        // There is no asynchronous public work; frames drain before returning.
        vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE;
    }
    if (messenger_) { destroy_messenger_(instance_, messenger_, nullptr); messenger_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    if (validation_errors()) return std::unexpected(graphics_error::validation_failure);
    if (idle != VK_SUCCESS) return std::unexpected(normalize(idle));
    return {};
}
} // namespace

graphics_result<std::unique_ptr<renderer>> make_vulkan_renderer(vulkan_options options) noexcept {
    return detail::make_vulkan_renderer_with_failures(options, {});
}

graphics_result<std::unique_ptr<renderer>> detail::make_vulkan_renderer_with_failures(
    vulkan_options options, vulkan_failure_api failures) noexcept {
    try {
        auto result = std::make_unique<vulkan_renderer>(failures);
        result->initialize(options);
        return result;
    } catch (const backend_failure& failure) { return std::unexpected(failure.error); }
    catch (const std::bad_alloc&) { return std::unexpected(graphics_error::out_of_memory); }
    catch (const std::length_error&) { return std::unexpected(graphics_error::out_of_memory); }
}
} // namespace pcsx5::graphics
