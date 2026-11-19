// 探测鸿蒙 HMS 图形加速能力：XEngine Kit（libxengine.so）与
// Graphics Accelerate Kit 超帧（libframegeneration.so）。
//
// 为什么要用 dlopen 而不是直接链接：
//   这两个库属于 HMS 侧，不在 OpenHarmony SDK 的 sysroot 里，而且模拟器上根本不装。
//   直接链接会让 libentry.so 在模拟器上加载失败（DT_NEEDED 找不到），整个应用起不来。
//   dlopen 让「库不存在」变成一个可报告的探测结果，而不是崩溃。
#pragma once

#include <string>

// 返回 JSON，字段见 hms_probe.cpp 里的说明。
std::string HmsProbeJson();
