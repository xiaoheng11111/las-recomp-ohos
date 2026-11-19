// HMS 图形加速能力探测实现。
//
// 这份代码回答一个具体问题：这台设备上，鸿蒙的官方图形加速到底能不能用。
// 三个库分别对应：
//   libxengine.so          XEngine Kit（VRS / 超分 / HPS / 光追），扩展名形如 XEG_*
//   libframegeneration.so  Graphics Accelerate Kit 超帧，函数名前缀 HMS_FG_
//   libgame_performance.z.so  性能配置相关
//
// 注意：这里一律用 dlopen 探测，不做链接。原因写在 hms_probe.h。
#include "hms_probe.h"

#include <dlfcn.h>
#include <hilog/log.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kDomain = 0x0000;
constexpr const char *kTag = "HmsProbe";
#define HLOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, __VA_ARGS__)

// 与 xengine/xeg_vulkan_extension.h 中的 XEG_ExtensionProperties 保持内存布局一致。
// 不直接 include 那个头文件，是为了让本模块在不带 HMS SDK 的构建环境里也能编译。
constexpr int kXegNameSize = 256;
struct XegExtensionProperties {
    char extensionName[kXegNameSize];
    uint32_t version;
};

// 真实签名是三参数（physicalDevice, pPropertyCount, pProperties），
// 标准 Vulkan 两段式：先传 null 拿数量，再传数组拿内容。
using PFN_XegEnumerate =
    VkResult (*)(VkPhysicalDevice, uint32_t *, XegExtensionProperties *);

void AppendQuoted(std::string &o, const char *s) {
    o += '"';
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    o += '"';
}

// 打开库；失败时把 dlerror 的原因写进 err。
void *TryOpen(const char *name, std::string &err) {
    HLOGI("dlopen %{public}s 开始", name);
    dlerror();
    void *h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    if (h == nullptr) {
        const char *e = dlerror();
        err = (e != nullptr) ? e : "dlopen 返回 null 且无错误信息";
        HLOGI("dlopen %{public}s 失败: %{public}s", name, err.c_str());
    } else {
        HLOGI("dlopen %{public}s 成功", name);
    }
    return h;
}

std::string ProbeXengine() {
    std::string o = "{";
    std::string err;
    void *h = TryOpen("libxengine.so", err);
    o += "\"lib\":\"libxengine.so\",\"loaded\":";
    o += (h != nullptr) ? "true" : "false";

    if (h == nullptr) {
        o += ",\"error\":";
        AppendQuoted(o, err.c_str());
        o += "}";
        return o;
    }

    // 查询入口签名：VkResult HMS_XEG_EnumerateDeviceExtensionProperties(XEG_ExtensionProperties*)
    // 只有一个参数，约定调用方预分配足够大的数组，函数按顺序填充、遇到空名结束。
    auto fn = reinterpret_cast<PFN_XegEnumerate>(
        dlsym(h, "HMS_XEG_EnumerateDeviceExtensionProperties"));
    o += ",\"hasEnumerate\":";
    o += (fn != nullptr) ? "true" : "false";

    if (fn != nullptr) {
        // XEG 查询需要物理设备句柄：这里临时建一个 instance 枚举第一个设备。
        VkApplicationInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "hms_probe";
        ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &ai;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS && inst) {
            uint32_t dn = 0;
            vkEnumeratePhysicalDevices(inst, &dn, nullptr);
            VkPhysicalDevice phys = VK_NULL_HANDLE;
            if (dn > 0) {
                std::vector<VkPhysicalDevice> devs(dn);
                vkEnumeratePhysicalDevices(inst, &dn, devs.data());
                phys = devs[0];
            }
            HLOGI("XEG 查询用物理设备 %{public}p", (void *)phys);

            if (phys != VK_NULL_HANDLE) {
                uint32_t count = 0;
                const VkResult r1 = fn(phys, &count, nullptr);
                HLOGI("XEG 第一段返回 %{public}d，数量 %{public}u", (int)r1, count);
                if ((r1 == VK_SUCCESS || r1 == VK_INCOMPLETE) && count > 0) {
                    std::vector<XegExtensionProperties> props(count);
                    memset(props.data(), 0,
                           sizeof(XegExtensionProperties) * count);
                    const VkResult r2 = fn(phys, &count, props.data());
                    HLOGI("XEG 第二段返回 %{public}d", (int)r2);
                    o += ",\"count\":";
                    o += std::to_string(count);
                    o += ",\"extensions\":[";
                    for (uint32_t i = 0; i < count; ++i) {
                        if (i != 0) o += ',';
                        o += "{\"name\":";
                        AppendQuoted(o, props[i].extensionName);
                        o += ",\"version\":";
                        o += std::to_string(props[i].version);
                        o += "}";
                    }
                    o += "]";
                }
            }
            vkDestroyInstance(inst, nullptr);
        } else {
            o += ",\"instanceError\":\"vkCreateInstance 失败\"";
        }
    }
    dlclose(h);
    o += "}";
    return o;
}

std::string ProbeFramegen() {
    static const char *kSymbols[] = {
        "HMS_FG_CreateContext_VK",
        "HMS_FG_DestroyContext_VK",
        "HMS_FG_SetAlgorithmMode_VK",
        "HMS_FG_SetResolution_VK",
        "HMS_FG_SetCvvZSemantic_VK",
        "HMS_FG_SetImageFormat_VK",
        "HMS_FG_SetDepthStencilYDirectionInverted_VK",
        "HMS_FG_CreateImage_VK",
        "HMS_FG_DestroyImage_VK",
        "HMS_FG_Activate_VK",
        "HMS_FG_Deactivate_VK",
        "HMS_FG_IsActive_VK",
        "HMS_FG_Dispatch_VK",
        "HMS_FG_SetIntegrationMode_VK",
        "HMS_FG_SetUiPredictionEnabled_VK",
        "HMS_FG_SetTargetFps_VK",
    };
    constexpr std::size_t kTotal = sizeof(kSymbols) / sizeof(kSymbols[0]);

    std::string o = "{";
    std::string err;
    void *h = TryOpen("libframegeneration.so", err);
    o += "\"lib\":\"libframegeneration.so\",\"loaded\":";
    o += (h != nullptr) ? "true" : "false";

    if (h == nullptr) {
        o += ",\"error\":";
        AppendQuoted(o, err.c_str());
        o += "}";
        return o;
    }

    std::size_t found = 0;
    o += ",\"symbols\":{";
    for (std::size_t i = 0; i < kTotal; ++i) {
        dlerror();
        const bool ok = (dlsym(h, kSymbols[i]) != nullptr);
        if (ok) ++found;
        if (i != 0) o += ',';
        AppendQuoted(o, kSymbols[i]);
        o += ':';
        o += ok ? "true" : "false";
    }
    o += "},\"found\":";
    o += std::to_string(found);
    o += ",\"total\":";
    o += std::to_string(kTotal);

    dlclose(h);
    o += "}";
    return o;
}

std::string ProbeSimple(const char *name) {
    std::string o = "{";
    std::string err;
    void *h = TryOpen(name, err);
    o += "\"lib\":";
    AppendQuoted(o, name);
    o += ",\"loaded\":";
    o += (h != nullptr) ? "true" : "false";
    if (h == nullptr) {
        o += ",\"error\":";
        AppendQuoted(o, err.c_str());
    } else {
        dlclose(h);
    }
    o += "}";
    return o;
}

}  // namespace

std::string HmsProbeJson() {
    HLOGI("HmsProbeJson 入口");
    std::string o = "{\"ok\":true,";
    o += "\"xengine\":";
    o += ProbeXengine();
    HLOGI("xengine 段完成");
    o += ",\"framegeneration\":";
    o += ProbeFramegen();
    HLOGI("framegeneration 段完成");
    o += ",\"gamePerformance\":";
    o += ProbeSimple("libgame_performance.z.so");
    o += ",\"iGraphicsCore\":";
    o += ProbeSimple("libiGraphicsCore.huawei.so");
    o += ",\"frameFlow\":";
    o += ProbeSimple("libframeflowload.so");
    o += "}";
    HLOGI("HmsProbeJson 完成");
    return o;
}
