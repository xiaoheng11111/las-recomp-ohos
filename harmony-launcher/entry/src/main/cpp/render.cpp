// 鸿蒙原生 Vulkan 渲染：XComponent 表面 → vkCreateSurfaceOHOS → swapchain → 三角形。
//
// 线程模型：
//   ON_SurfaceCreated / Changed / Destroyed 只做「记录意图 + 置标志」，绝不碰 Vulkan。
//   所有 Vulkan 调用都在一个独立渲染线程里串行执行，避免句柄被并发释放。
//
// 同步模型：
//   本轮刻意用 vkQueueWaitIdle 串行，换取零同步 bug。这里验证的是链路而不是吞吐，
//   等移植层接进来之后再换成 per-frame fence。
#include "render.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "spv_tri.h"

namespace {

constexpr unsigned int kDomain = 0x0000;
constexpr const char *kTag = "LasRender";

#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, __VA_ARGS__)
#define LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, __VA_ARGS__)

struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;   // 每个 swapchain image 一个，随 swapchain 一起重建
};

struct Ctx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    Swapchain sc;

    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore semAcq = VK_NULL_HANDLE;
    VkSemaphore semRen = VK_NULL_HANDLE;

    // 回调 → 渲染线程 的交接区
    std::atomic<void *> wantWindow{nullptr};
    std::atomic<bool> surfaceDead{false};
    std::atomic<bool> surfaceAlive{false};   // 渲染线程自己的 surface 状态（跨线程只读这个）
    std::atomic<bool> suspend{false};        // 交给游戏后停止演示绘制
    std::atomic<bool> retrySurface{false};   // present 队列族不匹配，需要重建 device 后重试
    std::atomic<bool> needRebuild{false};
    std::atomic<uint64_t> sizeW{0};
    std::atomic<uint64_t> sizeH{0};

    // 渲染线程
    std::thread worker;
    std::atomic<bool> quit{false};
    bool started = false;

    // 首帧像素读回（诊断用）
    VkBuffer rbBuf = VK_NULL_HANDLE;
    VkDeviceMemory rbMem = VK_NULL_HANDLE;

    // 统计
    uint64_t frameCount = 0;
    uint64_t reportAt = 0;
    std::chrono::steady_clock::time_point reportTime;
};

Ctx g;

bool createDevice();
bool createCommandAndSync();

// ---------------------------------------------------------------- 工具

bool hasInstanceExt(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (std::strcmp(exts[i].extensionName, name) == 0) return true;
    }
    return false;
}

const char *devTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

int devScore(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 100;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 60;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 20;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return -1000;  // 软渲染永不优先
        default: return 10;                              // 未知类型不是排除理由
    }
}

// ---------------------------------------------------------------- 初始化

bool createInstance() {
    if (!hasInstanceExt(VK_KHR_SURFACE_EXTENSION_NAME)) {
        LOGE("instance 缺少 %{public}s", VK_KHR_SURFACE_EXTENSION_NAME);
        return false;
    }
    if (!hasInstanceExt(VK_OHOS_SURFACE_EXTENSION_NAME)) {
        LOGE("instance 缺少 %{public}s —— 该设备不支持鸿蒙原生表面", VK_OHOS_SURFACE_EXTENSION_NAME);
        return false;
    }

    const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "LAS Launcher";
    ai.applicationVersion = 1;
    ai.pEngineName = "las";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateInstance(&ci, nullptr, &g.inst);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateInstance 失败: %{public}d", (int)r);
        return false;
    }
    return true;
}

bool createDevice() {
    // VK_KHR_swapchain 必须显式启用。少了这一行，loader 不会报错，
    // 而是把 vkCreateSwapchainKHR 的函数指针置空后照样返回 VK_SUCCESS，
    // 崩溃推迟到后面几行，日志里只剩一句 "function pointer was NULL"。
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ex(en);
    if (en) vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &en, ex.data());
    bool hasSwapchain = false;
    for (uint32_t i = 0; i < en; ++i) {
        if (std::strcmp(ex[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
    }
    if (!hasSwapchain) {
        LOGE("设备未提供 %{public}s，无法建交换链", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return false;
    }
    LOGI("设备扩展 %{public}u 项，已确认含 %{public}s", en, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = g.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    const char *devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = devExts;

    VkResult r = vkCreateDevice(g.phys, &ci, nullptr, &g.dev);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateDevice 失败: %{public}d", (int)r);
        return false;
    }
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);
    return true;
}

bool createSurface(void *window) {
    VkSurfaceCreateInfoOHOS sci{};
    sci.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
    sci.flags = 0;
    sci.window = static_cast<OHNativeWindow *>(window);

    VkResult r = vkCreateSurfaceOHOS(g.inst, &sci, nullptr, &g.surface);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSurfaceOHOS 失败: %{public}d", (int)r);
        return false;
    }
    LOGI("vkCreateSurfaceOHOS 成功");

    // present 支持必须对着 surface 查。选设备时 surface 还不存在，只能在这里补一次。
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qs.data());
    uint32_t presentFam = UINT32_MAX;
    for (uint32_t j = 0; j < qn; ++j) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, j, g.surface, &ok);
        LOGI("队列族 %{public}u: flags=0x%{public}x count=%{public}u present=%{public}u", j,
             qs[j].queueFlags, qs[j].queueCount, (unsigned)ok);
        if (ok == VK_TRUE && (qs[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qs[j].queueCount > 0) {
            if (presentFam == UINT32_MAX) presentFam = j;
        }
    }
    if (presentFam == UINT32_MAX) {
        LOGE("没有任何图形队列族支持 present，swapchain 无法上屏");
        return false;
    }
    if (presentFam != g.qfam) {
        LOGI("图形队列 %{public}u 不支持 present，改用 %{public}u（重建 device 后重试）",
             g.qfam, presentFam);
        g.qfam = presentFam;
        vkDestroySurfaceKHR(g.inst, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
        g.retrySurface.store(true);
        return false;   // 让上层重建 device + sync 后重试
    }
    LOGI("队列族 %{public}u 同时支持 graphics + present", g.qfam);
    return true;
}

// present 队列族与创建 device 时的队列族不同：必须按新队列族重建 device。
bool recreateDevice() {
    if (g.dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.dev);
        if (g.cmd != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(g.dev, g.pool, 1, &g.cmd);
            g.cmd = VK_NULL_HANDLE;
        }
        if (g.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g.dev, g.pool, nullptr);
            g.pool = VK_NULL_HANDLE;
        }
        if (g.semAcq != VK_NULL_HANDLE) {
            vkDestroySemaphore(g.dev, g.semAcq, nullptr);
            g.semAcq = VK_NULL_HANDLE;
        }
        if (g.semRen != VK_NULL_HANDLE) {
            vkDestroySemaphore(g.dev, g.semRen, nullptr);
            g.semRen = VK_NULL_HANDLE;
        }
        vkDestroyDevice(g.dev, nullptr);
        g.dev = VK_NULL_HANDLE;
    }
    if (!createDevice()) return false;
    if (!createCommandAndSync()) return false;
    return true;
}

void destroySwapchain() {
    for (VkFramebuffer fb : g.sc.fbs) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g.dev, fb, nullptr);
    }
    g.sc.fbs.clear();
    for (VkImageView v : g.sc.views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(g.dev, v, nullptr);
    }
    g.sc.views.clear();
    g.sc.images.clear();
    if (g.sc.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g.dev, g.sc.handle, nullptr);
        g.sc.handle = VK_NULL_HANDLE;
    }
}

bool createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
    if (r != VK_SUCCESS) {
        LOGE("surface capabilities 失败: %{public}d", (int)r);
        return false;
    }

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fn, nullptr);
    if (fn == 0) {
        LOGE("surface 没有可用格式");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fn, fmts.data());

    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < fn; i++) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pick = fmts[i];
            break;
        }
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0 || ext.height == 0 || ext.width == UINT32_MAX) {
        uint32_t w = (uint32_t)g.sizeW.load();
        uint32_t h = (uint32_t)g.sizeH.load();
        if (w == 0 || h == 0) {
            w = 1080;
            h = 1920;
        }
        ext.width = w < caps.minImageExtent.width ? caps.minImageExtent.width : w;
        ext.height = h < caps.minImageExtent.height ? caps.minImageExtent.height : h;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                alpha = (VkCompositeAlphaFlagBitsKHR)bit;
                break;
            }
        }
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = g.surface;
    ci.minImageCount = count;
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(g.dev, &ci, nullptr, &handle);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR 失败: %{public}d (format=%{public}d %{public}ux%{public}u)",
             (int)r, (int)pick.format, ext.width, ext.height);
        return false;
    }

    g.sc.handle = handle;
    g.sc.format = pick.format;
    g.sc.colorSpace = pick.colorSpace;
    g.sc.extent = ext;

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(g.dev, handle, &ic, nullptr);
    if (ic == 0) {
        // loader 在扩展未启用时会返回假成功的句柄，这里必须挡住，否则后面解引用空指针
        LOGE("swapchain 返回 0 张图像，句柄无效（检查 VK_KHR_swapchain 是否启用）");
        vkDestroySwapchainKHR(g.dev, handle, nullptr);
        g.sc.handle = VK_NULL_HANDLE;
        return false;
    }
    g.sc.images.resize(ic);
    vkGetSwapchainImagesKHR(g.dev, handle, &ic, g.sc.images.data());

    g.sc.views.resize(ic, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ic; i++) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = g.sc.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = pick.format;
        vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = 0;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = 1;
        r = vkCreateImageView(g.dev, &vi, nullptr, &g.sc.views[i]);
        if (r != VK_SUCCESS) {
            LOGE("vkCreateImageView[%{public}u] 失败: %{public}d", i, (int)r);
            return false;
        }
    }

    LOGI("swapchain 就绪: %{public}ux%{public}u  format=%{public}d  images=%{public}u",
         ext.width, ext.height, (int)pick.format, ic);
    return true;
}

bool createRenderPass() {
    VkAttachmentDescription color{};
    color.format = g.sc.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    VkResult r = vkCreateRenderPass(g.dev, &ci, nullptr, &g.pass);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateRenderPass 失败: %{public}d", (int)r);
        return false;
    }
    return true;
}

VkShaderModule makeModule(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g.dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

bool createPipeline() {
    VkShaderModule vs = makeModule(kTriVert, kTriVertSize);
    VkShaderModule fs = makeModule(kTriFrag, kTriFragSize);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        LOGE("shader module 创建失败");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkResult r = vkCreatePipelineLayout(g.dev, &li, nullptr, &g.layout);
    if (r != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout 失败: %{public}d", (int)r);
        return false;
    }

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = g.layout;
    ci.renderPass = g.pass;
    ci.subpass = 0;

    r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &g.pipe);
    vkDestroyShaderModule(g.dev, vs, nullptr);
    vkDestroyShaderModule(g.dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines 失败: %{public}d", (int)r);
        return false;
    }
    return true;
}

// 建一个 4x4 的 host-visible 缓冲，用来把 swapchain 图像拉回 CPU 看看到底是不是黑的。
bool ensureReadbackBuffer() {
    if (g.rbBuf != VK_NULL_HANDLE) return true;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 4 * 4 * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &g.rbBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.dev, g.rbBuf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        bool vis = (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        bool coh = (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if ((req.memoryTypeBits & (1u << i)) && vis && coh) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return false;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(g.dev, &ai, nullptr, &g.rbMem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g.dev, g.rbBuf, g.rbMem, 0);
    return true;
}

void dumpFirstPixel() {
    void *p = nullptr;
    if (vkMapMemory(g.dev, g.rbMem, 0, 64, 0, &p) != VK_SUCCESS || !p) return;
    unsigned char *b = (unsigned char *)p;
    // 清屏是不透明色，只要 render pass 真的落盘，alpha 一定是 255。
    // 不是 255 就说明这条渲染指令被驱动吞了 —— 模拟器上就是这样。
    LOGI("首帧像素读回: (B,G,R,A)=%{public}u,%{public}u,%{public}u,%{public}u（仅供参考）",
         b[0], b[1], b[2], b[3]);
    vkUnmapMemory(g.dev, g.rbMem);
}

bool createFramebuffers() {
    g.sc.fbs.assign(g.sc.views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < g.sc.views.size(); i++) {
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = g.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &g.sc.views[i];
        fi.width = g.sc.extent.width;
        fi.height = g.sc.extent.height;
        fi.layers = 1;
        VkResult r = vkCreateFramebuffer(g.dev, &fi, nullptr, &g.sc.fbs[i]);
        if (r != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer[%{public}zu] 失败: %{public}d", i, (int)r);
            return false;
        }
    }
    return true;
}

bool createCommandAndSync() {
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g.qfam;
    if (vkCreateCommandPool(g.dev, &pi, nullptr, &g.pool) != VK_SUCCESS) {
        LOGE("vkCreateCommandPool 失败");
        return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = g.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g.dev, &ai, &g.cmd) != VK_SUCCESS) {
        LOGE("vkAllocateCommandBuffers 失败");
        return false;
    }

    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(g.dev, &si, nullptr, &g.semAcq) != VK_SUCCESS ||
        vkCreateSemaphore(g.dev, &si, nullptr, &g.semRen) != VK_SUCCESS) {
        LOGE("vkCreateSemaphore 失败");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 绘制

void drawFrame() {
    if (g.sc.handle == VK_NULL_HANDLE) return;

    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(g.dev, g.sc.handle, 1000000000ULL, g.semAcq, VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g.needRebuild.store(true);
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR 失败: %{public}d", (int)r);
        g.needRebuild.store(true);
        return;
    }
    if (img >= g.sc.fbs.size() || img >= g.sc.images.size()) {
        LOGE("swapchain image 下标越界: %{public}u / %{public}zu", img, g.sc.fbs.size());
        g.needRebuild.store(true);
        return;
    }

    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cmd, &bi);

    VkClearValue clear{};
    clear.color.float32[0] = 0.043f;
    clear.color.float32[1] = 0.055f;
    clear.color.float32[2] = 0.078f;
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = g.pass;
    rp.framebuffer = g.sc.fbs[img];  // 随 swapchain 预建，帧内零分配
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = g.sc.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(g.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vport{};
    vport.x = 0.0f;
    vport.y = 0.0f;
    vport.width = (float)g.sc.extent.width;
    vport.height = (float)g.sc.extent.height;
    vport.minDepth = 0.0f;
    vport.maxDepth = 1.0f;
    vkCmdSetViewport(g.cmd, 0, 1, &vport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = g.sc.extent;
    vkCmdSetScissor(g.cmd, 0, 1, &scissor);

    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);

    const bool doReadback = false;   // 读回实测不可靠，已停用；画面正确性改用截图验证
    if (doReadback) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g.sc.images[img];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy rgn{};
        rgn.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn.imageSubresource.layerCount = 1;
        rgn.imageExtent = {4, 4, 1};
        vkCmdCopyImageToBuffer(g.cmd, g.sc.images[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g.rbBuf, 1, &rgn);

        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b2.dstAccessMask = 0;
        vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
    }

    vkEndCommandBuffer(g.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.semAcq;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.semRen;

    r = vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        LOGE("vkQueueSubmit 失败: %{public}d", (int)r);
        g.needRebuild.store(true);
        return;
    }
    vkQueueWaitIdle(g.queue);

    VkPresentInfoKHR pi2{};
    pi2.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi2.waitSemaphoreCount = 1;
    pi2.pWaitSemaphores = &g.semRen;
    pi2.swapchainCount = 1;
    pi2.pSwapchains = &g.sc.handle;
    pi2.pImageIndices = &img;

    if (doReadback) dumpFirstPixel();   // 必须在 present 前读，present 后 image 可能被回收

    r = vkQueuePresentKHR(g.queue, &pi2);
    vkQueueWaitIdle(g.queue);

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        g.needRebuild.store(true);
        return;
    }
    if (r != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR 失败: %{public}d", (int)r);
        g.needRebuild.store(true);
        return;
    }

    g.frameCount++;
    if (g.frameCount == 1) {
        LOGI("首帧已呈现 img=%{public}u present=%{public}d extent=%{public}ux%{public}u fmt=%{public}d",
             img, (int)r, g.sc.extent.width, g.sc.extent.height, (int)g.sc.format);
    }
    if (g.frameCount >= g.reportAt) {
        auto now = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(now - g.reportTime).count();
        if (sec > 0.0) {
            LOGI("已呈现 %{public}llu 帧，近 %{public}llu 帧用时 %{public}.2f s → %{public}.1f fps",
                 (unsigned long long)g.frameCount, (unsigned long long)g.reportAt, sec,
                 (double)g.reportAt / sec);
        }
        g.reportTime = now;
        g.reportAt = g.frameCount + 60;
    }
}

// ---------------------------------------------------------------- 线程

void teardownSurface() {
    if (g.dev == VK_NULL_HANDLE) return;
    g.surfaceAlive.store(false);
    vkDeviceWaitIdle(g.dev);
    destroySwapchain();
    if (g.pipe != VK_NULL_HANDLE) {
        vkDestroyPipeline(g.dev, g.pipe, nullptr);
        g.pipe = VK_NULL_HANDLE;
    }
    if (g.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g.dev, g.layout, nullptr);
        g.layout = VK_NULL_HANDLE;
    }
    if (g.pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g.dev, g.pass, nullptr);
        g.pass = VK_NULL_HANDLE;
    }
    if (g.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g.inst, g.surface, nullptr);
        g.surface = VK_NULL_HANDLE;
    }
}

bool bringUpSurface(void *window) {
    if (!createSurface(window)) {
        if (g.retrySurface.exchange(false)) {
            if (recreateDevice()) return bringUpSurface(window);
        }
        teardownSurface();
        return false;
    }
    if (!createSwapchain()) {
        teardownSurface();
        return false;
    }
    if (!createRenderPass() || !createPipeline() || !createFramebuffers()) {
        teardownSurface();
        return false;
    }
    if (!ensureReadbackBuffer()) LOGE("读回缓冲创建失败，跳过像素校验");
    g.reportTime = std::chrono::steady_clock::now();
    g.reportAt = 60;
    g.surfaceAlive.store(true);
    return true;
}

void workerLoop() {
    LOGI("渲染线程启动");

    while (!g.quit.load()) {
        void *win = g.wantWindow.exchange(nullptr);
        if (win != nullptr && !g.suspend.load()) {
            if (g.surface != VK_NULL_HANDLE) teardownSurface();
            if (!bringUpSurface(win)) {
                LOGE("表面初始化失败，等待下一次 onSurfaceCreated");
            }
        }

        if (g.surfaceDead.exchange(false)) {
            teardownSurface();
        }

        if (g.needRebuild.exchange(false) && g.surface != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g.dev);
            destroySwapchain();
            // 格式可能随窗口变化，旧的 pass/pipeline 先销毁，避免泄漏与 fb 引用旧 pass
            if (g.pipe != VK_NULL_HANDLE) {
                vkDestroyPipeline(g.dev, g.pipe, nullptr);
                g.pipe = VK_NULL_HANDLE;
            }
            if (g.layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(g.dev, g.layout, nullptr);
                g.layout = VK_NULL_HANDLE;
            }
            if (g.pass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(g.dev, g.pass, nullptr);
                g.pass = VK_NULL_HANDLE;
            }
            if (!createSwapchain() || !createRenderPass() || !createPipeline() ||
                !createFramebuffers()) {
                LOGE("swapchain 重建失败");
                teardownSurface();
            }
        }

        if (g.suspend.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if (g.surface == VK_NULL_HANDLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        drawFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (g.dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.dev);
    }
    teardownSurface();
    if (g.dev != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g.dev, g.pool, nullptr);
        vkDestroySemaphore(g.dev, g.semAcq, nullptr);
        vkDestroySemaphore(g.dev, g.semRen, nullptr);
        vkDestroyDevice(g.dev, nullptr);
        g.dev = VK_NULL_HANDLE;
    }
    if (g.inst != VK_NULL_HANDLE) {
        vkDestroyInstance(g.inst, nullptr);
        g.inst = VK_NULL_HANDLE;
    }
    LOGI("渲染线程退出，累计呈现 %{public}llu 帧", (unsigned long long)g.frameCount);
}

// ---------------------------------------------------------------- 回调

void onSurfaceCreated(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    g.sizeW.store(w);
    g.sizeH.store(h);
    LOGI("onSurfaceCreated window=%{public}p size=%{public}llux%{public}llu", window,
         (unsigned long long)w, (unsigned long long)h);
    g.wantWindow.store(window);
}

void onSurfaceChanged(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0;
    uint64_t h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    uint64_t ow = g.sizeW.load();
    uint64_t oh = g.sizeH.load();
    g.sizeW.store(w);
    g.sizeH.store(h);
    if (w != ow || h != oh) {
        LOGI("onSurfaceChanged %{public}llux%{public}llu → %{public}llux%{public}llu", (unsigned long long)ow,
             (unsigned long long)oh, (unsigned long long)w, (unsigned long long)h);
        g.needRebuild.store(true);
    }
}

void onSurfaceDestroyed(OH_NativeXComponent *component, void *window) {
    (void)component;
    (void)window;
    LOGI("onSurfaceDestroyed");
    g.surfaceDead.store(true);
}

}  // namespace

void RenderAttach(OH_NativeXComponent *component) {
    if (component == nullptr) {
        LOGE("RenderAttach: component 为空");
        return;
    }
    if (g.started) return;

    if (!createInstance()) return;
    // 设备枚举需要 surface 已存在才能查 present 支持，所以先用占位入口：
    // 这里先选设备（不校验 present），surface 建立后再校正。
    // 鸿蒙上 graphics 队列通常同时支持 present，风险已由创建失败日志兜住。
    {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
        if (n == 0) {
            LOGE("没有物理设备");
            return;
        }
        std::vector<VkPhysicalDevice> list(n);
        vkEnumeratePhysicalDevices(g.inst, &n, list.data());

        int best = -100000;
        VkPhysicalDevice bestDev = VK_NULL_HANDLE;
        uint32_t bestQ = 0;
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(list[i], &p);
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(list[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(list[i], &qn, qs.data());
            uint32_t q0 = UINT32_MAX;
            for (uint32_t j = 0; j < qn; j++) {
                if ((qs[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qs[j].queueCount) {
                    q0 = j;
                    break;
                }
            }
            if (q0 == UINT32_MAX) continue;
            int s = devScore(p.deviceType);
            LOGI("设备 [%{public}u] %{public}s type=%{public}s score=%{public}d", i, p.deviceName,
                 devTypeName(p.deviceType), s);
            if (s > best) {
                best = s;
                bestDev = list[i];
                bestQ = q0;
            }
        }
        if (bestDev == VK_NULL_HANDLE) {
            LOGE("没有可用图形队列");
            return;
        }
        g.phys = bestDev;
        g.qfam = bestQ;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(bestDev, &p);
        LOGI("选定设备: %{public}s (%{public}s)", p.deviceName, devTypeName(p.deviceType));
    }

    if (!createDevice()) return;
    if (!createCommandAndSync()) return;

    g.started = true;
    g.worker = std::thread(workerLoop);
    // 回调统一由 game_entry_attach_xcomp 注册（RegisterCallback 是整体覆盖，不能注册两次）
    LOGI("渲染线程已启动，等待 surface 回调");
}

void RenderOnSurfaceCreated(OH_NativeXComponent *component, void *window) {
    onSurfaceCreated(component, window);
}

void RenderOnSurfaceChanged(OH_NativeXComponent *component, void *window) {
    onSurfaceChanged(component, window);
}

void RenderOnSurfaceDestroyed(OH_NativeXComponent *component, void *window) {
    onSurfaceDestroyed(component, window);
}

void RenderHandOff() {
    if (!g.started) return;
    LOGI("handoff: 停止演示渲染，把窗口交给游戏");
    g.suspend.store(true);
    g.surfaceDead.store(true);
    for (int i = 0; i < 100 && g.surfaceAlive.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
