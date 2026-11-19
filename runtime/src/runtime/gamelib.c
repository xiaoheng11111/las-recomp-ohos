// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

// GAMELIB_ID comes from -DGAMELIB_ID at build time (build-gamelib.sh / Makefile).
// When not defined, gamelib.h falls back to "dev" (its official default), which is
// what our arm64 liblasgame.so is built with. Do NOT hardcode a platform-specific id
// here: a hardcoded x86_64 id makes arm64 runtime reject a matching arm64 game lib.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    LOG("[gamelib] id_matches: %s (want %s)", lib, GAMELIB_ID);
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    LOG("[gamelib] id file content: '%s' match=%d", id, !strcmp(id, GAMELIB_ID));
    return !strcmp(id, GAMELIB_ID);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    // OHOS: 游戏库作为 HAP 原生库打包（libs/<abi>/liblasgame.so）——官方 namespace 只认裸 soname
    snprintf(out[n++], 1100, "%s", "liblasgame.so");
    const char *env = NULL;
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_GLOBAL);  // 游戏库 UND(rt_dispatch/g_fnhook/f_*)要从移植层全局域解析
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        LOG("[gamelib] tables mismatch: t=%p abi=%u (want %d) id=%s (want %s)",
            (void *)t, t ? (unsigned)t->abi : 0u, GAMELIB_ABI,
            t && t->id ? t->id : "(null)", GAMELIB_ID);
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        LOG("[gamelib] load called: game_dir=%s", game_dir);
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            bool soname = !strchr(c[i], '/');  // 裸 soname：linker namespace 按名搜索，不查文件系统
            LOG("[gamelib] candidate[%d]: %s (soname=%d)", i, c[i], soname);
            if (!soname && !exists(c[i])) continue;
            bool given = false;  // env 已弃用  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!soname && !given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            LOG("[gamelib] opening %s", c[i]);
            g_tables = open_lib(c[i]);
            if (g_tables)
                for (uint32_t m = 0; m < g_tables->nmods; m++)
                    LOG("[gamelib] %s: %s, %llu functions\n", c[i], g_tables->mods[m].name,
                        (unsigned long long)*g_tables->mods[m].count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
