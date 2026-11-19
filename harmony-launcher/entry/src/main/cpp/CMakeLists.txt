# 鸿蒙原生启动器 —— 原生渲染库
#
# 这一层不经过 Android 兼容层，直接链接 OHOS 的 libvulkan.so。
# 链路：XComponent -> __NATIVE_XCOMPONENT_OBJ__ -> OHNativeWindow
#       -> vkCreateSurfaceOHOS -> swapchain -> 屏幕
cmake_minimum_required(VERSION 3.5.0)
project(las_launcher)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(entry SHARED
    vk_probe.cpp
    hms_probe.cpp
    game_entry.cpp
    render.cpp
    egl_render.cpp
    napi_init.cpp)

# bionic 兼容垫片：游戏库引用的 __register_atfork / __system_property_get 需要全局可见。
# 用 lashim.so 这个名字替换游戏库里的 libdl.so 依赖（游戏库本身不用 dl* 符号），
# 这样垫片会作为它的直接依赖进入同一条符号查找链。
add_library(lashim SHARED lashim.cpp)
set_target_properties(lashim PROPERTIES PREFIX "" OUTPUT_NAME "shim" SUFFIX ".so")
# 旧名字残留会一起进 HAP，清掉
add_custom_command(TARGET lashim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/liblashim.so)

# 移植层 so 不直接链接：运行时 dlopen（错误可见）。文件仍随 HAP libs 打包（third_party 下复制到 libs 由 build 前脚本完成）。
target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdl3shim
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shaderc-build/glslang-15.1.0/glslang/Include)

# 文件名与 SONAME 必须都等于 liblasrecomp.so：liblasgame.so 的 DT_NEEDED 写的是这个裸名，
# 名字不一致时动态链接器会在加载游戏库时直接报 "library liblasrecomp.so not found"。
add_custom_command(TARGET entry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/liblasrecomp.so
        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/liblasrecomp.so)
# 旧名字的残留会随 HAP 一起打包，清掉避免出现第二份移植层实例
add_custom_command(TARGET entry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/liblasrecomp_ohos_x64.so)
add_custom_command(TARGET entry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/liblasgame.so
        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/liblasgame.so)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libohaudio.so
    libhilog_ndk.z.so
    librawfile.z.so
    libz.so)
