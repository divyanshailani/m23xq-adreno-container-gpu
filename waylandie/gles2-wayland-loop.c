/* Sustained GLES2 render loop over the WayLandIE bridge via freedreno/kgsl.
 * Renders + swaps continuously for N seconds, like a minimal compositor. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xdg-shell-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static int configured = 0, closed = 0;

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { xdg_wm_base_ping };

static void toplevel_configure(void *data, struct xdg_toplevel *t,
        int32_t w, int32_t h, struct wl_array *states) { }
static void toplevel_close(void *data, struct xdg_toplevel *t) { closed = 1; }
static const struct xdg_toplevel_listener toplevel_listener = { toplevel_configure, toplevel_close };

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
        const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
    else if (strcmp(interface, "xdg_wm_base") == 0) wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name) { }
static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

int main(int argc, char **argv) {
    int run_seconds = argc > 1 ? atoi(argv[1]) : 120;
    int w = argc > 2 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    display = wl_display_connect(NULL);
    if (!display) { printf("FAIL connect\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !wm_base) { printf("FAIL globals comp=%p wm=%p\n", (void*)compositor, (void*)wm_base); return 1; }
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "gles2-loop");
    wl_surface_commit(surface);

    while (!configured && wl_display_dispatch(display) >= 0 && !closed) { }

    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)display);
    if (!eglInitialize(edpy, NULL, NULL)) { printf("FAIL eglInit\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg; EGLint n;
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    if (!eglChooseConfig(edpy, attrs, &cfg, 1, &n) || n < 1) { printf("FAIL config\n"); return 1; }
    EGLSurface surf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)wl_egl_window_create(surface, w, h), NULL);
    EGLint cattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, cattrs);
    eglMakeCurrent(edpy, surf, surf, ctx);
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    printf("version: %s\n", glGetString(GL_VERSION));

    struct timespec ts_start; clock_gettime(CLOCK_MONOTONIC, &ts_start);
    long frames = 0;
    while (!closed) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - ts_start.tv_sec) + (now.tv_nsec - ts_start.tv_nsec) / 1e9;
        if (elapsed >= run_seconds) break;
        float t = (float)elapsed;
        glClearColor(0.5f + 0.5f * sinf(t), 0.2f, 0.5f + 0.5f * cosf(t * 0.7f), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(edpy, surf);
        frames++;
        if (frames % 600 == 0) printf("frames=%ld t=%.1f\n", frames, elapsed);
    }
    printf("DONE frames=%ld seconds=%d\n", frames, run_seconds);
    return 0;
}
