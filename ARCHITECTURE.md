# noshare-cover 2.0 architecture

## Why not "pure Rust"

A Hyprland plugin is a `.so` tied to the C++ ABI of one Hyprland build. Drawing into a
screencopy frame needs a hook on the private method `Screenshare::CScreenshareFrame::renderMonitor`
plus render pass classes (`CTexPassElement`, `CRectPassElement`), window/layer rules
(`Desktop::Rule`) and config values (`Config::Values`). Hyprland has no C API for any of
that, and its C++ ABI can't be called from Rust reliably (`SP<>` templates, virtual classes,
header versions). So: **all logic lives in Rust, with a thin C++ layer only where the C++
ABI is unavoidable.** The layer is a single file with no media logic.

```
                    ┌──────────────── Hyprland (C++) ───────────────────┐
  CScreenshareFrame │ renderMonitor ──hook──▶ shim/plugin.cpp           │
                    │                         │ config, window/layer     │
                    │                         │ rules, geometry,         │
                    │                         │ textures, render pass    │
                    └─────────────────────────┼──────────────────────────┘
                                              │ C ABI (include/noshare_cover.h)
                    ┌─────────────────────────▼──────── Rust (src/) ─────┐
                    │ ffi.rs      entry points, panic guards              │
                    │ registry.rs covers: cache, epoch, errors, eviction  │
                    │ config.rs   settings, rule overrides, backend, GPU  │
                    │ media/      still (png/jpg) · gif · video           │
                    │   video/    demux → decode → YUV→BGRA · worker      │
                    │ gpu.rs      render node selection                   │
                    │ frame.rs    frame: BGRA pixels or dmabuf            │
                    │ extra.rs    public API for other plugins            │
                    └────────────────────────────────────────────────────┘
```

The VA-API decoder is a separate small library, `vaapi-helper` (cros-codecs over libva).
Its bytes are embedded into the plugin at build time and it is loaded from a memfd with
dlopen the first time a GPU video is opened. libva/libgbm are dependencies of the helper
only: without libva there is no VA-API, but the plugin still loads and uses the CPU.

## A screencopy frame

1. Hyprland renders a screencopy frame → our hook → the original → `paintCovers`.
2. The layer hands the current config to the core (`nsc_set_settings`, also on every config
   reload). If it changed, the core closes all sources and bumps the epoch; the layer then
   drops its textures. The default cover is opened right away so the first capture already
   has a frame.
3. For every window and layer with `no_screen_share`: geometry as Hyprland computes it,
   rule overrides → `nsc_resolve` → frame. Covers with the same (file, speed, loop) share one
   source and one texture; a source is polled once per frame however many windows show it.
4. Frame → texture: pixels via `createTexture(drmFormat, pixels, stride, size)` /
   `ITexture::update`, dmabuf via `createTexture(SDMABUFAttrs)`. The texture is only updated
   when `generation` changes.
5. Rectangles from other plugins (gloview's overview tiles) get black or the window's cover.
6. `nsc_end_frame`: covers nobody showed for 30 s are closed (for video that stops the
   thread). Every 120 frames the layer drops textures of dead covers.
7. While a monitor is being shared and shows an animated cover, a timer damages the cover
   boxes at ~60 Hz. Hyprland only produces capture frames when the monitor repaints, so a
   video cover would otherwise stall on a static monitor.

## Video

One thread per source. Only the latest decoded frame is exposed (no queue, no memory
growth). The thread sleeps when nobody asks for frames for 400 ms; after waking up the
clock restarts from the current frame instead of catching up. Frames more than 80 ms late
are dropped. The first poll of a new source waits up to 120 ms for its first frame.
Dropping a source = `quit` + `notify` + `join`: unloading the plugin leaves no threads.
A decoder panic is caught in the thread and reported as an error.

Backend (`backend` in the config): `auto` tries the GPU (NVDEC on NVIDIA render nodes, then
VA-API) and falls back to the CPU; `gpu` never falls back; `cpu` is software only.
`gpu_device` pins a render node, otherwise the first `/dev/dri/renderD*` is used. Every
refusal is explained in a message the user sees.

YUV → BGRA runs on the worker thread, split across up to 4 threads for frames of ~1 MP and up.

## Coexisting with other plugins

Hyprland's function hooks are exclusive: two plugins can't hook `renderMonitor` at once.
gloview hooks it itself while noshare-cover isn't loaded. If the hook is taken when
noshare-cover loads, noshare-cover keeps retrying instead of refusing to load; gloview
re-checks after every config reload (Hyprland reloads after each plugin load/unload),
lets go of the hook and switches to noshare-cover's API. On unload noshare-cover removes
its hook first and then calls API clients' gone callbacks, so gloview can take the hook
back and never has to keep a dlopen handle that would pin noshare-cover in memory.

## Lifecycle and safety

- No panic reaches Hyprland: every `extern "C"` entry is wrapped in `catch_unwind`.
- `PLUGIN_EXIT`: stop timers, remove the hook first (Hyprland removes hooks only *after*
  EXIT), notify API clients, drop textures, `nsc_shutdown` (joins all threads), then rule
  effects and config values.
- The Rust archive is linked hidden (`--exclude-libs,ALL`); the public API is exported by
  thin wrappers in `shim/plugin.cpp`. Do not use a version script with `local: *`: it also
  localizes Hyprland's inline globals (`g_pHyprRenderer` & co) and the plugin ends up with
  its own null copies.
- `-fno-gnu-unique`: without it glibc marks the `.so` NODELETE and a reload silently gets
  the old image.
- On load the plugin compares `__hyprland_api_get_hash()` with the headers it was built
  against and refuses to load on mismatch instead of crashing the compositor.
- C ABI struct layouts are pinned by numbers on both sides (`static_assert` in the header,
  the `ffi::tests::abi_layout` test).

## Status

| Part | State |
|---|---|
| Config, window and layer rules (flat Lua fields), `backend`, `gpu_device` | ✅ tests + live Hyprland |
| PNG / JPEG / GIF | ✅ tests + live Hyprland |
| MP4/MOV, WebM/MKV demuxers | ✅ tests on real files |
| CPU: AV1 (rav1d), H.264 (openh264), VP8/VP9 (libvpx), YUV→BGRA | ✅ tests on real files + live Hyprland |
| NVDEC (dlopen, driver parser, NV12/P016 copy) | ⚠️ builds, struct layouts checked against ffnvcodec-headers; not run on a GPU |
| VA-API (vaapi-helper on cros-codecs, embedded, loaded from memfd) | ⚠️ builds, helper loading checked; not run on a GPU; 8-bit only |
| `shim/plugin.cpp` (0.56.x and 0.57.x) | ✅ builds against 0.56.0/0.56.2/0.57.0, runs on 0.56.2 and 0.57.0 |
| Together with gloview | ✅ `tests/e2e/with-gloview.sh`: both load orders, unload/reload, overview tiles |
| Unload/load, leaks | ✅ `tests/e2e/run.sh`: 8 cycles, threads and RSS stable |
| Packaging: hyprpm, Arch PKGBUILD, Nix flake | ✅ makepkg and nix build (0.56.0, 0.56.2) |

## What to check on a real machine

- NVDEC on NVIDIA and VA-API on Intel/AMD (`backend = "gpu"`, `NOSHARE_COVER_DEBUG`);
- long video runs (hours): Hyprland RSS and fd count stay flat;
- two GPUs with `gpu_device`;
- portal-based capture (OBS, Discord) on a static monitor while working on another one.
