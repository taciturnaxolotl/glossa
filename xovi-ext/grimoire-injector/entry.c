#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <dlfcn.h>
#include <unistd.h>
#include "xovi.h"

void registerGrimoire();
extern char *program_invocation_short_name;

/* Global state captured by hooks */
static void *g_qmlEngine = NULL;
static void *g_framebufferAddr = NULL;
static int g_fbWidth = 0, g_fbHeight = 0, g_fbBpl = 0, g_fbFormat = 0;
static void *g_sceneController = NULL;

/* Hot-reload support */
static volatile sig_atomic_t g_reloadRequested = 0;

static void sigusr1Handler(int sig) {
    (void)sig;
    g_reloadRequested = 1;
}

/* Exports for GrimoireInjector.cpp to access */
void *grimoire_getQmlEngine(void) { return g_qmlEngine; }
void *grimoire_getSceneController(void) { return g_sceneController; }
int grimoire_checkReload(void) {
    if (g_reloadRequested) {
        g_reloadRequested = 0;
        return 1;
    }
    return 0;
}
void *grimoire_getFramebuffer(int *w, int *h, int *bpl, int *fmt) {
    if (w) *w = g_fbWidth;
    if (h) *h = g_fbHeight;
    if (bpl) *bpl = g_fbBpl;
    if (fmt) *fmt = g_fbFormat;
    return g_framebufferAddr;
}

/* Hook: QImage::QImage(uchar*, int w, int h, int bpl, Format, CleanupFn, void*) */
void override$_ZN6QImageC1EPhiiiNS_6FormatEPFvPvES2_(
    void *self, void *data, int w, int h, int bpl, int fmt, void *cleanup, void *info)
{
    bool isRM2 = (w == 1404 && h == 1872 &&
                  ((bpl == 2808 && fmt == 7) || (bpl == 5616 && fmt == 4)));
    if (isRM2 && g_framebufferAddr == NULL) {
        g_framebufferAddr = data;
        g_fbWidth = w;
        g_fbHeight = h;
        g_fbBpl = bpl;
        g_fbFormat = fmt;
        fprintf(stderr, "[grimoire] Framebuffer captured: %p %dx%d bpl=%d fmt=%d\n",
                data, w, h, bpl, fmt);
    }
    
    /* Screenshot trigger is now handled by the watch thread in GrimoireInjector */
    
    $_ZN6QImageC1EPhiiiNS_6FormatEPFvPvES2_(self, data, w, h, bpl, fmt, cleanup, info);
}

/*
 * Try to hook SceneController::addDrawingLine at runtime via dlsym.
 * The symbol may be stripped, so we check before hooking.
 */
typedef void (*AddDrawingLineFn)(void *closure, void *scene);
static AddDrawingLineFn g_origAddDrawingLine = NULL;

static void hookedAddDrawingLine(void *closure, void *scene) {
    void **captured = (void **)closure;
    void *sc_this = captured[0];

    fprintf(stderr, "[grimoire] addDrawingLine called! closure=%p scene=%p this=%p\n",
            closure, scene, sc_this);
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "[grimoire]   closure[%d] = %p\n", i, captured[i]);
    }

    if (g_sceneController == NULL) {
        g_sceneController = sc_this;
        fprintf(stderr, "[grimoire] Captured SceneController @ %p\n", sc_this);
    }

    if (g_origAddDrawingLine) {
        g_origAddDrawingLine(closure, scene);
    }
}

void _xovi_construct() {
    if (strstr(program_invocation_short_name, "worker") != NULL) {
        return;
    }

    /* Write proof-of-life to a file since stderr may be swallowed */
    FILE *log = fopen("/tmp/grimoire_ext.log", "a");
    if (log) {
        fprintf(log, "[%d] _xovi_construct called for %s\n",
                getpid(), program_invocation_short_name);
        fclose(log);
    }

    printf("[grimoire] Main process (%s), registering hooks + singleton\n",
           program_invocation_short_name);

    /* Install SIGUSR1 handler for hot-reload signaling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1Handler;
    sigaction(SIGUSR1, &sa, NULL);
    fprintf(stderr, "[grimoire] SIGUSR1 handler installed (PID %d)\n", getpid());

    registerGrimoire();
}
