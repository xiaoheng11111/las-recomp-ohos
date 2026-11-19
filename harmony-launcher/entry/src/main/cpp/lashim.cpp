// Android/bionic 兼容垫片：游戏库（liblasgame.so）是按 NDK 构建的，
// 引用了两个 OHOS libc 不导出的符号。本库单独 dlopen(RTLD_GLOBAL) 进入全局符号域，
// 让游戏库在加载时能解析到它们。
#include <pthread.h>
#include <string.h>

extern "C" __attribute__((visibility("default"))) int __register_atfork(
    void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso_handle) {
    int r = pthread_atfork(prepare, parent, child);
    (void)dso_handle;
    return r;
}

// bionic 的签名：返回属性值长度，value 最多 92 字节（含结尾 0）。
extern "C" __attribute__((visibility("default"))) int __system_property_get(const char *name,
                                                                           char *value) {
    (void)name;
    if (value != nullptr) {
        value[0] = '\0';
    }
    return 0;
}
