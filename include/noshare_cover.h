/*
 * noshare-cover: boundary between the Rust core and the Hyprland C++ shim.
 * Implemented in src/ffi.rs and src/extra.rs. Change them together.
 *
 * All functions are called from the Hyprland render thread. None of them throws
 * or panics across the boundary. Frame data (pixels, fd) lives until nsc_end_frame().
 */
#ifndef NOSHARE_COVER_H
#define NOSHARE_COVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* public API types; the shim doesn't need the binder, it is this plugin */
#define NOSHARE_COVER_NO_BINDER
#include "noshare_cover_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* path_cover; /* "" = default file from ~/.config/hypr */
    bool        loop;
    double      speed;
    const char* backend;    /* "auto" | "gpu" | "cpu" */
    const char* gpu_device; /* "" = first render node */
} nsc_settings;

/* Overrides from a window rule; NULL = field not set. */
typedef struct {
    const char* rule_path;
    const char* rule_speed;
    const char* rule_loop;
} nsc_play_request;

enum {
    NSC_FRAME_NONE   = 0,
    NSC_FRAME_CPU    = 1, /* pixels: premultiplied BGRA, fourcc = DRM_FORMAT_ARGB8888 */
    NSC_FRAME_DMABUF = 2, /* planes[]: do NOT close fd, the core owns it; EGL doesn't take ownership */
};

typedef struct {
    int32_t  fd;
    uint32_t offset;
    uint32_t stride;
} nsc_plane;

typedef struct {
    uint64_t       cover_id;   /* stable texture cache key */
    uint64_t       generation; /* changed: update the texture */
    uint32_t       kind;
    uint32_t       width;
    uint32_t       height;
    uint32_t       stride;
    const uint8_t* pixels;
    uint32_t       fourcc;
    uint64_t       modifier;
    uint32_t       plane_count;
    nsc_plane      planes[4];
} nsc_frame;

/* Rect from another plugin (see noshare_cover_api.h), client already stripped. */
typedef struct {
    double   x, y, w, h, rounding;
    uint64_t window; /* window address to fill with its cover, 0 = none */
    uint32_t fill;   /* 0 = black, 1 = window cover */
} nsc_extra_rect;

/*
 * Struct layout is pinned by numbers on both sides: static_assert here, the
 * ffi::tests::abi_layout test in Rust. If they drift, the build or the test fails,
 * not Hyprland at runtime. Numbers are for LP64 (x86_64 / aarch64 Linux).
 */
#if defined(__LP64__)
#ifdef __cplusplus
#define NSC_ASSERT static_assert
#else
#define NSC_ASSERT _Static_assert
#endif
NSC_ASSERT(sizeof(nsc_frame) == 112, "nsc_frame layout");
NSC_ASSERT(offsetof(nsc_frame, pixels) == 32 && offsetof(nsc_frame, modifier) == 48 && offsetof(nsc_frame, planes) == 60, "nsc_frame offsets");
NSC_ASSERT(sizeof(nsc_plane) == 12, "nsc_plane layout");
NSC_ASSERT(sizeof(nsc_settings) == 40 && offsetof(nsc_settings, speed) == 16, "nsc_settings layout");
NSC_ASSERT(sizeof(nsc_play_request) == 24, "nsc_play_request layout");
NSC_ASSERT(sizeof(nsc_extra_rect) == 56 && offsetof(nsc_extra_rect, fill) == 48, "nsc_extra_rect layout");
NSC_ASSERT(sizeof(noshare_cover_rect) == 56 && offsetof(noshare_cover_rect, window) == 40, "noshare_cover_rect layout");
#undef NSC_ASSERT
#endif

bool     nsc_init(void);
void     nsc_shutdown(void);
void     nsc_set_settings(const nsc_settings* settings);
uint64_t nsc_epoch(void);
void     nsc_begin_frame(void);
bool     nsc_resolve(const nsc_play_request* request, nsc_frame* out);
void     nsc_prewarm(const nsc_play_request* request);
void     nsc_end_frame(void);
bool     nsc_animating(void);
bool     nsc_cover_alive(uint64_t cover_id);
size_t   nsc_take_notification(char* buf, size_t cap);
size_t   nsc_extra_rects(int64_t monitor_id, nsc_extra_rect* out, size_t cap);

/* Public ABI implementation (noshare_cover_api.h). It is exported by the
 * noshare_cover_* wrappers in shim/plugin.cpp: the whole Rust archive is linked
 * hidden (--exclude-libs) so it doesn't share symbols with other plugins. */
uint32_t nsc_api_api_version(void);
bool     nsc_api_set_gone_callback(uint64_t client, void (*cb)(void* user), void* user);
void     nsc_api_notify_gone(void);
uint64_t nsc_api_register_client(const char* name);
void     nsc_api_unregister_client(uint64_t client);
bool     nsc_api_set_rects(uint64_t client, int monitor_id, const noshare_cover_rect* rects, size_t count);
bool     nsc_api_clear_client_rects(uint64_t client);
void     nsc_api_clear_extra_rects(void);
void     nsc_api_add_extra_rect(int monitor_id, double x, double y, double w, double h, double rounding);

#ifdef __cplusplus
}
#endif

#endif
