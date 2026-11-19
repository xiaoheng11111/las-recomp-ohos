// 鸿蒙原生 Vulkan 探针实现。
//
// 判据与 probe/vkprobe.py 完全一致，两端结果可直接对照：
//   SOFTWARE  deviceType==CPU / driverID∈{SWIFTSHADER,LLVMPIPE,SC_EMULATION} / 名字含软渲染关键字
//   HARDWARE  deviceType∈{INTEGRATED,DISCRETE} 且驱动不是软渲染
//   UNKNOWN   VIRTUAL_GPU / OTHER

#include "vk_probe.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *DeviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "OTHER";
    }
}

const char *DriverIdName(int id) {
    switch (id) {
        case 1: return "AMD_PROPRIETARY";
        case 2: return "AMD_OPEN_SOURCE";
        case 3: return "MESA_RADV";
        case 4: return "NVIDIA_PROPRIETARY";
        case 5: return "INTEL_PROPRIETARY_WINDOWS";
        case 6: return "INTEL_OPEN_SOURCE_MESA";
        case 7: return "IMAGINATION_PROPRIETARY";
        case 8: return "QUALCOMM_PROPRIETARY";
        case 9: return "ARM_PROPRIETARY";
        case 10: return "GOOGLE_SWIFTSHADER";
        case 11: return "GGP_PROPRIETARY";
        case 12: return "BROADCOM_PROPRIETARY";
        case 13: return "MESA_LLVMPIPE";
        case 14: return "MOLTENVK";
        case 15: return "COREAVI_PROPRIETARY";
        case 16: return "JUICE_PROPRIETARY";
        case 17: return "VERISILICON_PROPRIETARY";
        case 18: return "MESA_TURNIP";
        case 19: return "MESA_V3DV";
        case 20: return "MESA_PANVK";
        case 21: return "SAMSUNG_PROPRIETARY";
        case 22: return "MESA_VENUS";
        case 23: return "MESA_DOZEN";
        case 24: return "MESA_NVK";
        case 25: return "IMAGINATION_OPEN_SOURCE_MESA";
        case 26: return "MESA_HONEYKRISP";
        case 27: return "VULKAN_SC_EMULATION_ON_VULKAN";
        default: return "UNKNOWN";
    }
}

const char *VendorName(unsigned v) {
    switch (v) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x5143: return "Qualcomm";
        case 0x13B5: return "ARM";
        case 0x1010: return "Imagination";
        case 0x1AE0: return "Google";
        case 0x19E5: return "Huawei";
        case 0x1AF4: return "virtio";
        default: return "";
    }
}

void Esc(std::string &o, const char *s) {
    o += '"';
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += (char)c;
        }
    }
    o += '"';
}

// 这几项直接决定移植层能走哪条提交路径，所以要一个个点名查，不能只看扩展总数。
struct ExtCheck {
    const char *key;
    const char *name;
};

const ExtCheck kKeyExts[] = {
    {"descriptorBuffer", "VK_EXT_descriptor_buffer"},        // 最激进：描述符直接写显存
    {"pushDescriptor", "VK_KHR_push_descriptor"},            // 次优：推进 command buffer
    {"descriptorIndexing", "VK_EXT_descriptor_indexing"},    // bindless 基础
    {"multiDraw", "VK_EXT_multi_draw"},                      // 一次提交多个 draw
    {"dynamicRendering", "VK_KHR_dynamic_rendering"},        // 省掉 renderpass/framebuffer
    {"bufferDeviceAddress", "VK_KHR_buffer_device_address"},
    {"timelineSemaphore", "VK_KHR_timeline_semaphore"},
    {"sync2", "VK_KHR_synchronization2"},
    {"drawIndirectCount", "VK_KHR_draw_indirect_count"},
    {"vertexInputDynamicState", "VK_EXT_vertex_input_dynamic_state"},
};

bool ContainsICase(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            ++i;
        }
        if (i == nl) return true;
    }
    return false;
}

}  // namespace

std::string VkProbeJson() {
    std::string o;
    o.reserve(1024);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "harmony-launcher";
    app.applicationVersion = 1;
    app.pEngineName = "none";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    bool downgraded = false;
    if (r != VK_SUCCESS) {
        // 有的设备只报 1.0/1.1，别在 instance 这关就死掉
        app.apiVersion = VK_API_VERSION_1_0;
        r = vkCreateInstance(&ci, nullptr, &inst);
        downgraded = (r == VK_SUCCESS);
    }
    if (r != VK_SUCCESS) {
        o += "{\"ok\":false,\"error\":";
        o += '"';
        o += "vkCreateInstance failed, VkResult=";
        o += std::to_string((int)r);
        o += '"';
        o += "}";
        return o;
    }

    // instance 版本
    uint32_t iv = VK_API_VERSION_1_0;
    auto enumVer = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
    if (enumVer) enumVer(&iv);

    char ivs[32];
    snprintf(ivs, sizeof ivs, "%u.%u.%u", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv),
             VK_API_VERSION_PATCH(iv));

    o += "{\"ok\":true,\"instanceVersion\":";
    Esc(o, ivs);
    o += ",\"downgradedTo1_0\":";
    o += downgraded ? "true" : "false";

    // instance 层：鸿蒙把图形加速做成 layer 注入（APK 里那两个加载失败的 .so 就是）。
    // 原生进程能枚举到它们，就有机会主动启用。
    {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> layers(ln);
        if (ln) vkEnumerateInstanceLayerProperties(&ln, layers.data());
        o += ",\"instanceLayers\":[";
        for (uint32_t i = 0; i < ln; ++i) {
            if (i) o += ',';
            Esc(o, layers[i].layerName);
        }
        o += "],\"instanceLayerCount\":";
        o += std::to_string(ln);
    }

    {
        uint32_t en = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ex(en);
        if (en) vkEnumerateInstanceExtensionProperties(nullptr, &en, ex.data());
        bool hasOhosSurface = false;
        for (uint32_t i = 0; i < en; ++i) {
            if (!strcmp(ex[i].extensionName, VK_OHOS_SURFACE_EXTENSION_NAME)) hasOhosSurface = true;
        }
        o += ",\"instanceExtCount\":";
        o += std::to_string(en);
        o += ",\"hasOhosSurface\":";
        o += hasOhosSurface ? "true" : "false";
    }

    auto gpp2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    if (!gpp2)
        gpp2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst,
                                                                        "vkGetPhysicalDeviceProperties2KHR");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(inst, &n, devs.data());

    o += ",\"deviceCount\":";
    o += std::to_string(n);
    o += ",\"devices\":[";

    const char *finalVerdict = "UNKNOWN";
    std::string finalReason = "没有可用的 Vulkan 物理设备";
    std::string finalName;
    bool haveHardware = false;

    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);

        // 必须有图形队列，否则这个设备对渲染没意义
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        if (qn) vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs.data());
        bool graphics = false;
        for (uint32_t k = 0; k < qn; ++k)
            if ((qs[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qs[k].queueCount) graphics = true;

        int did = -1;
        char dname[256] = {0};
        char dinfo[256] = {0};
        if (gpp2) {
            VkPhysicalDeviceDriverProperties dp{};
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2{};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &dp;
            gpp2(devs[i], &p2);
            did = (int)dp.driverID;
            memcpy(dname, dp.driverName, sizeof dname - 1);
            memcpy(dinfo, dp.driverInfo, sizeof dinfo - 1);
        }

        // ---- 判定
        const char *v = "UNKNOWN";
        std::string why;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            v = "SOFTWARE";
            why = "deviceType=CPU，物理设备本身是 CPU 实现";
        } else if (did == 10 || did == 13 || did == 27) {
            v = "SOFTWARE";
            why = std::string("driverID=") + DriverIdName(did) + " 是软渲染驱动";
        } else if (ContainsICase(p.deviceName, "swiftshader") || ContainsICase(p.deviceName, "llvmpipe") ||
                   ContainsICase(p.deviceName, "lavapipe") || ContainsICase(p.deviceName, "softpipe")) {
            v = "SOFTWARE";
            why = "设备名含软渲染关键字";
        } else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                   p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            v = "HARDWARE";
            why = "deviceType 为独立/集成 GPU，驱动非软渲染";
        } else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
            why = "VIRTUAL_GPU，可能透传也可能软栅格";
        } else {
            why = "deviceType=OTHER，驱动未正确分类";
        }

        if (i) o += ',';
        o += "{\"index\":";
        o += std::to_string(i);
        o += ",\"name\":";
        Esc(o, p.deviceName);
        o += ",\"deviceType\":";
        Esc(o, DeviceTypeName(p.deviceType));
        o += ",\"deviceTypeValue\":";
        o += std::to_string((int)p.deviceType);
        o += ",\"vendorID\":";
        o += std::to_string(p.vendorID);
        o += ",\"vendorName\":";
        Esc(o, VendorName(p.vendorID));
        o += ",\"deviceID\":";
        o += std::to_string(p.deviceID);
        o += ",\"apiVersion\":";
        {
            char b[32];
            snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion),
                     VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
            Esc(o, b);
        }
        o += ",\"driverID\":";
        o += std::to_string(did);
        o += ",\"driverName\":";
        Esc(o, DriverIdName(did));
        o += ",\"driverString\":";
        Esc(o, dname);
        o += ",\"driverInfo\":";
        Esc(o, dinfo);

        // ---- 关键扩展：决定移植层走哪条提交路径
        uint32_t den = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &den, nullptr);
        std::vector<VkExtensionProperties> dex(den);
        if (den) vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &den, dex.data());
        auto has = [&dex, den](const char *nm) {
            for (uint32_t k = 0; k < den; ++k)
                if (!strcmp(dex[k].extensionName, nm)) return true;
            return false;
        };

        o += ",\"deviceExtCount\":";
        o += std::to_string(den);
        o += ",\"ext\":{";
        for (size_t k = 0; k < sizeof(kKeyExts) / sizeof(kKeyExts[0]); ++k) {
            if (k) o += ',';
            Esc(o, kKeyExts[k].key);
            o += ':';
            o += has(kKeyExts[k].name) ? "true" : "false";
        }
        o += '}';

        // ---- 关键格式支持（vkGetPhysicalDeviceFormatProperties 官方查询：Maleoon 能力清单）
        {
            static const struct { VkFormat f; const char *key; } FMTS[] = {
                {VK_FORMAT_D32_SFLOAT_S8_UINT, "fmtD32S8"},
                {VK_FORMAT_D32_SFLOAT, "fmtD32"},
                {VK_FORMAT_D24_UNORM_S8_UINT, "fmtD24S8"},
                {VK_FORMAT_D16_UNORM, "fmtD16"},
                {VK_FORMAT_R8G8B8A8_SRGB, "fmtRGBA8SRGB"},
                {VK_FORMAT_R8G8B8A8_UNORM, "fmtRGBA8UNORM"},
                {VK_FORMAT_B8G8R8A8_UNORM, "fmtBGRA8"},
                {VK_FORMAT_R32G32B32_SFLOAT, "fmtRGB32F"},
                {VK_FORMAT_R16G16_UNORM, "fmtRG16UNORM"},
                {VK_FORMAT_R16G16_SSCALED, "fmtRG16SSCALED"},
                {VK_FORMAT_R8_UNORM, "fmtR8UNORM"},
            };
            o += ",\"formats\":{";
            bool ffirst = true;
            for (unsigned k = 0; k < sizeof(FMTS) / sizeof(FMTS[0]); ++k) {
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(devs[i], FMTS[k].f, &fp);
                bool attach = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                           VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) != 0;
                bool vtx = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
                if (!ffirst) o += ',';
                ffirst = false;
                Esc(o, FMTS[k].key);
                o += "{\"opt\":";
                o += std::to_string((unsigned)fp.optimalTilingFeatures);
                o += ",\"attach\":";
                o += attach ? "true" : "false";
                o += ",\"vtx\":";
                o += vtx ? "true" : "false";
                o += '}';
            }
            o += '}';
        }

        // 给出建议路径：有哪张牌就打哪张，都没有就退回 set 缓存
        const char *fastPath = "set_cache";
        if (has("VK_EXT_descriptor_buffer") && has("VK_KHR_buffer_device_address")) {
            fastPath = "descriptor_buffer";
        } else if (has("VK_KHR_push_descriptor")) {
            fastPath = "push_descriptor";
        } else if (has("VK_EXT_multi_draw")) {
            fastPath = "multi_draw";
        }
        o += ",\"fastPath\":";
        Esc(o, fastPath);

        // 描述符上限：bindless 能塞多少，看这几个数
        {
            VkPhysicalDeviceLimits &L = p.limits;
            o += ",\"limits\":{\"maxPerStageSampledImages\":";
            o += std::to_string(L.maxPerStageDescriptorSampledImages);
            o += ",\"maxPerStageUniformBuffers\":";
            o += std::to_string(L.maxPerStageDescriptorUniformBuffers);
            o += ",\"maxDescriptorSetSampledImages\":";
            o += std::to_string(L.maxDescriptorSetSampledImages);
            o += ",\"maxMemoryAllocationCount\":";
            o += std::to_string(L.maxMemoryAllocationCount);
            o += '}';
        }

        o += ",\"hasGraphicsQueue\":";
        o += graphics ? "true" : "false";
        o += ",\"verdict\":";
        Esc(o, v);
        o += ",\"reason\":";
        Esc(o, why.c_str());
        o += '}';

        if (!haveHardware && strcmp(v, "HARDWARE") == 0) {
            haveHardware = true;
            finalVerdict = "HARDWARE";
            finalReason = why;
            finalName = p.deviceName;
        } else if (!haveHardware && strcmp(finalVerdict, "UNKNOWN") == 0) {
            finalVerdict = v;
            finalReason = why;
            finalName = p.deviceName;
        }
    }

    o += "],\"verdict\":";
    Esc(o, finalVerdict);
    o += ",\"reason\":";
    Esc(o, finalReason.c_str());
    o += ",\"deviceName\":";
    Esc(o, finalName.c_str());
    o += '}';

    vkDestroyInstance(inst, nullptr);
    return o;
}
