// XComponent 原生渲染入口。
//
// 目的只有一个：证明「鸿蒙原生进程 → OHOS libvulkan → 屏幕」这条链是通的。
// 通了之后，移植层的 VkDevice/管线/VMA 才能搬进来；不通则一切优化都无从谈起。
#pragma once

#include <ace/xcomponent/native_interface_xcomponent.h>

// 绑定 XComponent 并启动演示渲染线程。由 napi_init 在模块加载时调用。
// 回调不在这里注册：同一个 XComponent 的 RegisterCallback 是整体覆盖，
// 统一由 game_entry_attach_xcomp 注册后转发到下面三个函数。
void RenderAttach(OH_NativeXComponent *component);

void RenderOnSurfaceCreated(OH_NativeXComponent *component, void *window);
void RenderOnSurfaceChanged(OH_NativeXComponent *component, void *window);
void RenderOnSurfaceDestroyed(OH_NativeXComponent *component, void *window);

// 把窗口让给游戏：停帧并销毁演示 swapchain / render pass / pipeline。
// 游戏在自己的 surface 上建 swapchain 前必须调用，否则两个 Vulkan 消费者抢同一块窗口。
void RenderHandOff();
