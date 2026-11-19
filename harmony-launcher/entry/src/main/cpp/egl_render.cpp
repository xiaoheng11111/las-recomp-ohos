// EGL 对照实验：在同一个 XComponent 上用 OpenGL ES 画三角形。
//
// 目的不是替代 Vulkan，而是做一次二分定位：
//   EGL 能出画面 → XComponent 用法正确，问题在 Vulkan present 配置
//   EGL 也不出   → XComponent 的接法本身有问题（libraryname / type / 时序）
#include "egl_render.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

constexpr unsigned int kDomain = 0x0000;
constexpr const char *kTag = "LasEgl";

#define ELOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, __VA_ARGS__)
#define ELOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, __VA_ARGS__)

struct EglCtx {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig cfg = nullptr;
    int32_t w = 0;
    int32_t h = 0;

    std::atomic<void *> wantWindow{nullptr};
    std::atomic<bool> dead{false};
    std::thread worker;
    std::atomic<bool> quit{false};
    bool started = false;
    uint64_t frames = 0;
};

EglCtx g;

const char *kVert = R"(#version 300 es
layout(location=0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

const char *kFrag = R"(#version 300 es
precision mediump float;
out vec4 o;
void main() { o = vec4(0.25, 0.72, 0.31, 1.0); }
)";

GLuint makeShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        ELOGE("shader 编译失败: %{public}s", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool makeProgram(GLuint *out) {
    GLuint vs = makeShader(GL_VERTEX_SHADER, kVert);
    GLuint fs = makeShader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(p, sizeof buf, nullptr, buf);
        ELOGE("program 链接失败: %{public}s", buf);
        glDeleteProgram(p);
        return false;
    }
    *out = p;
    return true;
}

bool initEgl(void *window) {
    g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.dpy == EGL_NO_DISPLAY) {
        ELOGE("eglGetDisplay 失败");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g.dpy, &major, &minor)) {
        ELOGE("eglInitialize 失败: 0x%{public}x", eglGetError());
        return false;
    }
    ELOGI("EGL %{public}d.%{public}d", major, minor);

    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                              EGL_RED_SIZE, 8,
                              EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8,
                              EGL_ALPHA_SIZE, 8,
                              EGL_NONE};
    EGLint num = 0;
    if (!eglChooseConfig(g.dpy, cfgAttr, &g.cfg, 1, &num) || num == 0) {
        ELOGE("eglChooseConfig 失败, num=%{public}d", num);
        return false;
    }

    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.ctx = eglCreateContext(g.dpy, g.cfg, EGL_NO_CONTEXT, ctxAttr);
    if (g.ctx == EGL_NO_CONTEXT) {
        ELOGE("eglCreateContext 失败: 0x%{public}x", eglGetError());
        return false;
    }

    g.surf = eglCreateWindowSurface(g.dpy, g.cfg, (EGLNativeWindowType)window, nullptr);
    if (g.surf == EGL_NO_SURFACE) {
        ELOGE("eglCreateWindowSurface 失败: 0x%{public}x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g.dpy, g.surf, g.surf, g.ctx)) {
        ELOGE("eglMakeCurrent 失败: 0x%{public}x", eglGetError());
        return false;
    }
    ELOGI("EGL 表面就绪 %{public}dx%{public}d", g.w, g.h);
    return true;
}

void drawLoop() {
    ELOGI("EGL 渲染线程启动");
    GLuint prog = 0;

    while (!g.quit.load()) {
        void *win = g.wantWindow.exchange(nullptr);
        if (win) {
            if (g.surf != EGL_NO_SURFACE) {
                eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(g.dpy, g.surf);
                g.surf = EGL_NO_SURFACE;
            }
            if (initEgl(win)) {
                prog = 0;
                if (!makeProgram(&prog)) ELOGE("program 创建失败");
            }
        }

        if (g.dead.exchange(false) && g.surf != EGL_NO_SURFACE) {
            eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(g.dpy, g.surf);
            g.surf = EGL_NO_SURFACE;
        }

        if (g.surf == EGL_NO_SURFACE || prog == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        glViewport(0, 0, g.w, g.h);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        static const GLfloat verts[] = {0.0f, 0.72f, -0.72f, -0.62f, 0.72f, -0.62f};
        glUseProgram(prog);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        eglSwapBuffers(g.dpy, g.surf);
        g.frames++;
        if (g.frames == 1) ELOGI("EGL 首帧已交换（品红清屏 + 绿三角）");
        if (g.frames % 60 == 0) ELOGI("EGL 已交换 %{public}llu 帧", (unsigned long long)g.frames);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (g.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.surf != EGL_NO_SURFACE) eglDestroySurface(g.dpy, g.surf);
        if (g.ctx != EGL_NO_CONTEXT) eglDestroyContext(g.dpy, g.ctx);
        eglTerminate(g.dpy);
    }
    ELOGI("EGL 渲染线程退出");
}

void onCreated(OH_NativeXComponent *c, void *window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(c, window, &w, &h);
    g.w = (int32_t)w;
    g.h = (int32_t)h;
    ELOGI("onSurfaceCreated size=%{public}dx%{public}d", g.w, g.h);
    g.wantWindow.store(window);
}

void onChanged(OH_NativeXComponent *c, void *window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(c, window, &w, &h);
    g.w = (int32_t)w;
    g.h = (int32_t)h;
    ELOGI("onSurfaceChanged size=%{public}dx%{public}d", g.w, g.h);
}

void onDestroyed(OH_NativeXComponent *c, void *window) {
    (void)c;
    (void)window;
    ELOGI("onSurfaceDestroyed");
    g.dead.store(true);
}

void onTouch(OH_NativeXComponent *c, void *window) {
    (void)c;
    (void)window;
}

}  // namespace

void EglRenderAttach(OH_NativeXComponent *component) {
    if (component == nullptr || g.started) return;
    g.started = true;
    g.worker = std::thread(drawLoop);

    static OH_NativeXComponent_Callback cb{};
    cb.OnSurfaceCreated = onCreated;
    cb.OnSurfaceChanged = onChanged;
    cb.OnSurfaceDestroyed = onDestroyed;
    cb.DispatchTouchEvent = onTouch;
    OH_NativeXComponent_RegisterCallback(component, &cb);
    ELOGI("EGL 回调已注册");
}
