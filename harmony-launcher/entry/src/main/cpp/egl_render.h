// EGL 对照实验入口。只用于定位 XComponent 显示链路，不参与正式渲染。
#pragma once

#include <ace/xcomponent/native_interface_xcomponent.h>

void EglRenderAttach(OH_NativeXComponent *component);
