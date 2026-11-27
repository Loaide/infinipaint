#include "bench.hpp"

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "vk_vert.inc"
#include "vk_frag.inc"

namespace {
template<class T> T vkInfo(VkStructureType type) {
    T v{};
    v.sType = type;
    return v;
}
void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(where) + ": VkResult " + std::to_string(r));
    }
}
template<class T, class F> std::vector<T> enumerate(F get, const char* label) {
    for (int i = 0; i < 4; ++i) {
        uint32_t count = 0;
        check(get(&count, static_cast<T*>(nullptr)), label);
        if (!count) return {};
        std::vector<T> out(count);
        const auto result = get(&count, out.data());
        if (result == VK_INCOMPLETE) continue;
        check(result, label);
        out.resize(count);
        return out;
    }
    throw std::runtime_error(std::string(label) + ": unstable enumeration");
}
bool has(const std::vector<VkExtensionProperties>& ext, const char* name) {
    return std::any_of(ext.begin(), ext.end(), [name](const auto& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}
constexpr uint32_t kSlots = 2;
constexpr VkPipelineStageFlags kColorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkShaderStageFlags kShaders = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
struct Push {
    float color[4];
    float screen[2];
    float pan;
    int32_t mode;
};
static_assert(sizeof(Push) == 32);
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize capacity{};
    void* mapping{};
    bool coherent{};
};
struct Slot {
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkSemaphore acquire{};
    Buffer staging;
    int timerSeq{};
};
struct Target {
    VkImage image{};
    VkImageView view{};
    VkFramebuffer fb{};
    VkSemaphore ready{}; // Only reused after this same image is reacquired.
};

class Vulkan final : public Backend {
public:
    ~Vulkan() override;
    void init(ANativeWindow*, const Scene&, int, bool);
    Metrics frame(const Frame&, std::vector<uint8_t>*) override;

private:
    ANativeWindow* window{};
    const Scene* scene{};
    bool compact{};
    int prefix{}, resident{};
    VkDeviceSize stride{};
    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, timestampBits{}, nextSlot{};
    float timestampPeriod{};
    PFN_vkGetPastPresentationTimingGOOGLE getPast{};
    VkSwapchainKHR swapchain{};
    VkFormat format{};
    VkExtent2D extent{};
    VkRenderPass pass{};
    VkPipelineLayout layout{};
    std::array<VkPipeline, 3> pipelines{}; // indexed by shader mode
    VkShaderModule vert{}, frag{};
    VkCommandPool pool{};
    VkQueryPool queries{};
    std::array<Slot, kSlots> slots{};
    std::vector<Target> targets;
    Buffer geometry, quad, readback;

    void createDevice();
    void createSwapchain();
    void createPass();
    void createCommands();
    void alloc(Buffer&, VkDeviceSize, VkBufferUsageFlags, VkMemoryPropertyFlags,
               VkMemoryPropertyFlags, const char*);
    void drop(Buffer&);
    void mappedSync(const Buffer&, bool flush);
    void upload(Slot&, int first, int count);
    void begin(Slot&);
    void submit(Slot&, VkSemaphore ready);
    void wait(Slot&);
    void harvest(uint32_t i);
    void pastPresents();
    void draw(VkCommandBuffer, const Target&, const Frame&, Metrics&);
    void finishImage(VkCommandBuffer, VkImage, bool copy);
};

Vulkan::~Vulkan() {
    if (device) {
        if (vkDeviceWaitIdle(device) == VK_SUCCESS) {
            try {
                for (uint32_t i = 0; i < kSlots; ++i) harvest(i);
                pastPresents();
            } catch (...) {
                BLOG("vk teardown: pending telemetry not fully collected");
            }
        }
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (queries) vkDestroyQueryPool(device, queries, nullptr);
        for (auto& s : slots) {
            if (s.fence) vkDestroyFence(device, s.fence, nullptr);
            if (s.acquire) vkDestroySemaphore(device, s.acquire, nullptr);
            drop(s.staging);
        }
        for (auto& t : targets) {
            if (t.ready) vkDestroySemaphore(device, t.ready, nullptr);
            if (t.fb) vkDestroyFramebuffer(device, t.fb, nullptr);
            if (t.view) vkDestroyImageView(device, t.view, nullptr);
        }
        for (auto p : pipelines) if (p) vkDestroyPipeline(device, p, nullptr);
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (pass) vkDestroyRenderPass(device, pass, nullptr);
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        drop(readback);
        drop(quad);
        drop(geometry);
        vkDestroyDevice(device, nullptr);
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    if (window) ANativeWindow_release(window);
}

void Vulkan::createDevice() {
    const auto instExt = enumerate<VkExtensionProperties>([](auto* n, auto* p) {
        return vkEnumerateInstanceExtensionProperties(nullptr, n, p);
    }, "vk instance extensions");
    std::vector<const char*> names{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const bool props2 = has(instExt, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (props2) names.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    auto app = vkInfo<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "InfiniPaint matched benchmark";
    app.apiVersion = VK_API_VERSION_1_1; // SPIR-V compiled for Vulkan 1.1
    auto ci = vkInfo<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(names.size());
    ci.ppEnabledExtensionNames = names.data();
    check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    auto android = vkInfo<VkAndroidSurfaceCreateInfoKHR>(VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR);
    android.window = window;
    check(vkCreateAndroidSurfaceKHR(instance, &android, nullptr, &surface), "vkCreateAndroidSurfaceKHR");

    VkPhysicalDeviceProperties props{};
    bool timing = false, budget = false;
    for (const auto candidate : enumerate<VkPhysicalDevice>([&](auto* n, auto* p) {
        return vkEnumeratePhysicalDevices(instance, n, p);
    }, "vk physical devices")) {
        VkPhysicalDeviceProperties candidateProps{};
        vkGetPhysicalDeviceProperties(candidate, &candidateProps);
        if (candidateProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const auto ext = enumerate<VkExtensionProperties>([&](auto* n, auto* p) {
            return vkEnumerateDeviceExtensionProperties(candidate, nullptr, n, p);
        }, "vk device extensions");
        if (!has(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
        for (uint32_t j = 0; j < n; ++j) {
            if (!families[j].queueCount || !(families[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, j, surface, &present), "vk surface support");
            if (!present || (physical && timestampBits >= families[j].timestampValidBits)) continue;
            physical = candidate;
            family = j;
            timestampBits = families[j].timestampValidBits;
            props = candidateProps;
            timing = has(ext, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
            budget = props2 && has(ext, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }
    if (!physical) throw std::runtime_error("vk: no native graphics+present queue");
    timestampPeriod = props.limits.timestampPeriod;
    const float priority = 1.f;
    auto q = vkInfo<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (timing) names.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    if (budget) names.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    auto dc = vkInfo<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &q;
    dc.enabledExtensionCount = static_cast<uint32_t>(names.size());
    dc.ppEnabledExtensionNames = names.data();
    check(vkCreateDevice(physical, &dc, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, family, 0, &queue);
    if (timing) {
        getPast = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
            vkGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));
        if (!getPast) throw std::runtime_error("vk: display timing entry point missing");
    }

    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    auto b = vkInfo<VkPhysicalDeviceMemoryBudgetPropertiesEXT>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
    if (budget) {
        const auto getMemory2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2KHR>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
        if (!getMemory2) throw std::runtime_error("vk: memory properties2 entry point missing");
        auto m = vkInfo<VkPhysicalDeviceMemoryProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2);
        m.pNext = &b;
        getMemory2(physical, &m);
    }
    BLOG("vk device=%s api=%#x driver=%#x queue=%u timestampBits=%u periodNs=%.6f displayTiming=%d budget=%d",
         props.deviceName, props.apiVersion, props.driverVersion, family, timestampBits,
         timestampPeriod, timing, budget);
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        const auto& h = memory.memoryHeaps[i];
        BLOG("vk heap=%u flags=%#x size=%llu budget=%llu usage=%llu budgetAvailable=%d",
             i, h.flags, static_cast<unsigned long long>(h.size),
             static_cast<unsigned long long>(b.heapBudget[i]),
             static_cast<unsigned long long>(b.heapUsage[i]), budget);
    }
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        BLOG("vk type=%u heap=%u flags=%#x", i, memory.memoryTypes[i].heapIndex,
             memory.memoryTypes[i].propertyFlags);
    }
}

void Vulkan::createSwapchain() {
    VkSurfaceCapabilitiesKHR c{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &c), "vk surface capabilities");
    const int32_t w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
    BLOG("vk surface window=%dx%d current=%ux%u min=%ux%u max=%ux%u images=%u..%u "
         "layers=%u transform=%#x supported=%#x alpha=%#x usage=%#x",
         w, h, c.currentExtent.width, c.currentExtent.height, c.minImageExtent.width,
         c.minImageExtent.height, c.maxImageExtent.width, c.maxImageExtent.height, c.minImageCount,
         c.maxImageCount, c.maxImageArrayLayers, static_cast<unsigned>(c.currentTransform),
         c.supportedTransforms, c.supportedCompositeAlpha, c.supportedUsageFlags);
    if (w <= 0 || h <= 0 || c.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        !(c.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
        throw std::runtime_error("vk: window must support and currently use identity orientation");
    }
    extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    constexpr uint32_t variable = std::numeric_limits<uint32_t>::max();
    if ((c.currentExtent.width != variable && c.currentExtent.width != extent.width) ||
        (c.currentExtent.height != variable && c.currentExtent.height != extent.height) ||
        extent.width < c.minImageExtent.width || extent.width > c.maxImageExtent.width ||
        extent.height < c.minImageExtent.height || extent.height > c.maxImageExtent.height) {
        throw std::runtime_error("vk: swapchain/window size mismatch");
    }
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((c.supportedUsageFlags & usage) != usage) throw std::runtime_error("vk: surface lacks color/readback usage");
    const auto formats = enumerate<VkSurfaceFormatKHR>([&](auto* n, auto* p) {
        return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, p);
    }, "vk surface formats");
    const auto modes = enumerate<VkPresentModeKHR>([&](auto* n, auto* p) {
        return vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, n, p);
    }, "vk present modes");
    for (const auto& f : formats) BLOG("vk surface format=%d colorSpace=%d", f.format, f.colorSpace);
    for (const auto m : modes) BLOG("vk surface presentMode=%d", m);
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) == modes.end()) {
        throw std::runtime_error("vk: FIFO not advertised");
    }
    for (const auto f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        if (!std::any_of(formats.begin(), formats.end(), [f, &formats](const auto& s) {
            return s.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                   (s.format == f || (formats.size() == 1 && s.format == VK_FORMAT_UNDEFINED));
        })) continue;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) continue;
        format = f;
        break;
    }
    if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("vk: no blendable RGBA8/BGRA8 UNORM surface");
    VkCompositeAlphaFlagBitsKHR alpha{};
    for (const auto a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (c.supportedCompositeAlpha & a) { alpha = a; break; }
    }
    if (!alpha) throw std::runtime_error("vk: no surface composite alpha mode");
    auto info = vkInfo<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    info.surface = surface;
    info.minImageCount = std::max(c.minImageCount, kSlots + 1);
    if (c.maxImageCount) info.minImageCount = std::min(info.minImageCount, c.maxImageCount);
    info.imageFormat = format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_FALSE; // Keep occluded pixels defined for correctness capture.
    check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
    const auto images = enumerate<VkImage>([&](auto* n, auto* p) {
        return vkGetSwapchainImagesKHR(device, swapchain, n, p);
    }, "vk swapchain images");
    if (images.empty()) throw std::runtime_error("vk: empty swapchain");
    targets.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i) targets[i].image = images[i];
    BLOG("vk swapchain format=%d alpha=%#x extent=%ux%u images=%zu slots=%u",
         format, static_cast<unsigned>(alpha), extent.width, extent.height, images.size(), kSlots);
}

void Vulkan::createPass() {
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    const VkSubpassDependency acquireDep{VK_SUBPASS_EXTERNAL, 0, kColorStage, kColorStage, 0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    auto r = vkInfo<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    r.attachmentCount = 1;
    r.pAttachments = &attachment;
    r.subpassCount = 1;
    r.pSubpasses = &sub;
    r.dependencyCount = 1;
    r.pDependencies = &acquireDep;
    check(vkCreateRenderPass(device, &r, nullptr, &pass), "vkCreateRenderPass");

    auto iv = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = format;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto fb = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    fb.renderPass = pass;
    fb.attachmentCount = 1;
    fb.width = extent.width;
    fb.height = extent.height;
    fb.layers = 1;
    const auto sem = vkInfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    for (auto& t : targets) {
        iv.image = t.image;
        check(vkCreateImageView(device, &iv, nullptr, &t.view), "vkCreateImageView");
        fb.pAttachments = &t.view;
        check(vkCreateFramebuffer(device, &fb, nullptr, &t.fb), "vkCreateFramebuffer");
        check(vkCreateSemaphore(device, &sem, nullptr, &t.ready), "vk present semaphore");
    }

    const VkPushConstantRange range{kShaders, 0, sizeof(Push)};
    auto l = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    l.pushConstantRangeCount = 1;
    l.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &l, nullptr, &layout), "vkCreatePipelineLayout");
    auto sm = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    sm.codeSize = sizeof(vk_vert);
    sm.pCode = vk_vert;
    check(vkCreateShaderModule(device, &sm, nullptr, &vert), "vk vertex shader");
    sm.codeSize = sizeof(vk_frag);
    sm.pCode = vk_frag;
    check(vkCreateShaderModule(device, &sm, nullptr, &frag), "vk fragment shader");
    auto vs = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName = "main";
    auto fs = vs;
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    const VkPipelineShaderStageCreateInfo stages[]{vs, fs};

    constexpr uint32_t f32 = sizeof(float);
    const VkVertexInputBindingDescription bindings[]{
        {0, static_cast<uint32_t>(stride), compact ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX},
        {1, 2u * f32, VK_VERTEX_INPUT_RATE_VERTEX}};
    const uint32_t off = compact ? 0u : 2u * f32;
    const VkVertexInputAttributeDescription attrs[]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, compact ? 1u : 0u, VK_FORMAT_R32G32_SFLOAT, compact ? 0u : 2u * f32},
        {2, 0, VK_FORMAT_R32_SFLOAT, off + 2u * f32},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, off + 3u * f32},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, off + 5u * f32}};
    auto vi = vkInfo<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vi.vertexBindingDescriptionCount = compact ? 2 : 1;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;
    auto ia = vkInfo<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    auto vp = vkInfo<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vp.viewportCount = vp.scissorCount = 1;
    auto rs = vkInfo<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    auto ms = vkInfo<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    auto ds = vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkPipelineColorBlendAttachmentState blend{};
    auto cb = vkInfo<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    auto gp = vkInfo<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout;
    gp.renderPass = pass;
    for (size_t i = 0; i < pipelines.size(); ++i) {
        blend = {};
        blend.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
        if (i == 0) {
            blend.blendEnable = VK_TRUE;
            blend.srcAlphaBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.alphaBlendOp = VK_BLEND_OP_MAX;
        } else if (i == 2) {
            blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelines[i]),
              "vk matched draw pipeline");
    }
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    vert = frag = VK_NULL_HANDLE;
}

void Vulkan::createCommands() {
    auto cp = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = family;
    check(vkCreateCommandPool(device, &cp, nullptr, &pool), "vkCreateCommandPool");
    auto cb = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = kSlots;
    std::array<VkCommandBuffer, kSlots> cmds{};
    check(vkAllocateCommandBuffers(device, &cb, cmds.data()), "vkAllocateCommandBuffers");
    auto f = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const auto sem = vkInfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    for (uint32_t i = 0; i < kSlots; ++i) {
        slots[i].cmd = cmds[i];
        check(vkCreateFence(device, &f, nullptr, &slots[i].fence), "vk slot fence");
        check(vkCreateSemaphore(device, &sem, nullptr, &slots[i].acquire), "vk acquire semaphore");
    }
    if (timestampBits) {
        auto qp = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 2 * kSlots;
        check(vkCreateQueryPool(device, &qp, nullptr, &queries), "vk timestamp pool");
    }
}

void Vulkan::alloc(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, const char* tag) {
    auto bc = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bc.size = size;
    bc.usage = usage;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &bc, nullptr, &b.handle), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, b.handle, &req);
    uint32_t type = memory.memoryTypeCount;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || (flags & required) != required) continue;
        if (type == memory.memoryTypeCount) type = i;
        if ((flags & preferred) == preferred) { type = i; break; }
    }
    if (type == memory.memoryTypeCount) throw std::runtime_error("vk: no compatible memory type");
    auto a = vkInfo<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    a.allocationSize = req.size;
    a.memoryTypeIndex = type;
    check(vkAllocateMemory(device, &a, nullptr, &b.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, b.handle, b.memory, 0), "vkBindBufferMemory");
    b.capacity = size;
    b.coherent = (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapping), "vkMapMemory");
    }
    BLOG("vk buffer=%s bytes=%llu allocation=%llu type=%u", tag, static_cast<unsigned long long>(size),
         static_cast<unsigned long long>(req.size), type);
}

void Vulkan::drop(Buffer& b) {
    if (b.mapping) vkUnmapMemory(device, b.memory);
    if (b.handle) vkDestroyBuffer(device, b.handle, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = {};
}

void Vulkan::mappedSync(const Buffer& b, bool flush) {
    if (b.coherent) return;
    auto range = vkInfo<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    range.memory = b.memory;
    range.size = VK_WHOLE_SIZE;
    if (flush) check(vkFlushMappedMemoryRanges(device, 1, &range), "vk flush");
    else check(vkInvalidateMappedMemoryRanges(device, 1, &range), "vk invalidate");
}

void Vulkan::upload(Slot& slot, int first, int count) {
    const VkDeviceSize unit = compact ? sizeof(Instance) : 6 * sizeof(Vertex);
    const VkDeviceSize offset = first * unit, bytes = count * unit;
    auto& b = slot.staging;
    if (bytes > b.capacity) {
        const auto capacity = std::max(bytes, std::min(scene->items.size() * unit, 2 * b.capacity));
        drop(b);
        alloc(b, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "staging");
    }
    const void* src = compact ? static_cast<const void*>(scene->items.data() + first)
                              : static_cast<const void*>(scene->expanded.data() + 6ull * first);
    std::memcpy(b.mapping, src, static_cast<size_t>(bytes));
    mappedSync(b, true);
    auto barrier = vkInfo<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = geometry.handle;
    barrier.offset = offset;
    barrier.size = bytes;
    if (first < resident) {
        // A sent suffix may replace previously submitted suffix data, but never the prefix.
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    }
    const VkBufferCopy copy{0, offset, bytes};
    vkCmdCopyBuffer(slot.cmd, b.handle, geometry.handle, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void Vulkan::begin(Slot& s) {
    check(vkResetCommandBuffer(s.cmd, 0), "vkResetCommandBuffer");
    auto info = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.cmd, &info), "vkBeginCommandBuffer");
}

void Vulkan::submit(Slot& s, VkSemaphore ready) {
    check(vkResetFences(device, 1, &s.fence), "vkResetFences");
    auto info = vkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    info.commandBufferCount = 1;
    info.pCommandBuffers = &s.cmd;
    if (ready) {
        info.waitSemaphoreCount = info.signalSemaphoreCount = 1;
        info.pWaitSemaphores = &s.acquire;
        info.pWaitDstStageMask = &kColorStage;
        info.pSignalSemaphores = &ready;
    }
    check(vkQueueSubmit(queue, 1, &info, s.fence), "vkQueueSubmit");
}

// Break a stalled run instead of leaving its renderer thread in an unbounded frame wait.
constexpr uint64_t kWaitNs = 5'000'000'000ull;
void Vulkan::wait(Slot& s) {
    check(vkWaitForFences(device, 1, &s.fence, VK_TRUE, kWaitNs), "vk wait slot");
}

void Vulkan::harvest(uint32_t i) {
    auto& s = slots[i];
    if (!s.timerSeq) return;
    // A completed slot is the only place queries are read; no WAIT query flag.
    uint64_t data[4]{};
    const auto r = vkGetQueryPoolResults(device, queries, 2 * i, 2, sizeof(data),
        data, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_NOT_READY) check(r, "vk timestamp results");
    if (r == VK_SUCCESS && data[1] && data[3]) {
        const uint64_t mask = timestampBits == 64 ? ~uint64_t(0) : ((uint64_t(1) << timestampBits) - 1);
        const double ns = double((data[2] - data[0]) & mask) * timestampPeriod;
        if (ns >= 0.0 && ns < double(std::numeric_limits<uint64_t>::max())) {
            gpuResult(s.timerSeq, static_cast<uint64_t>(ns + 0.5));
        }
    } else {
        BLOG("vk timestamp unavailable seq=%d", s.timerSeq);
    }
    s.timerSeq = 0;
}

void Vulkan::pastPresents() {
    if (!getPast || !swapchain) return;
    // The extension returns only timings not previously returned for this chain.
    // Keep the per-frame drain bounded; VK_INCOMPLETE leaves later feedback for the next call.
    std::array<VkPastPresentationTimingGOOGLE, 64> past{};
    uint32_t n = static_cast<uint32_t>(past.size());
    const auto r = getPast(device, swapchain, &n, past.data());
    if (r != VK_INCOMPLETE) check(r, "vkGetPastPresentationTimingGOOGLE");
    for (uint32_t i = 0; i < n; ++i) {
        if (past[i].presentID && past[i].actualPresentTime) {
            presentResult(static_cast<int>(past[i].presentID), static_cast<int64_t>(past[i].actualPresentTime));
        }
    }
}

void Vulkan::draw(VkCommandBuffer cmd, const Target& target, const Frame& f, Metrics& m) {
    VkClearValue clear{};
    clear.color = {{.025f, .055f, .09f, 1.f}};
    auto r = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    r.renderPass = pass;
    r.framebuffer = target.fb;
    r.renderArea = {{0, 0}, extent};
    r.clearValueCount = 1;
    r.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &r, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.f, 0.f, float(extent.width), float(extent.height), 0.f, 1.f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkBuffer buffers[]{geometry.handle, quad.handle};
    const VkDeviceSize offsets[]{0, 0};
    vkCmdBindVertexBuffers(cmd, 0, compact ? 2 : 1, buffers, offsets);
    Push p{{}, {float(extent.width), float(extent.height)}, f.pan, 0};
    for (const auto& s : scene->strokes) {
        const int count = std::min(s.count, f.visible - s.first);
        if (count <= 0) continue;
        const auto x0 = std::clamp<int64_t>(s.x, 0, extent.width);
        const auto y0 = std::clamp<int64_t>(s.y, 0, extent.height);
        const auto x1 = std::clamp<int64_t>(int64_t(s.x) + s.w, 0, extent.width);
        const auto y1 = std::clamp<int64_t>(int64_t(s.y) + s.h, 0, extent.height);
        if (x1 <= x0 || y1 <= y0) continue;
        const VkRect2D scissor{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        std::memcpy(p.color, s.color, sizeof(p.color));
        const auto bind = [&](int mode) {
            p.mode = mode;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[mode]);
            vkCmdPushConstants(cmd, layout, kShaders, 0, sizeof(p), &p);
        };
        bind(1);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        bind(0);
        if (compact) vkCmdDraw(cmd, 6, uint32_t(count), 0, uint32_t(s.first));
        else vkCmdDraw(cmd, 6u * uint32_t(count), 1, 6u * uint32_t(s.first), 0);
        bind(2);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        m.draws += 3;
    }
    vkCmdEndRenderPass(cmd);
}

void Vulkan::finishImage(VkCommandBuffer cmd, VkImage image, bool copy) {
    auto b = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = copy ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = copy ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, kColorStage,
        copy ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
    if (!copy) return;

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
    auto host = vkInfo<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback.handle;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &host, 0, nullptr);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

void Vulkan::init(ANativeWindow* w, const Scene& sc, int initial, bool instanced) {
    if (!w || initial < 0 || size_t(initial) > sc.items.size() ||
        sc.items.size() > std::numeric_limits<uint32_t>::max() / 6u ||
        sc.strokes.size() > size_t(std::numeric_limits<int>::max() / 3) ||
        (!instanced && sc.expanded.size() != 6ull * sc.items.size())) {
        throw std::runtime_error("vk: invalid benchmark scene or window");
    }
    for (const auto& s : sc.strokes) {
        if (s.first < 0 || s.count < 0 || uint64_t(s.first) + uint64_t(s.count) > sc.items.size()) {
            throw std::runtime_error("vk: invalid stroke range");
        }
    }
    ANativeWindow_acquire(w);
    window = w;
    scene = &sc;
    compact = instanced;
    stride = compact ? sizeof(Instance) : sizeof(Vertex);
    prefix = initial;
    createDevice();
    createSwapchain();
    createPass();
    createCommands();
    alloc(geometry, std::max<size_t>(1, sc.items.size()) * (compact ? 1 : 6) * stride,
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "geometry");
    if (compact) {
        alloc(quad, sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "corners");
        std::memcpy(quad.mapping, corners, sizeof(corners));
        mappedSync(quad, true);
    }
    if (initial) {
        auto& s = slots[0];
        begin(s);
        upload(s, 0, initial);
        check(vkEndCommandBuffer(s.cmd), "vk end prefix");
        submit(s, VK_NULL_HANDLE);
        wait(s);
        drop(s.staging);
    }
    resident = initial;
}

Metrics Vulkan::frame(const Frame& f, std::vector<uint8_t>* pixels) {
    if (f.seq <= 0 || f.visible < 0 || f.added < 0 || !std::isfinite(f.pan) ||
        (f.readback && (!pixels || f.timer)) || size_t(f.visible) > scene->items.size() ||
        (f.added && (f.first < prefix || f.first > resident ||
                    uint64_t(f.first) + uint64_t(f.added) > scene->items.size()))) {
        throw std::runtime_error("vk: invalid benchmark frame");
    }
    const int filled = f.added ? std::max(resident, f.first + f.added) : resident;
    if (f.visible > filled) throw std::runtime_error("vk: visible geometry not uploaded");
    if (ANativeWindow_getWidth(window) != int32_t(extent.width) ||
        ANativeWindow_getHeight(window) != int32_t(extent.height)) {
        throw std::runtime_error("vk: window resized during benchmark");
    }
    if (f.readback && !readback.handle) {
        alloc(readback, VkDeviceSize(extent.width) * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "readback");
    }

    const uint32_t i = nextSlot;
    auto& s = slots[i];
    nextSlot = (i + 1) % kSlots;
    Metrics m{};
    uint32_t index = 0;
    auto t = nowNs();
    wait(s);
    check(vkAcquireNextImageKHR(device, swapchain, kWaitNs, s.acquire, VK_NULL_HANDLE, &index),
          "vk acquire (surface may have changed)");
    m.syncNs = nowNs() - t;
    harvest(i);
    pastPresents();
    const Target& target = targets.at(index);

    t = nowNs();
    begin(s);
    const bool timed = f.timer && queries;
    if (timed) {
        vkCmdResetQueryPool(s.cmd, queries, 2 * i, 2);
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 2 * i);
    }
    if (f.added) {
        upload(s, f.first, f.added);
        m.bytes = uint64_t(f.added) * (compact ? sizeof(Instance) : 6 * sizeof(Vertex));
    }
    draw(s.cmd, target, f, m);
    finishImage(s.cmd, target.image, f.readback);
    if (timed) vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 2 * i + 1);
    check(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
    submit(s, target.ready);
    m.submitNs = nowNs() - t;
    if (timed) s.timerSeq = f.seq;
    resident = filled;

    // The runner supplies unique positive seqs; use the seq itself as the WSI ID.
    const VkPresentTimeGOOGLE pt{static_cast<uint32_t>(f.seq), 0};
    auto times = vkInfo<VkPresentTimesInfoGOOGLE>(VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE);
    times.swapchainCount = 1;
    times.pTimes = &pt;
    auto present = vkInfo<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    present.pNext = getPast ? &times : nullptr;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &target.ready;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    t = nowNs();
    const auto status = vkQueuePresentKHR(queue, &present);
    m.presentCallNs = nowNs() - t;
    check(status, "vk present (surface may have changed)");
    if (f.readback) {
        wait(s);
        mappedSync(readback, false);
        pixels->resize(static_cast<size_t>(readback.capacity));
        std::memcpy(pixels->data(), readback.mapping, pixels->size());
        if (format == VK_FORMAT_B8G8R8A8_UNORM) {
            for (size_t p = 0; p < pixels->size(); p += 4) std::swap((*pixels)[p], (*pixels)[p + 2]);
        }
    }
    return m;
}
} // namespace

std::unique_ptr<Backend> openVK(ANativeWindow* window, const Scene& scene, int initial, bool compact) {
    auto backend = std::make_unique<Vulkan>();
    backend->init(window, scene, initial, compact);
    return backend;
}
