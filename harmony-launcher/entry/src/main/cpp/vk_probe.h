// 鸿蒙原生 Vulkan 探针：不碰 Android 兼容层，直接走 OHOS 的 libvulkan.so。
//
// 这一步只做一件事——回答「鸿蒙原生进程能不能拿到真 GPU」。
// 拿到 physical device 的身份（deviceType / driverID / deviceName）就够了，
// 不需要创建 device，也不需要 surface。
#ifndef VK_PROBE_H
#define VK_PROBE_H

#include <string>

// 返回一段 JSON：{ ok, instanceVersion, devices:[...], verdict, reason }
std::string VkProbeJson();

#endif
