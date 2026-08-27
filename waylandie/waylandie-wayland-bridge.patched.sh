#!/bin/sh
# Minimal Wayland dmabuf -> Android dmabuf-present bridge.
# Non-dmabuf client buffers are rejected; the fullscreen game path must stay
# on the AdrenoTools/Vulkan zero-copy presenter.

set -eu

BRIDGE_LOCAL_SOCKET="${BRIDGE_LOCAL_SOCKET:-waylandie.display.bridge.v1}"
FRAME_COUNT="${FRAME_COUNT:-12}"
FRAME_INTERVAL_MS="${FRAME_INTERVAL_MS:-16}"
CLIENT_WIDTH="${CLIENT_WIDTH:-2688}"
CLIENT_HEIGHT="${CLIENT_HEIGHT:-1216}"
SERVER_TIMEOUT_MS="${SERVER_TIMEOUT_MS:-15000}"
CLIENT_MODE="${CLIENT_MODE:-internal}"
EXTERNAL_CLIENT_COMMAND="${EXTERNAL_CLIENT_COMMAND:-vkcube --wsi wayland --c ${FRAME_COUNT} --width ${CLIENT_WIDTH} --height ${CLIENT_HEIGHT}}"
CLEAR_AHB_OUTSIDE="${CLEAR_AHB_OUTSIDE:-0}"
ACCEPT_CLIENT_COMPLETE="${ACCEPT_CLIENT_COMPLETE:-0}"
PRESERVE_TMPDIR_ON_FAIL="${PRESERVE_TMPDIR_ON_FAIL:-0}"
export BRIDGE_RECONNECT_FRAMES="${BRIDGE_RECONNECT_FRAMES:-4096}"
export PASS_LOG_INTERVAL="${PASS_LOG_INTERVAL:-0}"
XDG_SHELL_XML="${XDG_SHELL_XML:-/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml}"
LINUX_DMABUF_XML="${LINUX_DMABUF_XML:-/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml}"
PRESENTATION_TIME_XML="${PRESENTATION_TIME_XML:-/usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml}"
VIEWPORTER_XML="${VIEWPORTER_XML:-/usr/share/wayland-protocols/stable/viewporter/viewporter.xml}"
RELATIVE_POINTER_XML="${RELATIVE_POINTER_XML:-/usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml}"
POINTER_CONSTRAINTS_XML="${POINTER_CONSTRAINTS_XML:-/usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml}"

have() {
  command -v "$1" >/dev/null 2>&1
}

tmpdir="$(mktemp -d /tmp/waylandie-wayland.XXXXXX)"
cleanup_tmpdir() {
  status=$?
  if [ "$PRESERVE_TMPDIR_ON_FAIL" = "1" ] && [ "$status" -ne 0 ]; then
    printf 'wayland-shm-ahb preserved-tmpdir=%s\n' "$tmpdir" >&2
  else
    rm -rf "$tmpdir"
  fi
}
trap cleanup_tmpdir EXIT
trap 'rm -rf "$tmpdir"; exit 1' HUP INT TERM

if ! have cc; then
  printf 'wayland-shm-ahb SKIP: cc not found\n' >&2
  exit 2
fi
if ! have pkg-config; then
  printf 'wayland-shm-ahb SKIP: pkg-config not found\n' >&2
  exit 2
fi
if ! pkg-config --exists wayland-server wayland-client; then
  printf 'wayland-shm-ahb SKIP: wayland-server/client development files missing\n' >&2
  exit 2
fi
if ! have wayland-scanner; then
  printf 'wayland-shm-ahb SKIP: wayland-scanner not found\n' >&2
  exit 2
fi
if [ ! -r "$XDG_SHELL_XML" ]; then
  printf 'wayland-shm-ahb SKIP: xdg-shell protocol XML missing at %s\n' "$XDG_SHELL_XML" >&2
  exit 2
fi
if [ ! -r "$LINUX_DMABUF_XML" ]; then
  printf 'wayland-shm-ahb SKIP: linux-dmabuf protocol XML missing at %s\n' "$LINUX_DMABUF_XML" >&2
  exit 2
fi
if [ ! -r "$PRESENTATION_TIME_XML" ]; then
  printf 'wayland-shm-ahb SKIP: presentation-time protocol XML missing at %s\n' "$PRESENTATION_TIME_XML" >&2
  exit 2
fi
if [ ! -r "$VIEWPORTER_XML" ]; then
  printf 'wayland-shm-ahb SKIP: viewporter protocol XML missing at %s\n' "$VIEWPORTER_XML" >&2
  exit 2
fi
if [ ! -r "$RELATIVE_POINTER_XML" ]; then
  printf 'wayland-shm-ahb SKIP: relative-pointer protocol XML missing at %s\n' "$RELATIVE_POINTER_XML" >&2
  exit 2
fi
if [ ! -r "$POINTER_CONSTRAINTS_XML" ]; then
  printf 'wayland-shm-ahb SKIP: pointer-constraints protocol XML missing at %s\n' "$POINTER_CONSTRAINTS_XML" >&2
  exit 2
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

cat >"$tmpdir/wayland-shm-ahb-server.c" <<'EOF'
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"

#define RESPONSE_PREFIX "waylandie-bridge dmabuf-present "
#define DEFAULT_ANDROID_VK_DRIVER "vulkan.waylandie.a8xx.so"
#define MAX_FDS 16
#define MAX_DMABUF_PLANES 4
#define BUFFER_KIND_SHM 1
#define BUFFER_KIND_DMABUF 2
#define DRM_FORMAT_XRGB8888 0x34325258U
#define DRM_FORMAT_ARGB8888 0x34325241U
#define DRM_FORMAT_XBGR8888 0x34324258U
#define DRM_FORMAT_ABGR8888 0x34324241U
#define DRM_FORMAT_MOD_LINEAR 0ULL
#define DRM_FORMAT_MOD_QCOM_COMPRESSED 0x0500000000000001ULL
#define WAYLANDIE_BTN_LEFT 0x110U
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct server_state {
    struct wl_display *display;
    const char *bridge_socket_name;
    int bridge_sock;
    int bridge_frames_on_socket;
    int bridge_reconnect_frames;
    int pass_log_interval;
    int target_commits;
    int output_width;
    int output_height;
    int commit_count;
    int present_failures;
    int abort_requested;
    int clear_ahb_outside;
    int accept_client_complete;
    int client_seen;
    int completed_after_client_exit;
    int android_windows;
    int next_window_id;
    double total_present_ms;
    double total_app_wait_us;
    double total_app_slot_wait_us;
    int app_wait_samples;
    int app_slot_wait_samples;
    uint32_t input_serial;
    int input_sock;
    struct wl_event_source *input_source;
    char input_buffer[8192];
    size_t input_buffer_len;
    double pointer_x;
    double pointer_y;
    int focused_surface_width;
    int focused_surface_height;
    struct wl_list keyboard_resources;
    struct wl_list pointer_resources;
    struct wl_list touch_resources;
    struct wl_resource *focused_surface;
    Display *xtest_display;
    Window xtest_window;
    Atom xtest_utf8_atom;
    Atom xtest_text_atom;
    Atom xtest_clipboard_atom;
    Atom xtest_primary_atom;
    Atom xtest_clipboard_property_atom;
    int xtest_enabled;
    int xtest_width;
    int xtest_height;
    double frame_interval_ms;
    double next_frame_callback_ms;
    uint32_t presentation_refresh_nsec;
    int last_frame_callback_commit;
    int accept_scaled_primary;
};

struct shm_pool_state {
    void *data;
    int32_t size;
};

struct shm_buffer_state {
    int kind;
    struct wl_resource *resource;
    struct shm_pool_state *pool;
    int32_t offset;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint32_t format;
    uint32_t flags;
    uint64_t modifier;
    int dmabuf_fd;
};

struct dmabuf_params_state {
    int used;
    int fds[MAX_DMABUF_PLANES];
    uint32_t has_plane[MAX_DMABUF_PLANES];
    uint32_t offsets[MAX_DMABUF_PLANES];
    uint32_t strides[MAX_DMABUF_PLANES];
    uint64_t modifiers[MAX_DMABUF_PLANES];
};

struct dmabuf_feedback_state {
    int table_fd;
};

struct surface_state {
    struct server_state *server;
    struct wl_resource *resource;
    struct shm_buffer_state *pending_buffer;
    int32_t current_width;
    int32_t current_height;
    int has_pending_attach;
    int commit_count;
    int is_xdg_surface;
    int is_subsurface;
    struct surface_state *subsurface_parent;
    struct wl_list subsurface_children;
    struct wl_list subsurface_link;
    int subsurface_linked;
    int android_window_sent;
    char android_window_id[64];
    char title[128];
    char app_id[128];
    struct wl_list frame_callbacks;
    struct wl_list presentation_feedbacks;
};

struct frame_callback_state {
    struct wl_resource *resource;
    struct wl_list link;
};

struct presentation_feedback_state {
    struct wl_resource *resource;
    struct wl_list link;
};

struct input_resource_state {
    struct server_state *server;
    struct wl_resource *resource;
    struct wl_list link;
};

static void input_debug_log(const char *fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *debug = getenv("WAYLANDIE_WAYLAND_INPUT_DEBUG");
        enabled = debug != NULL && strcmp(debug, "1") == 0;
    }
    if (!enabled) {
        return;
    }
    const char *path = getenv("WAYLANDIE_WAYLAND_INPUT_LOG");
    if (path == NULL || path[0] == '\0') {
        path = "/tmp/waylandie-wayland-input.log";
    }
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

static int input_resource_count(struct wl_list *resources) {
    int count = 0;
    struct input_resource_state *input;
    wl_list_for_each(input, resources, link) {
        count++;
    }
    return count;
}

static void maybe_send_pointer_frame(struct wl_resource *resource);

static int xtest_input_enabled(void) {
    const char *enabled = getenv("WAYLANDIE_STEAM_XTEST_INPUT");
    return enabled != NULL && strcmp(enabled, "1") == 0;
}

static int xtest_ensure_window_and_atoms(struct server_state *state) {
    if (state->xtest_display == NULL) {
        return 0;
    }
    if (state->xtest_window != None) {
        return 1;
    }
    int screen = DefaultScreen(state->xtest_display);
    Window root = RootWindow(state->xtest_display, screen);
    state->xtest_window = XCreateSimpleWindow(
            state->xtest_display,
            root,
            0,
            0,
            1,
            1,
            0,
            0,
            0);
    if (state->xtest_window == None) {
        input_debug_log("xtest-window fail");
        return 0;
    }
    XSelectInput(state->xtest_display, state->xtest_window, PropertyChangeMask);
    state->xtest_utf8_atom = XInternAtom(state->xtest_display, "UTF8_STRING", False);
    state->xtest_text_atom = XInternAtom(state->xtest_display, "TEXT", False);
    state->xtest_clipboard_atom = XInternAtom(state->xtest_display, "CLIPBOARD", False);
    state->xtest_primary_atom = XInternAtom(state->xtest_display, "PRIMARY", False);
    state->xtest_clipboard_property_atom =
            XInternAtom(state->xtest_display, "WAYLANDIE_ANDROID_CLIPBOARD", False);
    XFlush(state->xtest_display);
    return 1;
}

static int xtest_ensure_display(struct server_state *state) {
    if (!state->xtest_enabled) {
        return 0;
    }
    if (state->xtest_display != NULL) {
        return xtest_ensure_window_and_atoms(state);
    }
    const char *display_name = getenv("WAYLANDIE_STEAM_XTEST_DISPLAY");
    if (display_name == NULL || display_name[0] == '\0') {
        display_name = ":0";
    }
    Display *display = XOpenDisplay(display_name);
    if (display == NULL) {
        input_debug_log("xtest-open fail display=%s", display_name);
        return 0;
    }
    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    if (!XTestQueryExtension(display, &event_base, &error_base, &major, &minor)) {
        input_debug_log("xtest-open fail reason=no-extension display=%s", display_name);
        XCloseDisplay(display);
        return 0;
    }
    int screen = DefaultScreen(display);
    state->xtest_display = display;
    state->xtest_width = DisplayWidth(display, screen);
    state->xtest_height = DisplayHeight(display, screen);
    if (!xtest_ensure_window_and_atoms(state)) {
        XCloseDisplay(display);
        state->xtest_display = NULL;
        return 0;
    }
    input_debug_log(
            "xtest-open pass display=%s size=%dx%d version=%d.%d",
            display_name,
            state->xtest_width,
            state->xtest_height,
            major,
            minor);
    return 1;
}

static int xtest_clamp_coord(double value, int limit) {
    if (limit <= 0) {
        return 0;
    }
    if (value < 0.0) {
        return 0;
    }
    if (value > (double)(limit - 1)) {
        return limit - 1;
    }
    return (int)(value + 0.5);
}

static void xtest_pointer_move(struct server_state *state, double x, double y) {
    if (!xtest_ensure_display(state)) {
        return;
    }
    int xi = xtest_clamp_coord(x, state->xtest_width);
    int yi = xtest_clamp_coord(y, state->xtest_height);
    XTestFakeMotionEvent(state->xtest_display, -1, xi, yi, CurrentTime);
    XFlush(state->xtest_display);
    input_debug_log("xtest-motion x=%d y=%d", xi, yi);
}

static void xtest_pointer_button(struct server_state *state, const char *button_state) {
    if (!xtest_ensure_display(state)) {
        return;
    }
    Bool is_press = strcmp(button_state, "down") == 0 ? True : False;
    XTestFakeButtonEvent(state->xtest_display, 1, is_press, CurrentTime);
    XFlush(state->xtest_display);
    input_debug_log("xtest-button state=%s", button_state);
}

static void xtest_key_sym(struct server_state *state, KeySym keysym, int press) {
    if (!xtest_ensure_display(state) || keysym == NoSymbol) {
        return;
    }
    KeyCode keycode = XKeysymToKeycode(state->xtest_display, keysym);
    if (keycode == 0) {
        input_debug_log("xtest-key drop=no-keycode keysym=0x%lx", (unsigned long)keysym);
        return;
    }
    XTestFakeKeyEvent(state->xtest_display, keycode, press ? True : False, CurrentTime);
}

static void xtest_tap_key_sym(struct server_state *state, KeySym keysym) {
    xtest_key_sym(state, keysym, 1);
    xtest_key_sym(state, keysym, 0);
}

static int ascii_to_keysym(unsigned char ch, KeySym *keysym, int *shift) {
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') {
        *keysym = (KeySym)(XK_a + (ch - 'a'));
        return 1;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *keysym = (KeySym)(XK_a + (ch - 'A'));
        *shift = 1;
        return 1;
    }
    if (ch >= '0' && ch <= '9') {
        *keysym = (KeySym)(XK_0 + (ch - '0'));
        return 1;
    }
    switch (ch) {
        case ' ': *keysym = XK_space; return 1;
        case '\n': case '\r': *keysym = XK_Return; return 1;
        case '\t': *keysym = XK_Tab; return 1;
        case '-': *keysym = XK_minus; return 1;
        case '_': *keysym = XK_minus; *shift = 1; return 1;
        case '=': *keysym = XK_equal; return 1;
        case '+': *keysym = XK_equal; *shift = 1; return 1;
        case '[': *keysym = XK_bracketleft; return 1;
        case '{': *keysym = XK_bracketleft; *shift = 1; return 1;
        case ']': *keysym = XK_bracketright; return 1;
        case '}': *keysym = XK_bracketright; *shift = 1; return 1;
        case '\\': *keysym = XK_backslash; return 1;
        case '|': *keysym = XK_backslash; *shift = 1; return 1;
        case ';': *keysym = XK_semicolon; return 1;
        case ':': *keysym = XK_semicolon; *shift = 1; return 1;
        case '\'': *keysym = XK_apostrophe; return 1;
        case '"': *keysym = XK_apostrophe; *shift = 1; return 1;
        case ',': *keysym = XK_comma; return 1;
        case '<': *keysym = XK_comma; *shift = 1; return 1;
        case '.': *keysym = XK_period; return 1;
        case '>': *keysym = XK_period; *shift = 1; return 1;
        case '/': *keysym = XK_slash; return 1;
        case '?': *keysym = XK_slash; *shift = 1; return 1;
        case '`': *keysym = XK_grave; return 1;
        case '~': *keysym = XK_grave; *shift = 1; return 1;
        case '!': *keysym = XK_1; *shift = 1; return 1;
        case '@': *keysym = XK_2; *shift = 1; return 1;
        case '#': *keysym = XK_3; *shift = 1; return 1;
        case '$': *keysym = XK_4; *shift = 1; return 1;
        case '%': *keysym = XK_5; *shift = 1; return 1;
        case '^': *keysym = XK_6; *shift = 1; return 1;
        case '&': *keysym = XK_7; *shift = 1; return 1;
        case '*': *keysym = XK_8; *shift = 1; return 1;
        case '(': *keysym = XK_9; *shift = 1; return 1;
        case ')': *keysym = XK_0; *shift = 1; return 1;
        default: return 0;
    }
}

static void xtest_type_ascii(struct server_state *state, unsigned char ch) {
    KeySym keysym = NoSymbol;
    int shift = 0;
    if (!ascii_to_keysym(ch, &keysym, &shift)) {
        input_debug_log("xtest-text drop=unsupported byte=0x%02x", ch);
        return;
    }
    if (shift) {
        xtest_key_sym(state, XK_Shift_L, 1);
    }
    xtest_tap_key_sym(state, keysym);
    if (shift) {
        xtest_key_sym(state, XK_Shift_L, 0);
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void xtest_type_text_hex(struct server_state *state, const char *hex) {
    if (hex == NULL || !xtest_ensure_display(state)) {
        return;
    }
    int count = 0;
    for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2) {
        int high = hex_value(hex[i]);
        int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            break;
        }
        unsigned char byte = (unsigned char)((high << 4) | low);
        if (byte < 0x80U) {
            xtest_type_ascii(state, byte);
            count++;
        }
    }
    XFlush(state->xtest_display);
    input_debug_log("xtest-text bytes=%d", count);
}

static KeySym android_keycode_to_keysym(int keycode) {
    switch (keycode) {
        case 19: return XK_Up;
        case 20: return XK_Down;
        case 21: return XK_Left;
        case 22: return XK_Right;
        case 61: return XK_Tab;
        case 62: return XK_space;
        case 66: return XK_Return;
        case 67: return XK_BackSpace;
        case 111: return XK_Escape;
        case 112: return XK_Delete;
        default: return NoSymbol;
    }
}

static void xtest_android_key(struct server_state *state, int keycode, const char *action) {
    if (action == NULL || !xtest_ensure_display(state)) {
        return;
    }
    KeySym keysym = android_keycode_to_keysym(keycode);
    if (keysym == NoSymbol) {
        return;
    }
    xtest_key_sym(state, keysym, strcmp(action, "down") == 0);
    XFlush(state->xtest_display);
    input_debug_log("xtest-key keycode=%d action=%s", keycode, action);
}

static void xtest_copy_shortcut(struct server_state *state) {
    if (!xtest_ensure_display(state)) {
        return;
    }
    xtest_key_sym(state, XK_Control_L, 1);
    xtest_key_sym(state, XK_c, 1);
    xtest_key_sym(state, XK_c, 0);
    xtest_key_sym(state, XK_Control_L, 0);
    XFlush(state->xtest_display);
    input_debug_log("xtest-copy-shortcut");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
}

static uint32_t now_msec32(void) {
    return (uint32_t)now_ms();
}

static void sleep_ms_precise(double duration_ms) {
    if (duration_ms <= 0.0) {
        return;
    }
    struct timespec duration;
    duration.tv_sec = (time_t)(duration_ms / 1000.0);
    duration.tv_nsec = (long)((duration_ms - ((double)duration.tv_sec * 1000.0)) * 1000000.0);
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

static int connect_abstract_socket(const char *name) {
    struct sockaddr_un addr;
    size_t name_len = strlen(name);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    if (name_len + 1 > sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path + 1, name, name_len);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int send_command_with_fd(int sock, const char *command, int fd) {
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {
        .iov_base = (void *)command,
        .iov_len = strlen(command),
    };
    struct msghdr msg;
    memset(control, 0, sizeof(control));
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL) {
        errno = EINVAL;
        return -1;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;

    ssize_t sent = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        return -1;
    }
    if ((size_t)sent != iov.iov_len) {
        errno = EPIPE;
        return -1;
    }
    return 0;
}

static int send_text_bridge_command(const char *socket_name, const char *command, char *response, size_t response_size) {
    int sock = connect_abstract_socket(socket_name);
    if (sock < 0) {
        return -1;
    }
    size_t command_len = strlen(command);
    ssize_t sent = send(sock, command, command_len, MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent != command_len) {
        int saved = sent < 0 ? errno : EPIPE;
        close(sock);
        errno = saved;
        return -1;
    }
    ssize_t response_len = read(sock, response, response_size > 0 ? response_size - 1U : 0U);
    if (response_len < 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }
    if (response_size > 0) {
        response[response_len < 0 ? 0 : response_len] = '\0';
    }
    close(sock);
    return 0;
}

static void append_bridge_token(char *out, size_t out_size, const char *text) {
    size_t used = 0;
    if (out_size == 0) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        snprintf(out, out_size, "empty");
        return;
    }
    for (size_t i = 0; text[i] != '\0' && used + 1U < out_size; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '-'
                || c == '.'
                || c == ':') {
            out[used++] = (char)c;
        } else {
            out[used++] = '_';
        }
    }
    out[used] = '\0';
}

static int response_is_pass(const char *response, size_t response_len) {
    return strstr(response, " status=pass ") != NULL
            || (response_len >= strlen("status=pass")
                    && memcmp(response + response_len - strlen("status=pass"),
                            "status=pass",
                            strlen("status=pass")) == 0);
}

static long long extract_response_us_field(const char *response, const char *field) {
    size_t field_len = strlen(field);
    const char *p = response;
    while (p != NULL && (p = strstr(p, field)) != NULL) {
        if (p > response && p[-1] == '-') {
            p += field_len;
            continue;
        }
        const char *q = p + field_len;
        if (*q == '=' || *q == '_' || *q == ' ') {
            q++;
            while (*q != '\0' && (*q < '0' || *q > '9')) {
                q++;
            }
            if (*q >= '0' && *q <= '9') {
                char *end = NULL;
                long long value = strtoll(q, &end, 10);
                if (end != q && end[0] == 'u' && end[1] == 's') {
                    return value;
                }
            }
        }
        p += field_len;
    }
    return -1;
}

static int ensure_android_window_for_surface(struct surface_state *surface, struct shm_buffer_state *buffer) {
    if (surface == NULL
            || surface->server == NULL
            || !surface->server->android_windows
            || surface->android_window_sent) {
        return 0;
    }
    if (surface->android_window_id[0] == '\0') {
        snprintf(
                surface->android_window_id,
                sizeof(surface->android_window_id),
                "wl%d",
                ++surface->server->next_window_id);
    }
    char title[128];
    char app_id[128];
    char response[1024];
    char command[512];
    append_bridge_token(
            title,
            sizeof(title),
            surface->title[0] != '\0' ? surface->title : surface->android_window_id);
    append_bridge_token(
            app_id,
            sizeof(app_id),
            surface->app_id[0] != '\0' ? surface->app_id : surface->android_window_id);
    int window_width = buffer != NULL && buffer->width > 0 ? buffer->width : surface->server->output_width;
    int window_height = buffer != NULL && buffer->height > 0 ? buffer->height : surface->server->output_height;
    if (window_width <= 0) {
        window_width = 960;
    }
    if (window_height <= 0) {
        window_height = 600;
    }
    int offset = (surface->server->next_window_id % 5) * 48;
    int command_len = snprintf(
            command,
            sizeof(command),
            "window-add id=%s app-id=%s title=%s x=%d y=%d width=%d height=%d\n",
            surface->android_window_id,
            app_id,
            title,
            80 + offset,
            80 + offset,
            window_width,
            window_height);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        printf("wayland-shm-ahb window-add id=%s status=fail reason=command-too-long\n",
                surface->android_window_id);
        return -1;
    }
    if (send_text_bridge_command(
            surface->server->bridge_socket_name,
            command,
            response,
            sizeof(response)) != 0) {
        printf("wayland-shm-ahb window-add id=%s status=fail reason=bridge errno=%d\n",
                surface->android_window_id,
                errno);
        return -1;
    }
    surface->android_window_sent = 1;
    response[strcspn(response, "\r\n")] = '\0';
    printf("wayland-shm-ahb window-add id=%s response=%s\n",
            surface->android_window_id,
            response);
    return 0;
}

static void close_android_window_for_surface(struct surface_state *surface) {
    if (surface == NULL
            || surface->server == NULL
            || !surface->server->android_windows
            || !surface->android_window_sent
            || surface->android_window_id[0] == '\0') {
        return;
    }
    char command[160];
    char response[512];
    int command_len = snprintf(
            command,
            sizeof(command),
            "window-remove id=%s\n",
            surface->android_window_id);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        return;
    }
    if (send_text_bridge_command(
            surface->server->bridge_socket_name,
            command,
            response,
            sizeof(response)) == 0) {
        response[strcspn(response, "\r\n")] = '\0';
        printf("wayland-shm-ahb window-remove id=%s response=%s\n",
                surface->android_window_id,
                response);
    }
    surface->android_window_sent = 0;
}

static int present_buffer_to_android(struct surface_state *surface, struct shm_buffer_state *buffer, int frame_index) {
    struct server_state *state = surface == NULL ? NULL : surface->server;
    char response[4096];
    char command[1024];
    int status = -1;
    double start_ms = now_ms();

    if (state == NULL) {
        printf("wayland-shm-ahb frame=%d status=fail reason=no-server\n", frame_index);
        return -1;
    }
    if (buffer == NULL || buffer->kind != BUFFER_KIND_DMABUF || buffer->dmabuf_fd < 0) {
        printf("wayland-shm-ahb frame=%d status=fail reason=not-dmabuf-zero-copy\n", frame_index);
        return -1;
    }
    if (buffer->width <= 0 || buffer->height <= 0 || buffer->stride <= 0 || buffer->offset < 0) {
        printf("wayland-shm-ahb frame=%d status=fail reason=invalid-dmabuf-meta\n", frame_index);
        return -1;
    }

    uint64_t required_size = (uint64_t)(uint32_t)buffer->offset
            + ((uint64_t)(uint32_t)buffer->stride * (uint64_t)(uint32_t)buffer->height);
    uint64_t dmabuf_size = required_size;
    struct stat st;
    if (fstat(buffer->dmabuf_fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size > dmabuf_size) {
        dmabuf_size = (uint64_t)st.st_size;
    }
    const char *driver_name = getenv("WAYLANDIE_ANDROID_VK_DRIVER");
    if (driver_name == NULL || driver_name[0] == '\0') {
        driver_name = DEFAULT_ANDROID_VK_DRIVER;
    }
    const char *target_window = state->android_windows && surface->android_window_id[0] != '\0'
            ? surface->android_window_id
            : "fullscreen";
    int command_len = snprintf(
            command,
            sizeof(command),
            "dmabuf-present fast=1 window=%s width=%d height=%d format=%" PRIu32 " modifier=0x%016" PRIx64 " planes=1 stride0=%d offset0=%d size=%" PRIu64 " driver=%s\n",
            target_window,
            buffer->width,
            buffer->height,
            buffer->format,
            buffer->modifier,
            buffer->stride,
            buffer->offset,
            dmabuf_size,
            driver_name);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        printf("wayland-shm-ahb frame=%d status=fail reason=command-too-long\n", frame_index);
        return -1;
    }

    if (state->bridge_sock >= 0
            && state->bridge_reconnect_frames > 0
            && state->bridge_frames_on_socket >= state->bridge_reconnect_frames) {
        close(state->bridge_sock);
        state->bridge_sock = -1;
        state->bridge_frames_on_socket = 0;
    }
    if (state->bridge_sock < 0) {
        state->bridge_sock = connect_abstract_socket(state->bridge_socket_name);
    }
    if (state->bridge_sock < 0) {
        printf("wayland-shm-ahb frame=%d status=fail reason=bridge-connect errno=%d\n", frame_index, errno);
        return -1;
    }
    if (send_command_with_fd(state->bridge_sock, command, buffer->dmabuf_fd) != 0) {
        printf("wayland-shm-ahb frame=%d status=fail reason=dmabuf-send errno=%d\n", frame_index, errno);
        close(state->bridge_sock);
        state->bridge_sock = -1;
        state->bridge_frames_on_socket = 0;
        goto cleanup;
    }
    ssize_t response_len = read(state->bridge_sock, response, sizeof(response) - 1U);
    if (response_len <= 0) {
        printf("wayland-shm-ahb frame=%d status=fail reason=response errno=%d\n", frame_index, errno);
        close(state->bridge_sock);
        state->bridge_sock = -1;
        state->bridge_frames_on_socket = 0;
        goto cleanup;
    }
    response[response_len] = '\0';
    if (strstr(response, RESPONSE_PREFIX) == NULL || !response_is_pass(response, (size_t)response_len)) {
        printf("wayland-shm-ahb frame=%d status=fail reason=app-response response=%s\n", frame_index, response);
        goto cleanup;
    }
    double present_ms = now_ms() - start_ms;
    state->total_present_ms += present_ms;
    long long app_wait_us = extract_response_us_field(response, "wait");
    long long app_slot_wait_us = extract_response_us_field(response, "slot-wait");
    long long source_wait_us = extract_response_us_field(response, "source-wait");
    if (app_wait_us >= 0) {
        state->total_app_wait_us += (double)app_wait_us;
        state->app_wait_samples++;
    }
    if (app_slot_wait_us >= 0) {
        state->total_app_slot_wait_us += (double)app_slot_wait_us;
        state->app_slot_wait_samples++;
    }
    if (state->pass_log_interval > 0 && (frame_index % state->pass_log_interval) == 0) {
        printf(
            "wayland-shm-ahb frame=%d status=pass kind=dmabuf client=%dx%d format=0x%08x modifier=0x%016" PRIx64 " stride=%d size=%" PRIu64 " zero-copy=gpu driver=%s present-ms=%.3f app-wait-us=%lld app-slot-wait-us=%lld source-wait-us=%lld\n",
            frame_index,
            buffer->width,
            buffer->height,
            buffer->format,
            buffer->modifier,
            buffer->stride,
            dmabuf_size,
            driver_name,
            present_ms,
            app_wait_us,
            app_slot_wait_us,
            source_wait_us);
    }
    state->bridge_frames_on_socket++;
    status = 0;

cleanup:
    return status;
}

static void destroy_buffer_resource(struct wl_resource *resource) {
    struct shm_buffer_state *buffer = wl_resource_get_user_data(resource);
    if (buffer != NULL && buffer->kind == BUFFER_KIND_DMABUF && buffer->dmabuf_fd >= 0) {
        close(buffer->dmabuf_fd);
    }
    free(buffer);
}

static void buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_impl = {
    .destroy = buffer_destroy,
};

static void destroy_pool_resource(struct wl_resource *resource) {
    struct shm_pool_state *pool = wl_resource_get_user_data(resource);
    if (pool != NULL) {
        if (pool->data != NULL && pool->size > 0) {
            munmap(pool->data, (size_t)pool->size);
        }
        free(pool);
    }
}

static void shm_pool_create_buffer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        int32_t offset,
        int32_t width,
        int32_t height,
        int32_t stride,
        uint32_t format) {
    struct shm_pool_state *pool = wl_resource_get_user_data(resource);
    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    struct shm_buffer_state *buffer = calloc(1, sizeof(*buffer));
    if (buffer_resource == NULL || buffer == NULL) {
        wl_client_post_no_memory(client);
        free(buffer);
        return;
    }
    if (pool == NULL) {
        wl_resource_destroy(buffer_resource);
        free(buffer);
        return;
    }
    buffer->kind = BUFFER_KIND_SHM;
    buffer->resource = buffer_resource;
    buffer->pool = pool;
    buffer->offset = offset;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    buffer->dmabuf_fd = -1;
    wl_resource_set_implementation(buffer_resource, &buffer_impl, buffer, destroy_buffer_resource);
}

static void shm_pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void shm_pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    (void)client; (void)resource; (void)size;
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void shm_create_pool(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        int32_t fd,
        int32_t size) {
    struct wl_resource *pool_resource = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    struct shm_pool_state *pool = calloc(1, sizeof(*pool));
    if (pool_resource == NULL || pool == NULL) {
        close(fd);
        wl_client_post_no_memory(client);
        free(pool);
        return;
    }
    if (size <= 0) {
        close(fd);
        free(pool);
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "invalid shm size");
        return;
    }
    pool->data = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
    pool->size = size;
    close(fd);
    if (pool->data == MAP_FAILED) {
        free(pool);
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "mmap failed");
        return;
    }
    wl_resource_set_implementation(pool_resource, &shm_pool_impl, pool, destroy_pool_resource);
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};

static void bind_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface, version > 1 ? 1 : version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shm_impl, NULL, NULL);
    wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
    wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
}

static int dmabuf_format_supported(uint32_t format) {
    return format == DRM_FORMAT_XRGB8888
            || format == DRM_FORMAT_ARGB8888
            || format == DRM_FORMAT_XBGR8888
            || format == DRM_FORMAT_ABGR8888;
}

static void destroy_dmabuf_params_resource(struct wl_resource *resource) {
    struct dmabuf_params_state *params = wl_resource_get_user_data(resource);
    if (params != NULL) {
        for (int i = 0; i < MAX_DMABUF_PLANES; i++) {
            if (params->fds[i] >= 0) {
                close(params->fds[i]);
            }
        }
    }
    free(params);
}

static void dmabuf_params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void dmabuf_params_add(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t fd,
        uint32_t plane_idx,
        uint32_t offset,
        uint32_t stride,
        uint32_t modifier_hi,
        uint32_t modifier_lo) {
    (void)client;
    struct dmabuf_params_state *params = wl_resource_get_user_data(resource);
    if (params == NULL || plane_idx >= MAX_DMABUF_PLANES) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "invalid dmabuf plane");
        return;
    }
    if (params->has_plane[plane_idx]) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "dmabuf plane already set");
        return;
    }
    params->fds[plane_idx] = fd;
    params->has_plane[plane_idx] = 1;
    params->offsets[plane_idx] = offset;
    params->strides[plane_idx] = stride;
    params->modifiers[plane_idx] = ((uint64_t)modifier_hi << 32U) | (uint64_t)modifier_lo;
}

static struct wl_resource *create_dmabuf_wl_buffer(
        struct wl_client *client,
        struct wl_resource *params_resource,
        uint32_t buffer_id,
        int32_t width,
        int32_t height,
        uint32_t format,
        uint32_t flags) {
    struct dmabuf_params_state *params = wl_resource_get_user_data(params_resource);
    if (params == NULL || params->used) {
        wl_resource_post_error(params_resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "dmabuf params already used");
        return NULL;
    }
    params->used = 1;
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(params_resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "invalid dmabuf size");
        return NULL;
    }
    if (!dmabuf_format_supported(format)) {
        wl_resource_post_error(params_resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, "unsupported dmabuf format");
        return NULL;
    }
    if (!params->has_plane[0] || params->fds[0] < 0) {
        wl_resource_post_error(params_resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "missing dmabuf plane 0");
        return NULL;
    }
    if (params->strides[0] < (uint32_t)width * 4U) {
        wl_resource_post_error(params_resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS, "invalid dmabuf stride");
        return NULL;
    }
    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    struct shm_buffer_state *buffer = calloc(1, sizeof(*buffer));
    if (buffer_resource == NULL || buffer == NULL) {
        wl_client_post_no_memory(client);
        free(buffer);
        return NULL;
    }
    buffer->kind = BUFFER_KIND_DMABUF;
    buffer->resource = buffer_resource;
    buffer->pool = NULL;
    buffer->offset = (int32_t)params->offsets[0];
    buffer->width = width;
    buffer->height = height;
    buffer->stride = (int32_t)params->strides[0];
    buffer->format = format;
    buffer->flags = flags;
    buffer->modifier = params->modifiers[0];
    buffer->dmabuf_fd = params->fds[0];
    params->fds[0] = -1;
    wl_resource_set_implementation(buffer_resource, &buffer_impl, buffer, destroy_buffer_resource);
    printf(
        "wayland-shm-ahb dmabuf-buffer width=%d height=%d stride=%d format=0x%08x modifier=0x%016" PRIx64 " flags=0x%x\n",
        buffer->width,
        buffer->height,
        buffer->stride,
        buffer->format,
        buffer->modifier,
        buffer->flags);
    return buffer_resource;
}

static void dmabuf_params_create(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t width,
        int32_t height,
        uint32_t format,
        uint32_t flags) {
    struct wl_resource *buffer = create_dmabuf_wl_buffer(client, resource, 0, width, height, format, flags);
    if (buffer != NULL) {
        zwp_linux_buffer_params_v1_send_created(resource, buffer);
    } else {
        zwp_linux_buffer_params_v1_send_failed(resource);
    }
}

static void dmabuf_params_create_immed(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t buffer_id,
        int32_t width,
        int32_t height,
        uint32_t format,
        uint32_t flags) {
    (void)create_dmabuf_wl_buffer(client, resource, buffer_id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface dmabuf_params_impl = {
    .destroy = dmabuf_params_destroy,
    .add = dmabuf_params_add,
    .create = dmabuf_params_create,
    .create_immed = dmabuf_params_create_immed,
};

static void linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void linux_dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t params_id) {
    struct wl_resource *params_resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), params_id);
    struct dmabuf_params_state *params = calloc(1, sizeof(*params));
    if (params_resource == NULL || params == NULL) {
        wl_client_post_no_memory(client);
        free(params);
        return;
    }
    for (int i = 0; i < MAX_DMABUF_PLANES; i++) {
        params->fds[i] = -1;
    }
    wl_resource_set_implementation(params_resource, &dmabuf_params_impl, params, destroy_dmabuf_params_resource);
}

struct dmabuf_feedback_format_pair {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};

static const struct dmabuf_feedback_format_pair dmabuf_feedback_formats[] = {
    { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_QCOM_COMPRESSED },
    { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_QCOM_COMPRESSED },
    { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_XBGR8888, 0, DRM_FORMAT_MOD_QCOM_COMPRESSED },
    { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_ABGR8888, 0, DRM_FORMAT_MOD_QCOM_COMPRESSED },
};

static void destroy_dmabuf_feedback_resource(struct wl_resource *resource) {
    struct dmabuf_feedback_state *feedback = wl_resource_get_user_data(resource);
    if (feedback != NULL && feedback->table_fd >= 0) {
        close(feedback->table_fd);
    }
    free(feedback);
}

static void dmabuf_feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface dmabuf_feedback_impl = {
    .destroy = dmabuf_feedback_destroy,
};

static int create_dmabuf_feedback_table(size_t *size_out) {
    int fd = -1;
#ifdef MFD_CLOEXEC
    fd = memfd_create("waylandie-wayland-dmabuf-feedback", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        char template[] = "/tmp/waylandie-wayland-dmabuf-feedback.XXXXXX";
        fd = mkstemp(template);
        if (fd >= 0) {
            unlink(template);
        }
    }
    if (fd < 0) {
        return -1;
    }
    size_t table_size = sizeof(dmabuf_feedback_formats);
    if (ftruncate(fd, (off_t)table_size) != 0) {
        close(fd);
        return -1;
    }
    const unsigned char *cursor = (const unsigned char *)dmabuf_feedback_formats;
    size_t written = 0;
    while (written < table_size) {
        ssize_t chunk = write(fd, cursor + written, table_size - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (chunk == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        written += (size_t)chunk;
    }
    lseek(fd, 0, SEEK_SET);
    *size_out = table_size;
    return fd;
}

static dev_t dmabuf_feedback_device(void) {
    const char *override = getenv("WAYLANDIE_WAYLAND_DMABUF_FEEDBACK_DEVICE");
    const char *fallbacks[] = {
        override != NULL ? override : "",
        "/dev/dri/renderD128",
        "/dev/kgsl-3d0",
        NULL,
    };
    for (int i = 0; fallbacks[i] != NULL; i++) {
        struct stat st;
        if (fallbacks[i][0] == '\0') {
            continue;
        }
        if (stat(fallbacks[i], &st) == 0 && S_ISCHR(st.st_mode)) {
            return st.st_rdev;
        }
    }
    return (dev_t)0;
}

static int wl_array_copy_bytes(struct wl_array *array, const void *data, size_t size) {
    void *dest = wl_array_add(array, size);
    if (dest == NULL) {
        return -1;
    }
    memcpy(dest, data, size);
    return 0;
}

static int send_dmabuf_feedback_events(struct wl_resource *feedback_resource) {
    struct dmabuf_feedback_state *feedback = wl_resource_get_user_data(feedback_resource);
    if (feedback == NULL) {
        return -1;
    }
    size_t table_size = 0;
    feedback->table_fd = create_dmabuf_feedback_table(&table_size);
    if (feedback->table_fd < 0 || table_size > UINT32_MAX) {
        return -1;
    }

    dev_t device = dmabuf_feedback_device();
    uint16_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    struct wl_array device_array;
    struct wl_array indices_array;
    wl_array_init(&device_array);
    wl_array_init(&indices_array);
    int status = 0;
    if (wl_array_copy_bytes(&device_array, &device, sizeof(device)) != 0
            || wl_array_copy_bytes(&indices_array, indices, sizeof(indices)) != 0) {
        status = -1;
        goto cleanup;
    }

    zwp_linux_dmabuf_feedback_v1_send_format_table(
            feedback_resource,
            feedback->table_fd,
            (uint32_t)table_size);
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback_resource, &device_array);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback_resource, &device_array);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback_resource, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback_resource, &indices_array);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback_resource);
    zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);

cleanup:
    wl_array_release(&indices_array);
    wl_array_release(&device_array);
    return status;
}

static void linux_dmabuf_create_feedback(struct wl_client *client, uint32_t id) {
    struct wl_resource *feedback_resource =
            wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    struct dmabuf_feedback_state *feedback = calloc(1, sizeof(*feedback));
    if (feedback_resource == NULL || feedback == NULL) {
        wl_client_post_no_memory(client);
        free(feedback);
        return;
    }
    feedback->table_fd = -1;
    wl_resource_set_implementation(
            feedback_resource,
            &dmabuf_feedback_impl,
            feedback,
            destroy_dmabuf_feedback_resource);
    if (send_dmabuf_feedback_events(feedback_resource) != 0) {
        wl_client_post_no_memory(client);
    }
}

static void linux_dmabuf_get_default_feedback(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id) {
    (void)resource;
    linux_dmabuf_create_feedback(client, id);
}

static void linux_dmabuf_get_surface_feedback(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface) {
    (void)resource;
    (void)surface;
    linux_dmabuf_create_feedback(client, id);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
    .destroy = linux_dmabuf_destroy,
    .create_params = linux_dmabuf_create_params,
    .get_default_feedback = linux_dmabuf_get_default_feedback,
    .get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

static void output_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct server_state *state = data;
    int32_t width = state != NULL && state->output_width > 0 ? state->output_width : 2688;
    int32_t height = state != NULL && state->output_height > 0 ? state->output_height : 1216;
    int32_t refresh_millihz = 120000;
    if (state != NULL && state->presentation_refresh_nsec > 0) {
        double derived_refresh_millihz = 1000000000000.0
                / (double)state->presentation_refresh_nsec;
        if (derived_refresh_millihz > 0.0 && derived_refresh_millihz < 1000000.0) {
            refresh_millihz = (int32_t)(derived_refresh_millihz + 0.5);
        }
    }
    uint32_t bind_version = version > 4 ? 4 : version;
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_impl, NULL, NULL);
    wl_output_send_geometry(
            resource,
            0,
            0,
            width,
            height,
            WL_OUTPUT_SUBPIXEL_UNKNOWN,
            "WayLandIE",
            "Android SurfaceControl",
            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(
            resource,
            WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
            width,
            height,
            refresh_millihz);
    if (bind_version >= 2) {
        wl_output_send_scale(resource, 1);
        wl_output_send_done(resource);
    }
}

static void send_dmabuf_format(struct wl_resource *resource, uint32_t format) {
    zwp_linux_dmabuf_v1_send_format(resource, format);
    if (wl_resource_get_version(resource) >= 3) {
        zwp_linux_dmabuf_v1_send_modifier(resource, format, 0, 0);
    }
}

static void bind_linux_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 4 ? 4 : version;
    struct wl_resource *resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &linux_dmabuf_impl, NULL, NULL);
    if (bind_version < 4) {
        send_dmabuf_format(resource, DRM_FORMAT_XRGB8888);
        send_dmabuf_format(resource, DRM_FORMAT_ARGB8888);
        send_dmabuf_format(resource, DRM_FORMAT_XBGR8888);
        send_dmabuf_format(resource, DRM_FORMAT_ABGR8888);
    }
}

struct waylandie_xdg_surface_state {
    struct surface_state *surface;
    struct wl_resource *resource;
    struct wl_resource *toplevel_resource;
};

struct waylandie_xdg_toplevel_state {
    struct waylandie_xdg_surface_state *xdg_surface;
};

static void xdg_positioner_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_positioner_set_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_positioner_set_anchor_rect(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t x,
        int32_t y,
        int32_t width,
        int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void xdg_positioner_set_anchor(struct wl_client *client, struct wl_resource *resource, uint32_t anchor) {
    (void)client; (void)resource; (void)anchor;
}

static void xdg_positioner_set_gravity(struct wl_client *client, struct wl_resource *resource, uint32_t gravity) {
    (void)client; (void)resource; (void)gravity;
}

static void xdg_positioner_set_constraint_adjustment(struct wl_client *client, struct wl_resource *resource, uint32_t adjustment) {
    (void)client; (void)resource; (void)adjustment;
}

static void xdg_positioner_set_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client; (void)resource; (void)x; (void)y;
}

static void xdg_positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_positioner_interface xdg_positioner_impl = {
    .destroy = xdg_positioner_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_anchor,
    .set_gravity = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = xdg_positioner_set_reactive,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void send_xdg_configure(struct waylandie_xdg_surface_state *xdg_surface) {
    if (xdg_surface == NULL
            || xdg_surface->resource == NULL
            || xdg_surface->surface == NULL
            || xdg_surface->surface->server == NULL) {
        return;
    }
    if (xdg_surface->toplevel_resource != NULL) {
        struct wl_array states;
        struct server_state *server = xdg_surface->surface->server;
        int32_t width = server != NULL && server->output_width > 0 ? server->output_width : 2688;
        int32_t height = server != NULL && server->output_height > 0 ? server->output_height : 1216;
        uint32_t *state_value;
        wl_array_init(&states);
        if (server == NULL || !server->android_windows) {
            state_value = wl_array_add(&states, sizeof(*state_value));
            if (state_value != NULL) {
                *state_value = XDG_TOPLEVEL_STATE_FULLSCREEN;
            }
        }
        state_value = wl_array_add(&states, sizeof(*state_value));
        if (state_value != NULL) {
            *state_value = XDG_TOPLEVEL_STATE_ACTIVATED;
        }
        xdg_toplevel_send_configure(xdg_surface->toplevel_resource, width, height, &states);
        wl_array_release(&states);
    }
    uint32_t serial = wl_display_next_serial(xdg_surface->surface->server->display);
    printf("wayland-shm-ahb xdg-configure serial=%u size=%dx%d\n",
            serial,
            xdg_surface->surface->server->output_width,
            xdg_surface->surface->server->output_height);
    fflush(stdout);
    xdg_surface_send_configure(xdg_surface->resource, serial);
}

static void destroy_xdg_toplevel_resource(struct wl_resource *resource) {
    struct waylandie_xdg_toplevel_state *toplevel = wl_resource_get_user_data(resource);
    if (toplevel != NULL && toplevel->xdg_surface != NULL) {
        toplevel->xdg_surface->toplevel_resource = NULL;
    }
    free(toplevel);
}

static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource, struct wl_resource *parent) {
    (void)client; (void)resource; (void)parent;
}

static void xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource, const char *title) {
    (void)client;
    struct waylandie_xdg_toplevel_state *toplevel = wl_resource_get_user_data(resource);
    struct surface_state *surface = toplevel != NULL && toplevel->xdg_surface != NULL
            ? toplevel->xdg_surface->surface
            : NULL;
    if (surface != NULL && title != NULL) {
        snprintf(surface->title, sizeof(surface->title), "%s", title);
    }
}

static void xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id) {
    (void)client;
    struct waylandie_xdg_toplevel_state *toplevel = wl_resource_get_user_data(resource);
    struct surface_state *surface = toplevel != NULL && toplevel->xdg_surface != NULL
            ? toplevel->xdg_surface->surface
            : NULL;
    if (surface != NULL && app_id != NULL) {
        snprintf(surface->app_id, sizeof(surface->app_id), "%s", app_id);
    }
}

static void xdg_toplevel_show_window_menu(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *seat,
        uint32_t serial,
        int32_t x,
        int32_t y) {
    (void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}

static void xdg_toplevel_move(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)resource; (void)seat; (void)serial;
}

static void xdg_toplevel_resize(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *seat,
        uint32_t serial,
        uint32_t edges) {
    (void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}

static void xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource, struct wl_resource *output) {
    (void)client; (void)resource; (void)output;
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .set_app_id = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_maximized = xdg_toplevel_set_maximized,
    .unset_maximized = xdg_toplevel_unset_maximized,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized = xdg_toplevel_set_minimized,
};

static void xdg_popup_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_popup_grab(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)resource; (void)seat; (void)serial;
}

static void xdg_popup_reposition(struct wl_client *client, struct wl_resource *resource, struct wl_resource *positioner, uint32_t token) {
    (void)client; (void)resource; (void)positioner; (void)token;
}

static const struct xdg_popup_interface xdg_popup_impl = {
    .destroy = xdg_popup_destroy,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void destroy_xdg_surface_resource(struct wl_resource *resource) {
    struct waylandie_xdg_surface_state *xdg_surface = wl_resource_get_user_data(resource);
    free(xdg_surface);
}

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct waylandie_xdg_surface_state *xdg_surface = wl_resource_get_user_data(resource);
    struct wl_resource *toplevel_resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    struct waylandie_xdg_toplevel_state *toplevel = calloc(1, sizeof(*toplevel));
    if (toplevel_resource == NULL || toplevel == NULL) {
        wl_client_post_no_memory(client);
        free(toplevel);
        return;
    }
    toplevel->xdg_surface = xdg_surface;
    if (xdg_surface != NULL) {
        xdg_surface->toplevel_resource = toplevel_resource;
    }
    wl_resource_set_implementation(toplevel_resource, &xdg_toplevel_impl, toplevel, destroy_xdg_toplevel_resource);
    send_xdg_configure(xdg_surface);
}

static void xdg_surface_get_popup(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *parent,
        struct wl_resource *positioner) {
    (void)parent; (void)positioner;
    struct wl_resource *popup_resource = wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (popup_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(popup_resource, &xdg_popup_impl, NULL, NULL);
}

static void xdg_surface_set_window_geometry(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t x,
        int32_t y,
        int32_t width,
        int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *positioner_resource = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (positioner_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(positioner_resource, &xdg_positioner_impl, NULL, NULL);
}

static void xdg_wm_base_get_xdg_surface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface_resource) {
    struct wl_resource *xdg_surface_resource = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    struct waylandie_xdg_surface_state *xdg_surface = calloc(1, sizeof(*xdg_surface));
    if (xdg_surface_resource == NULL || xdg_surface == NULL) {
        wl_client_post_no_memory(client);
        free(xdg_surface);
        return;
    }
    xdg_surface->surface = wl_resource_get_user_data(surface_resource);
    if (xdg_surface->surface != NULL) {
        xdg_surface->surface->is_xdg_surface = 1;
    }
    printf("wayland-shm-ahb xdg-surface surface=%p\n", (void *)xdg_surface->surface);
    fflush(stdout);
    xdg_surface->resource = xdg_surface_resource;
    wl_resource_set_implementation(xdg_surface_resource, &xdg_surface_impl, xdg_surface, destroy_xdg_surface_resource);
}

static void xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

static struct wl_resource *g_xdg_wm_base_resources[16];
static int g_xdg_wm_base_resource_count = 0;

static void destroy_xdg_wm_base_resource(struct wl_resource *resource) {
    for (int i = 0; i < g_xdg_wm_base_resource_count; i++) {
        if (g_xdg_wm_base_resources[i] == resource) {
            g_xdg_wm_base_resources[i] = g_xdg_wm_base_resources[g_xdg_wm_base_resource_count - 1];
            g_xdg_wm_base_resource_count--;
            break;
        }
    }
}

/* Nested compositors (Hyprland/aquamarine) can queue xdg requests without
 * flushing while polling our socket for the configure they wait for.
 * A periodic ping forces the client to answer, flushing its request queue. */
static void ping_bound_xdg_wm_bases(struct server_state *state) {
    static long long last_ping_ms = -100000;
    long long now = now_ms();
    if (now - last_ping_ms < 500) {
        return;
    }
    last_ping_ms = now;
    for (int i = 0; i < g_xdg_wm_base_resource_count; i++) {
        struct wl_resource *resource = g_xdg_wm_base_resources[i];
        if (resource != NULL && wl_resource_get_client(resource) != NULL) {
            uint32_t serial = wl_display_next_serial(state->display);
            if (wl_resource_get_version(resource) >= XDG_WM_BASE_PING_SINCE_VERSION) {
                xdg_wm_base_send_ping(resource, serial);
            }
        }
    }
}

static void bind_xdg_wm_base(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 6 ? 6 : version;
    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    if (g_xdg_wm_base_resource_count < 16) {
        g_xdg_wm_base_resources[g_xdg_wm_base_resource_count++] = resource;
    }
    wl_resource_set_implementation(resource, &xdg_wm_base_impl, NULL, destroy_xdg_wm_base_resource);
}

static void destroy_frame_callback_resource(struct wl_resource *resource) {
    struct frame_callback_state *callback = wl_resource_get_user_data(resource);
    if (callback != NULL) {
        wl_list_remove(&callback->link);
    }
    free(callback);
}

static void destroy_presentation_feedback_resource(struct wl_resource *resource) {
    struct presentation_feedback_state *feedback = wl_resource_get_user_data(resource);
    if (feedback != NULL) {
        wl_list_remove(&feedback->link);
    }
    free(feedback);
}

static void send_surface_frame_callbacks(struct surface_state *surface) {
    if (surface == NULL) {
        return;
    }
    struct server_state *server = surface->server;
    if (server != NULL
            && server->frame_interval_ms > 0.0
            && server->last_frame_callback_commit != server->commit_count) {
        double now = now_ms();
        if (server->next_frame_callback_ms <= 0.0) {
            server->next_frame_callback_ms = now;
        }
        double target_ms = server->next_frame_callback_ms;
        if (now < target_ms) {
            sleep_ms_precise(target_ms - now);
            now = now_ms();
        }
        target_ms += server->frame_interval_ms;
        while (target_ms <= now) {
            target_ms += server->frame_interval_ms;
        }
        server->next_frame_callback_ms = target_ms;
        server->last_frame_callback_commit = server->commit_count;
    }
    struct frame_callback_state *callback;
    struct frame_callback_state *next;
    wl_list_for_each_safe(callback, next, &surface->frame_callbacks, link) {
        wl_callback_send_done(callback->resource, now_msec32());
        wl_resource_destroy(callback->resource);
    }
}

static void send_surface_presentation_feedback(struct surface_state *surface, int presented) {
    if (surface == NULL) {
        return;
    }
    struct presentation_feedback_state *feedback;
    struct presentation_feedback_state *next;
    wl_list_for_each_safe(feedback, next, &surface->presentation_feedbacks, link) {
        if (presented) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t sec = (uint64_t)ts.tv_sec;
            uint64_t seq = surface->server != NULL && surface->server->commit_count >= 0
                    ? (uint64_t)surface->server->commit_count
                    : 0;
            uint32_t refresh_nsec = surface->server != NULL && surface->server->presentation_refresh_nsec > 0
                    ? surface->server->presentation_refresh_nsec
                    : 8333333U;
            wp_presentation_feedback_send_presented(
                    feedback->resource,
                    (uint32_t)(sec >> 32U),
                    (uint32_t)(sec & 0xffffffffU),
                    (uint32_t)ts.tv_nsec,
                    refresh_nsec,
                    (uint32_t)(seq >> 32U),
                    (uint32_t)(seq & 0xffffffffU),
                    WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
        } else {
            wp_presentation_feedback_send_discarded(feedback->resource);
        }
        wl_resource_destroy(feedback->resource);
    }
}

static void presentation_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void presentation_feedback(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *surface_resource,
        uint32_t callback) {
    (void)resource;
    struct surface_state *surface = wl_resource_get_user_data(surface_resource);
    struct wl_resource *feedback_resource = wl_resource_create(
            client,
            &wp_presentation_feedback_interface,
            1,
            callback);
    struct presentation_feedback_state *feedback = calloc(1, sizeof(*feedback));
    if (feedback_resource == NULL || feedback == NULL || surface == NULL) {
        wl_client_post_no_memory(client);
        if (feedback_resource != NULL) {
            wl_resource_destroy(feedback_resource);
        }
        free(feedback);
        return;
    }
    feedback->resource = feedback_resource;
    wl_list_insert(surface->presentation_feedbacks.prev, &feedback->link);
    wl_resource_set_implementation(
            feedback_resource,
            NULL,
            feedback,
            destroy_presentation_feedback_resource);
}

static const struct wp_presentation_interface presentation_impl = {
    .destroy = presentation_destroy,
    .feedback = presentation_feedback,
};

static void bind_presentation(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 2 ? 2 : version;
    struct wl_resource *resource = wl_resource_create(client, &wp_presentation_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &presentation_impl, NULL, NULL);
    wp_presentation_send_clock_id(resource, (uint32_t)CLOCK_MONOTONIC);
}

static void subsurface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client; (void)resource; (void)x; (void)y;
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client; (void)resource; (void)sibling;
}

static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client; (void)resource; (void)sibling;
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

static void subcompositor_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface,
        struct wl_resource *parent) {
    (void)resource;
    struct surface_state *surface_state = wl_resource_get_user_data(surface);
    struct surface_state *parent_state = wl_resource_get_user_data(parent);
    struct wl_resource *subsurface_resource = wl_resource_create(
            client,
            &wl_subsurface_interface,
            wl_resource_get_version(resource),
            id);
    if (subsurface_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    if (surface_state != NULL) {
        if (surface_state->subsurface_linked) {
            wl_list_remove(&surface_state->subsurface_link);
            wl_list_init(&surface_state->subsurface_link);
            surface_state->subsurface_linked = 0;
        }
        surface_state->is_subsurface = 1;
        surface_state->subsurface_parent = parent_state;
        if (parent_state != NULL) {
            wl_list_insert(parent_state->subsurface_children.prev, &surface_state->subsurface_link);
            surface_state->subsurface_linked = 1;
        }
    }
    printf("wayland-shm-ahb subsurface child=%p parent=%p parent-xdg=%d\n",
            (void *)surface_state,
            (void *)parent_state,
            parent_state != NULL ? parent_state->is_xdg_surface : 0);
    fflush(stdout);
    wl_resource_set_implementation(subsurface_resource, &subsurface_impl, surface_state, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 1 ? 1 : version;
    struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_impl, NULL, NULL);
}

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewport_set_source(
        struct wl_client *client,
        struct wl_resource *resource,
        wl_fixed_t x,
        wl_fixed_t y,
        wl_fixed_t width,
        wl_fixed_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void viewport_set_destination(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t width,
        int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewporter_get_viewport(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface) {
    (void)surface;
    struct wl_resource *viewport_resource = wl_resource_create(
            client,
            &wp_viewport_interface,
            wl_resource_get_version(resource),
            id);
    if (viewport_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(viewport_resource, &viewport_impl, NULL, NULL);
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void bind_viewporter(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 1 ? 1 : version;
    struct wl_resource *resource = wl_resource_create(client, &wp_viewporter_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewporter_impl, NULL, NULL);
}

static void relative_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
    .destroy = relative_pointer_destroy,
};

static void relative_pointer_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void relative_pointer_manager_get_relative_pointer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *pointer) {
    (void)pointer;
    struct wl_resource *relative_pointer_resource = wl_resource_create(
            client,
            &zwp_relative_pointer_v1_interface,
            wl_resource_get_version(resource),
            id);
    if (relative_pointer_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_impl, NULL, NULL);
}

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_impl = {
    .destroy = relative_pointer_manager_destroy,
    .get_relative_pointer = relative_pointer_manager_get_relative_pointer,
};

static void bind_relative_pointer_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 1 ? 1 : version;
    struct wl_resource *resource = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relative_pointer_manager_impl, NULL, NULL);
}

static void locked_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void locked_pointer_set_cursor_position_hint(
        struct wl_client *client,
        struct wl_resource *resource,
        wl_fixed_t surface_x,
        wl_fixed_t surface_y) {
    (void)client; (void)resource; (void)surface_x; (void)surface_y;
}

static void locked_pointer_set_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
    .destroy = locked_pointer_destroy,
    .set_cursor_position_hint = locked_pointer_set_cursor_position_hint,
    .set_region = locked_pointer_set_region,
};

static void confined_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void confined_pointer_set_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
    .destroy = confined_pointer_destroy,
    .set_region = confined_pointer_set_region,
};

static void pointer_constraints_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void pointer_constraints_lock_pointer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface,
        struct wl_resource *pointer,
        struct wl_resource *region,
        uint32_t lifetime) {
    (void)resource; (void)surface; (void)pointer; (void)region; (void)lifetime;
    struct wl_resource *locked_pointer_resource = wl_resource_create(
            client,
            &zwp_locked_pointer_v1_interface,
            1,
            id);
    if (locked_pointer_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(locked_pointer_resource, &locked_pointer_impl, NULL, NULL);
    zwp_locked_pointer_v1_send_locked(locked_pointer_resource);
}

static void pointer_constraints_confine_pointer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface,
        struct wl_resource *pointer,
        struct wl_resource *region,
        uint32_t lifetime) {
    (void)resource; (void)surface; (void)pointer; (void)region; (void)lifetime;
    struct wl_resource *confined_pointer_resource = wl_resource_create(
            client,
            &zwp_confined_pointer_v1_interface,
            1,
            id);
    if (confined_pointer_resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(confined_pointer_resource, &confined_pointer_impl, NULL, NULL);
    zwp_confined_pointer_v1_send_confined(confined_pointer_resource);
}

static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl = {
    .destroy = pointer_constraints_destroy,
    .lock_pointer = pointer_constraints_lock_pointer,
    .confine_pointer = pointer_constraints_confine_pointer,
};

static void bind_pointer_constraints(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    uint32_t bind_version = version > 1 ? 1 : version;
    struct wl_resource *resource = wl_resource_create(client, &zwp_pointer_constraints_v1_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_constraints_impl, NULL, NULL);
}

static void destroy_input_resource(struct wl_resource *resource) {
    struct input_resource_state *input = wl_resource_get_user_data(resource);
    if (input != NULL) {
        wl_list_remove(&input->link);
    }
    free(input);
}

static uint32_t next_input_serial(struct server_state *state) {
    if (state == NULL) {
        return 0;
    }
    state->input_serial++;
    if (state->input_serial == 0) {
        state->input_serial = 1;
    }
    return state->input_serial;
}

static int resource_same_client(struct wl_resource *a, struct wl_resource *b) {
    return a != NULL
            && b != NULL
            && wl_resource_get_client(a) == wl_resource_get_client(b);
}

static int surface_is_displayable(struct surface_state *surface) {
    for (int depth = 0; surface != NULL && depth < 16; depth++) {
        if (surface->is_xdg_surface) {
            return 1;
        }
        surface = surface->subsurface_parent;
    }
    return 0;
}

static int64_t buffer_area(struct shm_buffer_state *buffer) {
    if (buffer == NULL || buffer->width <= 0 || buffer->height <= 0) {
        return 0;
    }
    return (int64_t)buffer->width * (int64_t)buffer->height;
}

static int buffer_is_primary_for_surface(struct surface_state *surface, struct shm_buffer_state *buffer) {
    int64_t area = buffer_area(buffer);
    if (area <= 0) {
        return 0;
    }
    if (surface == NULL || surface->server == NULL
            || surface->server->output_width <= 0
            || surface->server->output_height <= 0) {
        return 1;
    }
    if (surface->server->accept_scaled_primary
            && surface->is_xdg_surface
            && !surface->is_subsurface
            && buffer->kind == BUFFER_KIND_DMABUF) {
        return 1;
    }
    if ((int64_t)buffer->width * 5 < (int64_t)surface->server->output_width * 4) {
        return 0;
    }
    if ((int64_t)buffer->height * 5 < (int64_t)surface->server->output_height * 4) {
        return 0;
    }
    int64_t output_area = (int64_t)surface->server->output_width
            * (int64_t)surface->server->output_height;
    return area * 5 >= output_area * 4;
}

static struct surface_state *find_presentable_subsurface(struct surface_state *surface) {
    if (surface == NULL) {
        return NULL;
    }

    struct surface_state *child;
    struct surface_state *best = NULL;
    int64_t best_area = 0;
    wl_list_for_each(child, &surface->subsurface_children, subsurface_link) {
        if (child->has_pending_attach && child->pending_buffer != NULL) {
            int64_t area = buffer_area(child->pending_buffer);
            if (best == NULL || area > best_area) {
                best = child;
                best_area = area;
            }
        }

        struct surface_state *nested = find_presentable_subsurface(child);
        if (nested != NULL) {
            int64_t area = buffer_area(nested->pending_buffer);
            if (best == NULL || area > best_area) {
                best = nested;
                best_area = area;
            }
        }
    }
    return best;
}

static void send_surface_focus(
        struct surface_state *surface,
        struct wl_resource *surface_resource,
        int32_t surface_width,
        int32_t surface_height) {
    if (surface == NULL || surface->server == NULL || surface_resource == NULL) {
        return;
    }
    struct server_state *state = surface->server;
    if (surface_width <= 0) {
        surface_width = state->output_width;
    }
    if (surface_height <= 0) {
        surface_height = state->output_height;
    }
    state->focused_surface_width = surface_width > 0 ? surface_width : 1;
    state->focused_surface_height = surface_height > 0 ? surface_height : 1;
    if (state->pointer_x > (double)(state->focused_surface_width - 1)) {
        state->pointer_x = (double)(state->focused_surface_width - 1);
    }
    if (state->pointer_y > (double)(state->focused_surface_height - 1)) {
        state->pointer_y = (double)(state->focused_surface_height - 1);
    }
    if (state->focused_surface == surface_resource) {
        return;
    }
    state->focused_surface = surface_resource;
    printf("wayland-shm-ahb input-focus surface=%p size=%dx%d\n",
            (void *)surface,
            state->focused_surface_width,
            state->focused_surface_height);
    fflush(stdout);
    input_debug_log(
            "focus surface=%p resource=%p xdg=%d subsurface=%d size=%dx%d pointers=%d keyboards=%d touches=%d",
            (void *)surface,
            (void *)surface_resource,
            surface->is_xdg_surface,
            surface->is_subsurface,
            state->focused_surface_width,
            state->focused_surface_height,
            input_resource_count(&state->pointer_resources),
            input_resource_count(&state->keyboard_resources),
            input_resource_count(&state->touch_resources));

    struct input_resource_state *keyboard;
    wl_list_for_each(keyboard, &state->keyboard_resources, link) {
        if (!resource_same_client(keyboard->resource, surface_resource)) {
            continue;
        }
        int keymap_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (keymap_fd >= 0) {
            wl_keyboard_send_keymap(
                    keyboard->resource,
                    WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
                    keymap_fd,
                    0);
            close(keymap_fd);
        }
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(
                keyboard->resource,
                next_input_serial(state),
                surface_resource,
                &keys);
        wl_array_release(&keys);
    }

    struct input_resource_state *pointer;
    wl_list_for_each(pointer, &state->pointer_resources, link) {
        if (!resource_same_client(pointer->resource, surface_resource)) {
            continue;
        }
        wl_pointer_send_enter(
                pointer->resource,
                next_input_serial(state),
                surface_resource,
                wl_fixed_from_double(state->pointer_x),
                wl_fixed_from_double(state->pointer_y));
        maybe_send_pointer_frame(pointer->resource);
    }
}

static void send_focus_for_presentable(
        struct surface_state *fallback_surface,
        struct surface_state *presentable,
        struct shm_buffer_state *buffer) {
    (void)presentable;
    if (fallback_surface == NULL || fallback_surface->server == NULL) {
        return;
    }
    int32_t focus_width = fallback_surface->server->output_width;
    int32_t focus_height = fallback_surface->server->output_height;
    if (buffer != NULL && buffer->width > 0 && buffer->height > 0) {
        focus_width = buffer->width;
        focus_height = buffer->height;
    }
    send_surface_focus(
            fallback_surface,
            fallback_surface->resource,
            focus_width,
            focus_height);
}

static const char *find_input_token(const char *line, const char *key) {
    size_t key_len = strlen(key);
    const char *p = line;
    while (p != NULL && *p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return p + key_len + 1;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return NULL;
}

static int input_token_string(const char *line, const char *key, char *out, size_t out_size) {
    const char *value = find_input_token(line, key);
    if (value == NULL || out_size == 0) {
        return 0;
    }
    size_t i = 0;
    while (value[i] != '\0' && value[i] != ' ' && value[i] != '\t' && i + 1U < out_size) {
        out[i] = value[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static int input_token_double(const char *line, const char *key, double *out) {
    const char *value = find_input_token(line, key);
    char *end = NULL;
    if (value == NULL) {
        return 0;
    }
    double parsed = strtod(value, &end);
    if (end == value) {
        return 0;
    }
    *out = parsed;
    return 1;
}

static int input_token_int(const char *line, const char *key, int *out) {
    const char *value = find_input_token(line, key);
    char *end = NULL;
    if (value == NULL) {
        return 0;
    }
    long parsed = strtol(value, &end, 0);
    if (end == value) {
        return 0;
    }
    *out = (int)parsed;
    return 1;
}

static size_t clipboard_max_bytes(void) {
    const char *max_env = getenv("WAYLANDIE_ANDROID_CLIPBOARD_MAX_BYTES");
    long parsed = max_env == NULL || max_env[0] == '\0' ? 16384L : strtol(max_env, NULL, 10);
    if (parsed < 256L) {
        parsed = 256L;
    }
    if (parsed > 262144L) {
        parsed = 262144L;
    }
    return (size_t)parsed;
}

static int send_input_stream_line(struct server_state *state, const char *line) {
    if (state->input_sock < 0 || line == NULL) {
        return 0;
    }
    size_t len = strlen(line);
    ssize_t sent = send(state->input_sock, line, len, MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent != len) {
        input_debug_log("input-stream-send fail errno=%d", errno);
        return 0;
    }
    sent = send(state->input_sock, "\n", 1U, MSG_NOSIGNAL);
    return sent == 1;
}

static char *hex_encode_bytes(const unsigned char *bytes, size_t len) {
    static const char alphabet[] = "0123456789abcdef";
    char *hex = calloc((len * 2U) + 1U, 1U);
    if (hex == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        hex[i * 2U] = alphabet[bytes[i] >> 4U];
        hex[(i * 2U) + 1U] = alphabet[bytes[i] & 0x0fU];
    }
    return hex;
}

static void send_clipboard_status(
        struct server_state *state,
        const char *action,
        const char *selection,
        const char *reason) {
    char line[256];
    snprintf(
            line,
            sizeof(line),
            "input-v1 kind=clipboard action=%s selection=%s reason=%s",
            action == NULL ? "fail" : action,
            selection == NULL || selection[0] == '\0' ? "auto" : selection,
            reason == NULL || reason[0] == '\0' ? "none" : reason);
    send_input_stream_line(state, line);
}

static int send_clipboard_text(
        struct server_state *state,
        const char *selection,
        const unsigned char *bytes,
        size_t len) {
    char *hex = hex_encode_bytes(bytes, len);
    if (hex == NULL) {
        send_clipboard_status(state, "fail", selection, "oom");
        return 0;
    }
    size_t line_len = strlen(hex) + 128U;
    char *line = calloc(line_len, 1U);
    if (line == NULL) {
        free(hex);
        send_clipboard_status(state, "fail", selection, "oom");
        return 0;
    }
    snprintf(
            line,
            line_len,
            "input-v1 kind=clipboard action=set selection=%s bytes=%zu text_hex=%s",
            selection == NULL || selection[0] == '\0' ? "auto" : selection,
            len,
            hex);
    int ok = send_input_stream_line(state, line);
    free(line);
    free(hex);
    return ok;
}

static char *xtest_read_selection_target(
        struct server_state *state,
        Atom selection,
        Atom target,
        const char *selection_name,
        size_t max_bytes,
        size_t *out_len) {
    if (!xtest_ensure_display(state) || target == None || selection == None) {
        return NULL;
    }
    Atom property = state->xtest_clipboard_property_atom;
    XDeleteProperty(state->xtest_display, state->xtest_window, property);
    XConvertSelection(
            state->xtest_display,
            selection,
            target,
            property,
            state->xtest_window,
            CurrentTime);
    XFlush(state->xtest_display);

    double start = now_ms();
    while ((now_ms() - start) < 250.0) {
        while (XPending(state->xtest_display) > 0) {
            XEvent event;
            XNextEvent(state->xtest_display, &event);
            if (event.type != SelectionNotify
                    || event.xselection.requestor != state->xtest_window
                    || event.xselection.selection != selection) {
                continue;
            }
            if (event.xselection.property == None) {
                input_debug_log(
                        "clipboard selection=%s target=%lu empty",
                        selection_name,
                        (unsigned long)target);
                return NULL;
            }
            Atom actual_type = None;
            int actual_format = 0;
            unsigned long nitems = 0;
            unsigned long bytes_after = 0;
            unsigned char *data = NULL;
            long max_longs = (long)((max_bytes + 3U) / 4U);
            int result = XGetWindowProperty(
                    state->xtest_display,
                    state->xtest_window,
                    property,
                    0L,
                    max_longs,
                    True,
                    AnyPropertyType,
                    &actual_type,
                    &actual_format,
                    &nitems,
                    &bytes_after,
                    &data);
            if (result != Success || data == NULL || actual_format != 8 || nitems == 0) {
                if (data != NULL) {
                    XFree(data);
                }
                input_debug_log(
                        "clipboard selection=%s target=%lu bad-result result=%d format=%d items=%lu",
                        selection_name,
                        (unsigned long)target,
                        result,
                        actual_format,
                        nitems);
                return NULL;
            }
            size_t copy_len = (size_t)nitems;
            if (copy_len > max_bytes) {
                copy_len = max_bytes;
            }
            char *copy = calloc(copy_len + 1U, 1U);
            if (copy != NULL) {
                memcpy(copy, data, copy_len);
                if (out_len != NULL) {
                    *out_len = copy_len;
                }
            }
            XFree(data);
            input_debug_log(
                    "clipboard selection=%s target=%lu bytes=%zu after=%lu",
                    selection_name,
                    (unsigned long)target,
                    copy_len,
                    bytes_after);
            return copy;
        }
        sleep_ms_precise(2.0);
    }
    input_debug_log("clipboard selection=%s timeout", selection_name);
    return NULL;
}

static char *xtest_read_selection_text(
        struct server_state *state,
        Atom selection,
        const char *selection_name,
        size_t max_bytes,
        size_t *out_len) {
    Atom targets[3] = {
        state->xtest_utf8_atom,
        XA_STRING,
        state->xtest_text_atom,
    };
    for (size_t i = 0; i < 3U; i++) {
        size_t len = 0;
        char *text = xtest_read_selection_target(
                state,
                selection,
                targets[i],
                selection_name,
                max_bytes,
                &len);
        if (text != NULL && len > 0 && text[0] != '\0') {
            if (out_len != NULL) {
                *out_len = len;
            }
            return text;
        }
        free(text);
    }
    return NULL;
}

static char *xtest_read_clipboard_auto(
        struct server_state *state,
        const char *requested_selection,
        int prefer_clipboard,
        char *used_selection,
        size_t used_selection_size,
        size_t *out_len) {
    if (used_selection != NULL && used_selection_size > 0) {
        used_selection[0] = '\0';
    }
    if (!xtest_ensure_display(state)) {
        return NULL;
    }
    size_t max_bytes = clipboard_max_bytes();
    if (requested_selection != NULL && strcmp(requested_selection, "clipboard") == 0) {
        if (used_selection != NULL) {
            snprintf(used_selection, used_selection_size, "clipboard");
        }
        return xtest_read_selection_text(
                state,
                state->xtest_clipboard_atom,
                "clipboard",
                max_bytes,
                out_len);
    }
    if (requested_selection != NULL && strcmp(requested_selection, "primary") == 0) {
        if (used_selection != NULL) {
            snprintf(used_selection, used_selection_size, "primary");
        }
        return xtest_read_selection_text(
                state,
                state->xtest_primary_atom,
                "primary",
                max_bytes,
                out_len);
    }

    if (prefer_clipboard) {
        if (used_selection != NULL) {
            snprintf(used_selection, used_selection_size, "clipboard");
        }
        char *text = xtest_read_selection_text(
                state,
                state->xtest_clipboard_atom,
                "clipboard",
                max_bytes,
                out_len);
        if (text != NULL) {
            return text;
        }
        if (used_selection != NULL) {
            snprintf(used_selection, used_selection_size, "primary");
        }
        return xtest_read_selection_text(
                state,
                state->xtest_primary_atom,
                "primary",
                max_bytes,
                out_len);
    }

    if (used_selection != NULL) {
        snprintf(used_selection, used_selection_size, "primary");
    }
    char *text = xtest_read_selection_text(
            state,
            state->xtest_primary_atom,
            "primary",
            max_bytes,
            out_len);
    if (text != NULL) {
        return text;
    }
    if (used_selection != NULL) {
        snprintf(used_selection, used_selection_size, "clipboard");
    }
    return xtest_read_selection_text(
            state,
            state->xtest_clipboard_atom,
            "clipboard",
            max_bytes,
            out_len);
}

static void handle_clipboard_request(struct server_state *state, const char *line) {
    char selection[32] = "auto";
    char copy[8] = "0";
    input_token_string(line, "selection", selection, sizeof(selection));
    input_token_string(line, "copy", copy, sizeof(copy));
    if (!state->xtest_enabled) {
        send_clipboard_status(state, "fail", selection, "xtest-disabled");
        return;
    }
    int should_copy = strcmp(copy, "1") == 0 || strcmp(copy, "true") == 0;
    if (should_copy) {
        xtest_copy_shortcut(state);
        sleep_ms_precise(90.0);
    }
    char used_selection[32] = "auto";
    size_t len = 0;
    char *text = xtest_read_clipboard_auto(
            state,
            selection,
            should_copy,
            used_selection,
            sizeof(used_selection),
            &len);
    if (text == NULL || len == 0 || text[0] == '\0') {
        free(text);
        send_clipboard_status(state, "empty", used_selection, "no-selection");
        return;
    }
    send_clipboard_text(state, used_selection, (const unsigned char *)text, len);
    free(text);
}

static void clamp_pointer_to_output(struct server_state *state, double *x, double *y) {
    int clamp_width = state->focused_surface_width > 0
            ? state->focused_surface_width
            : state->output_width;
    int clamp_height = state->focused_surface_height > 0
            ? state->focused_surface_height
            : state->output_height;
    double max_x = clamp_width > 0 ? (double)(clamp_width - 1) : 0.0;
    double max_y = clamp_height > 0 ? (double)(clamp_height - 1) : 0.0;
    if (*x < 0.0) {
        *x = 0.0;
    } else if (*x > max_x) {
        *x = max_x;
    }
    if (*y < 0.0) {
        *y = 0.0;
    } else if (*y > max_y) {
        *y = max_y;
    }
}

static void map_input_to_focused_surface(
        struct server_state *state,
        double input_width,
        double input_height,
        double *x,
        double *y) {
    if (input_width <= 0.0) {
        input_width = state->output_width > 0 ? (double)state->output_width : 1.0;
    }
    if (input_height <= 0.0) {
        input_height = state->output_height > 0 ? (double)state->output_height : 1.0;
    }
    double target_width = state->focused_surface_width > 0
            ? (double)state->focused_surface_width
            : (state->output_width > 0 ? (double)state->output_width : input_width);
    double target_height = state->focused_surface_height > 0
            ? (double)state->focused_surface_height
            : (state->output_height > 0 ? (double)state->output_height : input_height);
    *x = (*x / input_width) * target_width;
    *y = (*y / input_height) * target_height;
}

static void maybe_send_pointer_frame(struct wl_resource *resource) {
    if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(resource);
    }
}

static void emit_pointer_enter_if_needed(struct server_state *state, struct wl_resource *pointer_resource) {
    if (state->focused_surface == NULL || !resource_same_client(pointer_resource, state->focused_surface)) {
        return;
    }
    wl_pointer_send_enter(
            pointer_resource,
            next_input_serial(state),
            state->focused_surface,
            wl_fixed_from_double(state->pointer_x),
            wl_fixed_from_double(state->pointer_y));
    maybe_send_pointer_frame(pointer_resource);
}

static void emit_pointer_motion(struct server_state *state, double x, double y, uint32_t time_ms) {
    if (state->focused_surface == NULL) {
        input_debug_log("pointer-motion drop=no-focus x=%.1f y=%.1f", x, y);
        return;
    }
    clamp_pointer_to_output(state, &x, &y);
    state->pointer_x = x;
    state->pointer_y = y;
    int emitted = 0;
    struct input_resource_state *pointer;
    wl_list_for_each(pointer, &state->pointer_resources, link) {
        if (!resource_same_client(pointer->resource, state->focused_surface)) {
            continue;
        }
        wl_pointer_send_motion(
                pointer->resource,
                time_ms,
                wl_fixed_from_double(x),
                wl_fixed_from_double(y));
        maybe_send_pointer_frame(pointer->resource);
        emitted++;
    }
    input_debug_log(
            "pointer-motion x=%.1f y=%.1f emitted=%d pointers=%d focused=%p",
            x,
            y,
            emitted,
            input_resource_count(&state->pointer_resources),
            (void *)state->focused_surface);
}

static void emit_pointer_button(struct server_state *state, const char *button_state, uint32_t time_ms) {
    if (state->focused_surface == NULL) {
        input_debug_log("pointer-button drop=no-focus state=%s", button_state);
        return;
    }
    uint32_t wl_state = strcmp(button_state, "down") == 0
            ? WL_POINTER_BUTTON_STATE_PRESSED
            : WL_POINTER_BUTTON_STATE_RELEASED;
    int emitted = 0;
    struct input_resource_state *pointer;
    wl_list_for_each(pointer, &state->pointer_resources, link) {
        if (!resource_same_client(pointer->resource, state->focused_surface)) {
            continue;
        }
        wl_pointer_send_button(
                pointer->resource,
                next_input_serial(state),
                time_ms,
                WAYLANDIE_BTN_LEFT,
                wl_state);
        maybe_send_pointer_frame(pointer->resource);
        emitted++;
    }
    input_debug_log(
            "pointer-button state=%s emitted=%d pointers=%d focused=%p",
            button_state,
            emitted,
            input_resource_count(&state->pointer_resources),
            (void *)state->focused_surface);
}

static void emit_pointer_scroll(struct server_state *state, double hscroll, double vscroll, uint32_t time_ms) {
    if (state->focused_surface == NULL) {
        return;
    }
    struct input_resource_state *pointer;
    wl_list_for_each(pointer, &state->pointer_resources, link) {
        if (!resource_same_client(pointer->resource, state->focused_surface)) {
            continue;
        }
        if (vscroll != 0.0) {
            wl_pointer_send_axis(
                    pointer->resource,
                    time_ms,
                    WL_POINTER_AXIS_VERTICAL_SCROLL,
                    wl_fixed_from_double(-vscroll * 120.0));
        }
        if (hscroll != 0.0) {
            wl_pointer_send_axis(
                    pointer->resource,
                    time_ms,
                    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    wl_fixed_from_double(hscroll * 120.0));
        }
        maybe_send_pointer_frame(pointer->resource);
    }
}

static void maybe_send_touch_frame(struct wl_resource *resource) {
    wl_touch_send_frame(resource);
}

static void emit_touch_event(
        struct server_state *state,
        const char *action,
        int touch_id,
        double x,
        double y,
        uint32_t time_ms) {
    if (state->focused_surface == NULL) {
        input_debug_log("touch drop=no-focus action=%s id=%d x=%.1f y=%.1f", action, touch_id, x, y);
        return;
    }
    clamp_pointer_to_output(state, &x, &y);
    int emitted = 0;
    struct input_resource_state *touch;
    wl_list_for_each(touch, &state->touch_resources, link) {
        if (!resource_same_client(touch->resource, state->focused_surface)) {
            continue;
        }
        if (strcmp(action, "down") == 0) {
            wl_touch_send_down(
                    touch->resource,
                    next_input_serial(state),
                    time_ms,
                    state->focused_surface,
                    touch_id,
                    wl_fixed_from_double(x),
                    wl_fixed_from_double(y));
        } else if (strcmp(action, "move") == 0) {
            wl_touch_send_motion(
                    touch->resource,
                    time_ms,
                    touch_id,
                    wl_fixed_from_double(x),
                    wl_fixed_from_double(y));
        } else if (strcmp(action, "up") == 0) {
            wl_touch_send_up(
                    touch->resource,
                    next_input_serial(state),
                    time_ms,
                    touch_id);
        } else if (strcmp(action, "cancel") == 0) {
            wl_touch_send_cancel(touch->resource);
        }
        maybe_send_touch_frame(touch->resource);
        emitted++;
    }
    input_debug_log(
            "touch action=%s id=%d x=%.1f y=%.1f emitted=%d touches=%d focused=%p",
            action,
            touch_id,
            x,
            y,
            emitted,
            input_resource_count(&state->touch_resources),
            (void *)state->focused_surface);
}

static void handle_input_line(struct server_state *state, const char *line) {
    char kind[32];
    char action[32];
    char button_state[16];
    char text_hex[2048];
    double x = state->pointer_x;
    double y = state->pointer_y;
    double input_width = state->output_width > 0 ? (double)state->output_width : 1.0;
    double input_height = state->output_height > 0 ? (double)state->output_height : 1.0;
    double hscroll = 0.0;
    double vscroll = 0.0;
    int touch_id = 0;
    int keycode = 0;
    int event_time = (int)now_msec32();
    if (strncmp(line, "input-v1 ", 9) != 0
            || !input_token_string(line, "kind", kind, sizeof(kind))
            || !input_token_string(line, "action", action, sizeof(action))) {
        return;
    }
    input_token_double(line, "x", &x);
    input_token_double(line, "y", &y);
    input_token_double(line, "width", &input_width);
    input_token_double(line, "height", &input_height);
    input_token_double(line, "hscroll", &hscroll);
    input_token_double(line, "vscroll", &vscroll);
    input_token_int(line, "id", &touch_id);
    input_token_int(line, "keycode", &keycode);
    input_token_int(line, "time", &event_time);
    map_input_to_focused_surface(state, input_width, input_height, &x, &y);
    input_debug_log(
            "input-line kind=%s action=%s x=%.1f y=%.1f width=%.1f height=%.1f focused=%p",
            kind,
            action,
            x,
            y,
            input_width,
            input_height,
            (void *)state->focused_surface);

    if (strcmp(kind, "pointer") == 0) {
        if (strcmp(action, "move") == 0) {
            emit_pointer_motion(state, x, y, (uint32_t)event_time);
            xtest_pointer_move(state, x, y);
        } else if (strcmp(action, "button") == 0
                && input_token_string(line, "state", button_state, sizeof(button_state))) {
            emit_pointer_motion(state, x, y, (uint32_t)event_time);
            xtest_pointer_move(state, x, y);
            emit_pointer_button(state, button_state, (uint32_t)event_time);
            xtest_pointer_button(state, button_state);
        } else if (strcmp(action, "scroll") == 0) {
            emit_pointer_motion(state, x, y, (uint32_t)event_time);
            xtest_pointer_move(state, x, y);
            emit_pointer_scroll(state, hscroll, vscroll, (uint32_t)event_time);
        }
    } else if (strcmp(kind, "touch") == 0) {
        emit_touch_event(state, action, touch_id, x, y, (uint32_t)event_time);
    } else if (strcmp(kind, "text") == 0) {
        if (strcmp(action, "commit") == 0
                && input_token_string(line, "text_hex", text_hex, sizeof(text_hex))) {
            xtest_type_text_hex(state, text_hex);
        }
    } else if (strcmp(kind, "key") == 0) {
        xtest_android_key(state, keycode, action);
    } else if (strcmp(kind, "clipboard") == 0) {
        if (strcmp(action, "request") == 0) {
            handle_clipboard_request(state, line);
        }
    }
}

static int input_stream_fd_event(int fd, uint32_t mask, void *data) {
    struct server_state *state = data;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        if (state->input_source != NULL) {
            wl_event_source_remove(state->input_source);
            state->input_source = NULL;
        }
        close(fd);
        state->input_sock = -1;
        state->input_buffer_len = 0;
        printf("wayland-shm-ahb input-stream=closed mask=0x%x\n", mask);
        return 0;
    }
    char buffer[1024];
    while (1) {
        ssize_t read_count = read(fd, buffer, sizeof(buffer));
        if (read_count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            if (state->input_source != NULL) {
                wl_event_source_remove(state->input_source);
                state->input_source = NULL;
            }
            close(fd);
            state->input_sock = -1;
            state->input_buffer_len = 0;
            printf("wayland-shm-ahb input-stream=read-error errno=%d\n", errno);
            return 0;
        }
        if (read_count == 0) {
            if (state->input_source != NULL) {
                wl_event_source_remove(state->input_source);
                state->input_source = NULL;
            }
            close(fd);
            state->input_sock = -1;
            state->input_buffer_len = 0;
            printf("wayland-shm-ahb input-stream=eof\n");
            return 0;
        }
        for (ssize_t i = 0; i < read_count; i++) {
            char c = buffer[i];
            if (c == '\n') {
                state->input_buffer[state->input_buffer_len] = '\0';
                handle_input_line(state, state->input_buffer);
                state->input_buffer_len = 0;
            } else if (c != '\r' && state->input_buffer_len + 1U < sizeof(state->input_buffer)) {
                state->input_buffer[state->input_buffer_len++] = c;
            } else if (state->input_buffer_len + 1U >= sizeof(state->input_buffer)) {
                state->input_buffer_len = 0;
            }
        }
    }
    return 0;
}

static int connect_bridge_input_stream(struct server_state *state) {
    int fd = connect_abstract_socket(state->bridge_socket_name);
    if (fd < 0) {
        printf("wayland-shm-ahb input-stream=connect-fail errno=%d\n", errno);
        return -1;
    }
    const char command[] = "input-stream\n";
    ssize_t sent = send(fd, command, sizeof(command) - 1U, MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent != sizeof(command) - 1U) {
        int saved = sent < 0 ? errno : EPIPE;
        close(fd);
        errno = saved;
        printf("wayland-shm-ahb input-stream=command-fail errno=%d\n", errno);
        return -1;
    }
    char response[256];
    ssize_t response_len = read(fd, response, sizeof(response) - 1U);
    if (response_len <= 0) {
        int saved = response_len < 0 ? errno : EPIPE;
        close(fd);
        errno = saved;
        printf("wayland-shm-ahb input-stream=response-fail errno=%d\n", errno);
        return -1;
    }
    response[response_len] = '\0';
    response[strcspn(response, "\r\n")] = '\0';
    if (strstr(response, "status=pass") == NULL) {
        close(fd);
        printf("wayland-shm-ahb input-stream=response-reject response=%s\n", response);
        errno = EPROTO;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    printf("wayland-shm-ahb input-stream=ready response=%s\n", response);
    return fd;
}

static void pointer_set_cursor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t serial,
        struct wl_resource *surface,
        int32_t hotspot_x,
        int32_t hotspot_y) {
    (void)client; (void)resource; (void)serial; (void)surface; (void)hotspot_x; (void)hotspot_y;
}

static void pointer_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

static void touch_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_touch_interface touch_impl = {
    .release = touch_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_state *state = wl_resource_get_user_data(resource);
    struct wl_resource *pointer_resource = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    struct input_resource_state *input = calloc(1, sizeof(*input));
    if (pointer_resource == NULL || input == NULL) {
        wl_client_post_no_memory(client);
        free(input);
        return;
    }
    input->server = state;
    input->resource = pointer_resource;
    wl_list_insert(&state->pointer_resources, &input->link);
    wl_resource_set_implementation(pointer_resource, &pointer_impl, input, destroy_input_resource);
    input_debug_log(
            "seat-get-pointer resource=%p focus=%p same-client=%d total=%d",
            (void *)pointer_resource,
            (void *)state->focused_surface,
            resource_same_client(pointer_resource, state->focused_surface),
            input_resource_count(&state->pointer_resources));
    if (resource_same_client(pointer_resource, state->focused_surface)) {
        wl_pointer_send_enter(
                pointer_resource,
                next_input_serial(state),
                state->focused_surface,
                wl_fixed_from_double(state->pointer_x),
                wl_fixed_from_double(state->pointer_y));
        maybe_send_pointer_frame(pointer_resource);
    }
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_state *state = wl_resource_get_user_data(resource);
    struct wl_resource *keyboard_resource = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    struct input_resource_state *input = calloc(1, sizeof(*input));
    if (keyboard_resource == NULL || input == NULL) {
        wl_client_post_no_memory(client);
        free(input);
        return;
    }
    input->server = state;
    input->resource = keyboard_resource;
    wl_list_insert(&state->keyboard_resources, &input->link);
    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, input, destroy_input_resource);
    input_debug_log(
            "seat-get-keyboard resource=%p focus=%p same-client=%d total=%d",
            (void *)keyboard_resource,
            (void *)state->focused_surface,
            resource_same_client(keyboard_resource, state->focused_surface),
            input_resource_count(&state->keyboard_resources));
    if (resource_same_client(keyboard_resource, state->focused_surface)) {
        int keymap_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (keymap_fd >= 0) {
            wl_keyboard_send_keymap(
                    keyboard_resource,
                    WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
                    keymap_fd,
                    0);
            close(keymap_fd);
        }
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(
                keyboard_resource,
                next_input_serial(state),
                state->focused_surface,
                &keys);
        wl_array_release(&keys);
    }
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_state *state = wl_resource_get_user_data(resource);
    struct wl_resource *touch_resource = wl_resource_create(client, &wl_touch_interface, 1, id);
    struct input_resource_state *input = calloc(1, sizeof(*input));
    if (touch_resource == NULL || input == NULL) {
        wl_client_post_no_memory(client);
        free(input);
        return;
    }
    input->server = state;
    input->resource = touch_resource;
    wl_list_insert(&state->touch_resources, &input->link);
    wl_resource_set_implementation(touch_resource, &touch_impl, input, destroy_input_resource);
    input_debug_log(
            "seat-get-touch resource=%p focus=%p same-client=%d total=%d",
            (void *)touch_resource,
            (void *)state->focused_surface,
            resource_same_client(touch_resource, state->focused_surface),
            input_resource_count(&state->touch_resources));
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    uint32_t bind_version = version > 9 ? 9 : version;
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, bind_version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, data, NULL);
    wl_seat_send_capabilities(
            resource,
            WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
    if (bind_version >= 2) {
        wl_seat_send_name(resource, "waylandie-android-seat");
    }
}

static void destroy_surface_resource(struct wl_resource *resource) {
    struct surface_state *surface = wl_resource_get_user_data(resource);
    if (surface != NULL) {
        if (surface->subsurface_linked) {
            wl_list_remove(&surface->subsurface_link);
            wl_list_init(&surface->subsurface_link);
            surface->subsurface_linked = 0;
        }
        struct surface_state *child;
        struct surface_state *tmp;
        wl_list_for_each_safe(child, tmp, &surface->subsurface_children, subsurface_link) {
            wl_list_remove(&child->subsurface_link);
            wl_list_init(&child->subsurface_link);
            child->subsurface_linked = 0;
            child->subsurface_parent = NULL;
            child->is_subsurface = 0;
        }
        if (surface->server != NULL && surface->server->focused_surface == surface->resource) {
            surface->server->focused_surface = NULL;
            surface->server->focused_surface_width = 0;
            surface->server->focused_surface_height = 0;
        }
        close_android_window_for_surface(surface);
        send_surface_presentation_feedback(surface, 0);
        send_surface_frame_callbacks(surface);
    }
    free(surface);
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer, int32_t x, int32_t y) {
    (void)client; (void)x; (void)y;
    struct surface_state *surface = wl_resource_get_user_data(resource);
    if (surface == NULL) {
        return;
    }
    surface->has_pending_attach = 1;
    if (surface->pending_buffer != NULL && surface->pending_buffer->resource != NULL) {
        wl_buffer_send_release(surface->pending_buffer->resource);
    }
    surface->pending_buffer = buffer == NULL ? NULL : wl_resource_get_user_data(buffer);
    if (surface->pending_buffer != NULL) {
        surface->current_width = surface->pending_buffer->width;
        surface->current_height = surface->pending_buffer->height;
        printf("wayland-shm-ahb attach xdg=%d subsurface=%d displayable=%d kind=%d size=%dx%d\n",
                surface->is_xdg_surface,
                surface->is_subsurface,
                surface_is_displayable(surface),
                surface->pending_buffer->kind,
                surface->pending_buffer->width,
                surface->pending_buffer->height);
        fflush(stdout);
    }
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t callback) {
    struct surface_state *surface = wl_resource_get_user_data(resource);
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
    struct frame_callback_state *callback_state = calloc(1, sizeof(*callback_state));
    if (cb == NULL || callback_state == NULL || surface == NULL) {
        wl_client_post_no_memory(client);
        if (cb != NULL) {
            wl_resource_destroy(cb);
        }
        free(callback_state);
        return;
    }
    callback_state->resource = cb;
    wl_list_insert(surface->frame_callbacks.prev, &callback_state->link);
    wl_resource_set_implementation(cb, NULL, callback_state, destroy_frame_callback_resource);
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct surface_state *surface = wl_resource_get_user_data(resource);
    if (surface == NULL || surface->server == NULL) {
        return;
    }
    if (!surface_is_displayable(surface)) {
        printf("wayland-shm-ahb commit=ignored-non-displayable xdg=%d subsurface=%d kind=%d\n",
                surface->is_xdg_surface,
                surface->is_subsurface,
                surface->has_pending_attach && surface->pending_buffer != NULL ? surface->pending_buffer->kind : 0);
        fflush(stdout);
        if (surface->has_pending_attach && surface->pending_buffer != NULL && surface->pending_buffer->resource != NULL) {
            wl_buffer_send_release(surface->pending_buffer->resource);
        }
        surface->has_pending_attach = 0;
        surface->pending_buffer = NULL;
        surface->commit_count++;
        send_surface_presentation_feedback(surface, 0);
        send_surface_frame_callbacks(surface);
        return;
    }
    if (surface->is_subsurface && surface->subsurface_parent != NULL) {
        if (surface->has_pending_attach && surface->pending_buffer == NULL) {
            printf("wayland-shm-ahb commit=subsurface-clear\n");
            surface->has_pending_attach = 0;
        } else if (surface->has_pending_attach && surface->pending_buffer != NULL) {
            printf("wayland-shm-ahb commit=subsurface-pending kind=%d size=%dx%d primary=%d\n",
                    surface->pending_buffer->kind,
                    surface->pending_buffer->width,
                    surface->pending_buffer->height,
                    buffer_is_primary_for_surface(surface, surface->pending_buffer));
        } else {
            printf("wayland-shm-ahb commit=subsurface-no-buffer\n");
        }
        fflush(stdout);
        surface->commit_count++;
        send_surface_frame_callbacks(surface);
        return;
    }
    if (!surface->has_pending_attach) {
        printf("wayland-shm-ahb commit=no-buffer\n");
        fflush(stdout);
        surface->commit_count++;
        send_surface_focus(surface, resource, surface->current_width, surface->current_height);
        send_surface_presentation_feedback(surface, 0);
        send_surface_frame_callbacks(surface);
        return;
    }
    if (surface->pending_buffer == NULL) {
        struct surface_state *presentable = find_presentable_subsurface(surface);
        if (presentable != NULL
                && presentable->pending_buffer != NULL
                && buffer_is_primary_for_surface(surface, presentable->pending_buffer)) {
            int frame_index = surface->server->commit_count;
            int present_failed = 0;
            printf("wayland-shm-ahb commit=subsurface-latched xdg=%d subsurface=%d kind=%d size=%dx%d\n",
                    presentable->is_xdg_surface,
                    presentable->is_subsurface,
                    presentable->pending_buffer->kind,
                    presentable->pending_buffer->width,
                    presentable->pending_buffer->height);
            fflush(stdout);
            if (ensure_android_window_for_surface(surface, presentable->pending_buffer) != 0
                    || present_buffer_to_android(surface, presentable->pending_buffer, frame_index) != 0) {
                surface->server->present_failures++;
                surface->server->abort_requested = 1;
                send_surface_presentation_feedback(presentable, 0);
                present_failed = 1;
                fflush(stdout);
                wl_display_terminate(surface->server->display);
            }
            send_focus_for_presentable(surface, presentable, presentable->pending_buffer);
            if (presentable->pending_buffer->resource != NULL) {
                wl_buffer_send_release(presentable->pending_buffer->resource);
            }
            presentable->has_pending_attach = 0;
            presentable->pending_buffer = NULL;
            surface->has_pending_attach = 0;
            surface->server->commit_count++;
            surface->commit_count++;
            presentable->commit_count++;
            if (!present_failed) {
                send_surface_presentation_feedback(presentable, 1);
            }
            send_surface_frame_callbacks(presentable);
            send_surface_presentation_feedback(surface, present_failed ? 0 : 1);
            send_surface_frame_callbacks(surface);
            return;
        }
        printf("wayland-shm-ahb commit=configure-only\n");
        fflush(stdout);
        surface->has_pending_attach = 0;
        surface->commit_count++;
        send_surface_focus(surface, resource, surface->current_width, surface->current_height);
        send_surface_presentation_feedback(surface, 0);
        send_surface_frame_callbacks(surface);
        return;
    }
    int frame_index = surface->server->commit_count;
    int present_failed = 0;
    struct surface_state *presentable = surface;
    struct surface_state *child_presentable = find_presentable_subsurface(surface);
    if (child_presentable != NULL
            && child_presentable->pending_buffer != NULL
            && buffer_area(child_presentable->pending_buffer) > buffer_area(surface->pending_buffer)) {
        presentable = child_presentable;
        printf("wayland-shm-ahb commit=subsurface-preferred parent=%dx%d child=%dx%d\n",
                surface->pending_buffer != NULL ? surface->pending_buffer->width : 0,
                surface->pending_buffer != NULL ? surface->pending_buffer->height : 0,
                presentable->pending_buffer->width,
                presentable->pending_buffer->height);
        fflush(stdout);
    }
    struct shm_buffer_state *buffer_to_present = presentable->pending_buffer;
    if (!buffer_is_primary_for_surface(surface, buffer_to_present)) {
        printf("wayland-shm-ahb commit=nonprimary-skipped size=%dx%d\n",
                buffer_to_present != NULL ? buffer_to_present->width : 0,
                buffer_to_present != NULL ? buffer_to_present->height : 0);
        fflush(stdout);
        if (presentable != surface && surface->pending_buffer != NULL && surface->pending_buffer->resource != NULL) {
            wl_buffer_send_release(surface->pending_buffer->resource);
        }
        if (presentable == surface && buffer_to_present != NULL && buffer_to_present->resource != NULL) {
            wl_buffer_send_release(buffer_to_present->resource);
        }
        surface->has_pending_attach = 0;
        surface->pending_buffer = NULL;
        surface->commit_count++;
        send_surface_focus(surface, resource, surface->current_width, surface->current_height);
        send_surface_presentation_feedback(surface, 0);
        send_surface_frame_callbacks(surface);
        return;
    }
    if (ensure_android_window_for_surface(surface, buffer_to_present) != 0
            || present_buffer_to_android(surface, buffer_to_present, frame_index) != 0) {
        surface->server->present_failures++;
        surface->server->abort_requested = 1;
        send_surface_presentation_feedback(presentable, 0);
        present_failed = 1;
        fflush(stdout);
        wl_display_terminate(surface->server->display);
    }
    if (presentable != surface && surface->pending_buffer != NULL && surface->pending_buffer->resource != NULL) {
        wl_buffer_send_release(surface->pending_buffer->resource);
    }
    if (buffer_to_present != NULL && buffer_to_present->resource != NULL) {
        wl_buffer_send_release(buffer_to_present->resource);
    }
    surface->has_pending_attach = 0;
    surface->pending_buffer = NULL;
    if (presentable != surface) {
        presentable->has_pending_attach = 0;
        presentable->pending_buffer = NULL;
    }
    surface->server->commit_count++;
    surface->commit_count++;
    if (presentable != surface) {
        presentable->commit_count++;
    }
    send_focus_for_presentable(surface, presentable, buffer_to_present);
    if (!present_failed) {
        send_surface_presentation_feedback(presentable, 1);
    }
    if (presentable != surface) {
        send_surface_frame_callbacks(presentable);
        send_surface_presentation_feedback(surface, present_failed ? 0 : 1);
    }
    send_surface_frame_callbacks(surface);
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource, int32_t transform) {
    (void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale) {
    (void)client; (void)resource; (void)scale;
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client; (void)resource; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
};

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_state *state = wl_resource_get_user_data(resource);
    struct wl_resource *surface_resource = wl_resource_create(client, &wl_surface_interface, 5, id);
    struct surface_state *surface = calloc(1, sizeof(*surface));
    if (surface_resource == NULL || surface == NULL) {
        wl_client_post_no_memory(client);
        free(surface);
        return;
    }
    surface->server = state;
    surface->resource = surface_resource;
    wl_list_init(&surface->frame_callbacks);
    wl_list_init(&surface->presentation_feedbacks);
    wl_list_init(&surface->subsurface_children);
    wl_list_init(&surface->subsurface_link);
    wl_resource_set_implementation(surface_resource, &surface_impl, surface, destroy_surface_resource);
}

static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    (void)resource;
    if (region == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version > 6 ? 6 : version, id);
    struct server_state *state = data;
    if (state != NULL) {
        state->client_seen = 1;
    }
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}

int main(int argc, char **argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: %s <bridge-socket> <target-commits> <socket-file> <timeout-ms> <clear-ahb-outside> <accept-client-complete> <output-width> <output-height>\n", argv[0]);
        return 2;
    }
    struct server_state state;
    memset(&state, 0, sizeof(state));
    state.input_sock = -1;
    state.xtest_enabled = xtest_input_enabled();
    wl_list_init(&state.keyboard_resources);
    wl_list_init(&state.pointer_resources);
    wl_list_init(&state.touch_resources);
    state.bridge_sock = -1;
    state.last_frame_callback_commit = -1;
    const char *bridge_reconnect_frames_env = getenv("BRIDGE_RECONNECT_FRAMES");
    state.bridge_reconnect_frames = bridge_reconnect_frames_env == NULL
            ? 4096
            : atoi(bridge_reconnect_frames_env);
    if (state.bridge_reconnect_frames < 0) {
        state.bridge_reconnect_frames = 0;
    }
    const char *pass_log_interval_env = getenv("PASS_LOG_INTERVAL");
    state.pass_log_interval = pass_log_interval_env == NULL
            ? 0
            : atoi(pass_log_interval_env);
    if (state.pass_log_interval < 0) {
        state.pass_log_interval = 0;
    }
    const char *android_windows_env = getenv("WAYLANDIE_ANDROID_MULTI_WINDOW");
    state.android_windows = android_windows_env != NULL
            && android_windows_env[0] != '\0'
            && strcmp(android_windows_env, "0") != 0;
    const char *accept_scaled_primary_env = getenv("WAYLANDIE_WAYLAND_ACCEPT_SCALED_PRIMARY");
    state.accept_scaled_primary = accept_scaled_primary_env == NULL
            || accept_scaled_primary_env[0] == '\0'
            || strcmp(accept_scaled_primary_env, "0") != 0;
    state.bridge_socket_name = argv[1];
    state.target_commits = atoi(argv[2]);
    const char *socket_file = argv[3];
    double timeout_ms = atof(argv[4]);
    state.clear_ahb_outside = atoi(argv[5]) != 0;
    state.accept_client_complete = atoi(argv[6]) != 0;
    state.output_width = argc > 7 ? atoi(argv[7]) : 2688;
    state.output_height = argc > 8 ? atoi(argv[8]) : 1216;
    if (state.target_commits <= 0) {
        state.target_commits = 1;
    }
    if (state.output_width <= 0) {
        state.output_width = 2688;
    }
    if (state.output_height <= 0) {
        state.output_height = 1216;
    }
    if (timeout_ms <= 0.0) {
        timeout_ms = 15000.0;
    }
    const char *refresh_env = getenv("WAYLANDIE_WAYLAND_REFRESH_HZ");
    double refresh_hz = refresh_env != NULL && refresh_env[0] != '\0'
            ? atof(refresh_env)
            : 120.0;
    if (refresh_hz > 0.0) {
        state.presentation_refresh_nsec = (uint32_t)((1000000000.0 / refresh_hz) + 0.5);
    }
    const char *callback_mode_env = getenv("WAYLANDIE_WAYLAND_FRAME_CALLBACK_MODE");
    const char *callback_mode = callback_mode_env != NULL && callback_mode_env[0] != '\0'
            ? callback_mode_env
            : "paced";
    const char *frame_interval_env = getenv("FRAME_INTERVAL_MS");
    double frame_interval_override_ms =
            frame_interval_env != NULL && frame_interval_env[0] != '\0'
                    ? atof(frame_interval_env)
                    : -1.0;
    if (strcmp(callback_mode, "immediate") == 0
            || strcmp(callback_mode, "none") == 0
            || frame_interval_override_ms == 0.0) {
        state.frame_interval_ms = 0.0;
    } else if (frame_interval_override_ms > 0.0) {
        state.frame_interval_ms = frame_interval_override_ms;
    } else if (refresh_hz > 0.0) {
        state.frame_interval_ms = 1000.0 / refresh_hz;
    }
    state.display = wl_display_create();
    if (state.display == NULL) {
        printf("wayland-shm-ahb server=fail reason=display-create\n");
        return 1;
    }
    if (wl_global_create(state.display, &wl_compositor_interface, 6, &state, bind_compositor) == NULL
            || wl_global_create(state.display, &wl_subcompositor_interface, 1, NULL, bind_subcompositor) == NULL
            || wl_global_create(state.display, &wl_seat_interface, 9, &state, bind_seat) == NULL
            || wl_global_create(state.display, &wl_shm_interface, 1, NULL, bind_shm) == NULL
             || wl_global_create(state.display, &wl_output_interface, 4, &state, bind_output) == NULL
             || wl_global_create(state.display, &xdg_wm_base_interface, 6, NULL, bind_xdg_wm_base) == NULL
            || wl_global_create(state.display, &wp_presentation_interface, 2, NULL, bind_presentation) == NULL
            || wl_global_create(state.display, &wp_viewporter_interface, 1, NULL, bind_viewporter) == NULL
            || wl_global_create(state.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, bind_relative_pointer_manager) == NULL
            || wl_global_create(state.display, &zwp_pointer_constraints_v1_interface, 1, NULL, bind_pointer_constraints) == NULL
            || wl_global_create(state.display, &zwp_linux_dmabuf_v1_interface, 4, NULL, bind_linux_dmabuf) == NULL) {
        printf("wayland-shm-ahb server=fail reason=globals\n");
        wl_display_destroy(state.display);
        return 1;
    }
    const char *socket_name = wl_display_add_socket_auto(state.display);
    if (socket_name == NULL) {
        printf("wayland-shm-ahb server=fail reason=add-socket errno=%d\n", errno);
        wl_display_destroy(state.display);
        return 1;
    }
    FILE *socket_output = fopen(socket_file, "w");
    if (socket_output == NULL) {
        printf("wayland-shm-ahb server=fail reason=socket-file errno=%d\n", errno);
        wl_display_destroy(state.display);
        return 1;
    }
    fprintf(socket_output, "%s\n", socket_name);
    fclose(socket_output);
    printf("wayland-shm-ahb server=ready socket=%s target=%d timeout-ms=%.0f clear-ahb-outside=%d accept-client-complete=%d bridge-reconnect-frames=%d pass-log-interval=%d android-windows=%d accept-scaled-primary=%d frame-callback-mode=%s frame-interval-ms=%.3f presentation-refresh-nsec=%u\n",
            socket_name,
            state.target_commits,
            timeout_ms,
            state.clear_ahb_outside,
            state.accept_client_complete,
            state.bridge_reconnect_frames,
            state.pass_log_interval,
            state.android_windows,
            state.accept_scaled_primary,
            callback_mode,
            state.frame_interval_ms,
            state.presentation_refresh_nsec);
    fflush(stdout);

    struct wl_event_loop *loop = wl_display_get_event_loop(state.display);
    state.input_sock = connect_bridge_input_stream(&state);
    if (state.input_sock >= 0) {
        state.input_source = wl_event_loop_add_fd(
                loop,
                state.input_sock,
                WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
                input_stream_fd_event,
                &state);
        if (state.input_source == NULL) {
            printf("wayland-shm-ahb input-stream=event-source-fail\n");
            close(state.input_sock);
            state.input_sock = -1;
        }
    }
    double start_ms = now_ms();
    while (!state.abort_requested
            && state.commit_count < state.target_commits
            && (now_ms() - start_ms) < timeout_ms) {
        wl_event_loop_dispatch(loop, 20);
        ping_bound_xdg_wm_bases(&state);
        wl_display_flush_clients(state.display);
        if (state.accept_client_complete
                && state.client_seen
                && state.commit_count > 0
                && wl_list_empty(wl_display_get_client_list(state.display))) {
            state.completed_after_client_exit = 1;
            break;
        }
    }
    if (state.input_source != NULL) {
        wl_event_source_remove(state.input_source);
        state.input_source = NULL;
    }
    if (state.input_sock >= 0) {
        close(state.input_sock);
        state.input_sock = -1;
    }
    wl_display_destroy_clients(state.display);
    wl_display_destroy(state.display);
    if (state.bridge_sock >= 0) {
        close(state.bridge_sock);
        state.bridge_sock = -1;
    }
    if (state.xtest_display != NULL) {
        if (state.xtest_window != None) {
            XDestroyWindow(state.xtest_display, state.xtest_window);
            state.xtest_window = None;
        }
        XCloseDisplay(state.xtest_display);
        state.xtest_display = NULL;
    }
    double elapsed_ms = now_ms() - start_ms;
    double avg_present = state.commit_count > 0 ? state.total_present_ms / (double)state.commit_count : 0.0;
    double avg_app_wait = state.app_wait_samples > 0 ? state.total_app_wait_us / (double)state.app_wait_samples : 0.0;
    double avg_app_slot_wait = state.app_slot_wait_samples > 0 ? state.total_app_slot_wait_us / (double)state.app_slot_wait_samples : 0.0;
    printf(
        "wayland-shm-ahb summary commits=%d target=%d failures=%d elapsed-ms=%.2f avg-gpu-present-ms=%.3f avg-app-wait-us=%.1f avg-app-slot-wait-us=%.1f zero-copy=dmabuf-present\n",
        state.commit_count,
        state.target_commits,
        state.present_failures,
        elapsed_ms,
        avg_present,
        avg_app_wait,
        avg_app_slot_wait);
    if ((state.commit_count >= state.target_commits || state.completed_after_client_exit)
            && state.present_failures == 0) {
        printf("wayland-shm-ahb verdict=pass\n");
        return 0;
    }
    printf("wayland-shm-ahb verdict=fail\n");
    return 1;
}
EOF

cat >"$tmpdir/wayland-shm-ahb-client.c" <<'EOF'
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

struct client_state {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
};

static void registry_global(
        void *data,
        struct wl_registry *registry,
        uint32_t name,
        const char *interface,
        uint32_t version) {
    struct client_state *state = (struct client_state *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 5 ? version : 5);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void sleep_ms(int interval_ms) {
    struct timespec duration;
    duration.tv_sec = interval_ms / 1000;
    duration.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0) {
    }
}

static void fill_frame(uint32_t *pixels, int width, int height, int stride_pixels, int frame_index) {
    for (int y = 0; y < height; y++) {
        uint32_t *row = pixels + ((size_t)y * (size_t)stride_pixels);
        for (int x = 0; x < width; x++) {
            uint8_t r = (uint8_t)((x + frame_index * 19) & 0xff);
            uint8_t g = (uint8_t)((y + frame_index * 31) & 0xff);
            uint8_t b = (uint8_t)((x / 3 + y / 2 + frame_index * 7) & 0xff);
            row[x] = 0xff000000U | ((uint32_t)r << 16U) | ((uint32_t)g << 8U) | (uint32_t)b;
        }
    }
}

static int create_tmpfile(size_t size) {
    char template[] = "/tmp/wayland-shm-ahb-buffer.XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        return -1;
    }
    unlink(template);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <socket> <commits> <interval-ms> <width> <height>\n", argv[0]);
        return 2;
    }
    const char *socket_name = argv[1];
    int commits = atoi(argv[2]);
    int interval_ms = atoi(argv[3]);
    int width = atoi(argv[4]);
    int height = atoi(argv[5]);
    if (commits <= 0) commits = 1;
    if (interval_ms < 0) interval_ms = 0;
    if (width <= 0) width = 2688;
    if (height <= 0) height = 1216;
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;
    int fd = create_tmpfile(size);
    if (fd < 0) {
        printf("wayland-shm-client shm-file=fail errno=%d\n", errno);
        return 1;
    }
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        printf("wayland-shm-client mmap=fail errno=%d\n", errno);
        close(fd);
        return 1;
    }

    struct client_state state;
    memset(&state, 0, sizeof(state));
    struct wl_display *display = wl_display_connect(socket_name);
    if (display == NULL) {
        printf("wayland-shm-client connect=fail socket=%s\n", socket_name);
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display);
    if (state.compositor == NULL || state.shm == NULL) {
        printf("wayland-shm-client globals=missing compositor=%s shm=%s\n",
               state.compositor == NULL ? "no" : "yes",
               state.shm == NULL ? "no" : "yes");
        return 1;
    }
    struct wl_surface *surface = wl_compositor_create_surface(state.compositor);
    struct wl_shm_pool *pool = wl_shm_create_pool(state.shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
            pool,
            0,
            width,
            height,
            stride,
            WL_SHM_FORMAT_XRGB8888);
    printf("wayland-shm-client connected socket=%s commits=%d size=%dx%d interval-ms=%d\n",
           socket_name, commits, width, height, interval_ms);
    for (int i = 0; i < commits; i++) {
        fill_frame(pixels, width, height, width, i);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width, height);
        wl_surface_commit(surface);
        wl_display_flush(display);
        wl_display_roundtrip(display);
        printf("wayland-shm-client commit=%d\n", i);
        if (interval_ms > 0 && i + 1 < commits) {
            sleep_ms(interval_ms);
        }
    }
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    wl_surface_destroy(surface);
    wl_shm_destroy(state.shm);
    wl_compositor_destroy(state.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    munmap(pixels, size);
    close(fd);
    printf("wayland-shm-client verdict=pass\n");
    return 0;
}
EOF

wayland-scanner server-header "$XDG_SHELL_XML" "$tmpdir/xdg-shell-server-protocol.h"
wayland-scanner private-code "$XDG_SHELL_XML" "$tmpdir/xdg-shell-protocol.c"
wayland-scanner server-header "$LINUX_DMABUF_XML" "$tmpdir/linux-dmabuf-unstable-v1-server-protocol.h"
wayland-scanner private-code "$LINUX_DMABUF_XML" "$tmpdir/linux-dmabuf-unstable-v1-protocol.c"
wayland-scanner server-header "$PRESENTATION_TIME_XML" "$tmpdir/presentation-time-server-protocol.h"
wayland-scanner private-code "$PRESENTATION_TIME_XML" "$tmpdir/presentation-time-protocol.c"
wayland-scanner server-header "$VIEWPORTER_XML" "$tmpdir/viewporter-server-protocol.h"
wayland-scanner private-code "$VIEWPORTER_XML" "$tmpdir/viewporter-protocol.c"
wayland-scanner server-header "$RELATIVE_POINTER_XML" "$tmpdir/relative-pointer-unstable-v1-server-protocol.h"
wayland-scanner private-code "$RELATIVE_POINTER_XML" "$tmpdir/relative-pointer-unstable-v1-protocol.c"
wayland-scanner server-header "$POINTER_CONSTRAINTS_XML" "$tmpdir/pointer-constraints-unstable-v1-server-protocol.h"
wayland-scanner private-code "$POINTER_CONSTRAINTS_XML" "$tmpdir/pointer-constraints-unstable-v1-protocol.c"

cc -Wall -Wextra \
  -o "$tmpdir/wayland-shm-ahb-server" \
  "$tmpdir/wayland-shm-ahb-server.c" \
  "$tmpdir/xdg-shell-protocol.c" \
  "$tmpdir/linux-dmabuf-unstable-v1-protocol.c" \
  "$tmpdir/presentation-time-protocol.c" \
  "$tmpdir/viewporter-protocol.c" \
  "$tmpdir/relative-pointer-unstable-v1-protocol.c" \
  "$tmpdir/pointer-constraints-unstable-v1-protocol.c" \
  $(pkg-config --cflags --libs wayland-server x11 xtst)

cc -Wall -Wextra \
  -o "$tmpdir/wayland-shm-ahb-client" \
  "$tmpdir/wayland-shm-ahb-client.c" \
  $(pkg-config --cflags --libs wayland-client)

socket_file="$tmpdir/socket-name.txt"
server_log="$tmpdir/server.log"
client_log="$tmpdir/client.log"

printf 'wayland-shm-ahb cc=%s\n' "$(command -v cc)"
printf 'wayland-shm-ahb XDG_RUNTIME_DIR=%s\n' "$XDG_RUNTIME_DIR"
printf 'wayland-shm-ahb bridge=abstract:%s frames=%s interval-ms=%s client=%sx%s mode=%s timeout-ms=%s clear-ahb-outside=%s\n' \
  "$BRIDGE_LOCAL_SOCKET" "$FRAME_COUNT" "$FRAME_INTERVAL_MS" "$CLIENT_WIDTH" "$CLIENT_HEIGHT" "$CLIENT_MODE" "$SERVER_TIMEOUT_MS" "$CLEAR_AHB_OUTSIDE"
if [ "$CLIENT_MODE" = "external" ]; then
  printf 'wayland-shm-ahb external-client-command=%s\n' "$EXTERNAL_CLIENT_COMMAND"
fi

"$tmpdir/wayland-shm-ahb-server" \
  "$BRIDGE_LOCAL_SOCKET" \
  "$FRAME_COUNT" \
  "$socket_file" \
  "$SERVER_TIMEOUT_MS" \
  "$CLEAR_AHB_OUTSIDE" \
  "$ACCEPT_CLIENT_COMPLETE" \
  "$CLIENT_WIDTH" \
  "$CLIENT_HEIGHT" >"$server_log" 2>&1 &
server_pid=$!

for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [ -s "$socket_file" ]; then
    break
  fi
  sleep 0.1
done

if [ ! -s "$socket_file" ]; then
  printf 'wayland-shm-ahb FAIL: server did not publish socket\n' >&2
  sed 's/^/wayland-shm-ahb server-log: /' "$server_log" || true
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  exit 1
fi

wayland_socket="$(sed -n '1p' "$socket_file")"
set +e
if [ "$CLIENT_MODE" = "external" ]; then
  (
    export WAYLAND_DISPLAY="$wayland_socket"
    export XDG_RUNTIME_DIR
    sh -c "$EXTERNAL_CLIENT_COMMAND"
  ) >"$client_log" 2>&1
else
  "$tmpdir/wayland-shm-ahb-client" "$wayland_socket" "$FRAME_COUNT" "$FRAME_INTERVAL_MS" "$CLIENT_WIDTH" "$CLIENT_HEIGHT" >"$client_log" 2>&1
fi
client_status=$?
if [ "$CLIENT_MODE" = "external" ] && [ "$client_status" -ne 0 ]; then
  kill "$server_pid" 2>/dev/null || true
fi
wait "$server_pid"
server_status=$?
set -e

sed 's/^/wayland-shm-ahb server-log: /' "$server_log"
sed 's/^/wayland-shm-ahb client-log: /' "$client_log"

if [ "$client_status" -ne 0 ]; then
  printf 'wayland-shm-ahb FAIL: client exited %d\n' "$client_status" >&2
  exit 1
fi
if [ "$server_status" -ne 0 ]; then
  printf 'wayland-shm-ahb FAIL: server exited %d\n' "$server_status" >&2
  exit 1
fi

if grep -q 'wayland-shm-ahb verdict=pass' "$server_log"; then
  if [ "$CLIENT_MODE" = "external" ] || grep -q 'wayland-shm-client verdict=pass' "$client_log"; then
  printf 'wayland-shm-ahb verdict=pass\n'
  exit 0
  fi
fi

printf 'wayland-shm-ahb FAIL: missing pass verdict\n' >&2
exit 1
