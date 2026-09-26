# noshare-cover

[Русский](README.ru.md) · [Architecture & status](ARCHITECTURE.md)

Hyprland plugin. Windows with `no_screen_share` are covered by an image or a video in a
screen capture, instead of Hyprland's black box. On the real screen the window stays as it is.

Version 2.0 is a rewrite: the core is Rust (media, decoding, playback clock, lifecycle,
public API), and a thin C++ layer talks to Hyprland's C++ plugin API. No FFmpeg, no cairo,
no external processes.

> **Status.** Works end to end on a live Hyprland (0.56.2, Arch, llvmpipe): a window with
> `no_screen_share` shows the cover in a `grim` capture — still image, GIF, H.264, AV1, VP9 —
> and survives repeated `hyprctl plugin unload/load` without thread or memory growth
> (`tests/e2e/run.sh`). Builds with Nix against Hyprland 0.56.0 and 0.56.2 from nixpkgs and
> with makepkg on Arch. GPU decoding (NVDEC, VA-API) is implemented but was not run on real
> GPUs yet — see [Checking the GPU path](#checking-the-gpu-path).

## Video decoding

| | Codecs | Where it comes from |
|---|---|---|
| NVDEC (NVIDIA) | H.264, HEVC, VP8, VP9, AV1 | `libcuda`/`libnvcuvid` from the driver, dlopen |
| VA-API (Intel, AMD) | H.264, HEVC, VP8, VP9, AV1 (8-bit) | embedded helper over cros-codecs, needs `libva` |
| CPU | AV1 | rav1d, built in |
| CPU | H.264 | system `openh264`, dlopen |
| CPU | VP8, VP9 | system `libvpx`, dlopen |

`backend = "auto"` tries the GPU of `gpu_device` (or the first render node) and falls back to
the CPU; `"gpu"` never falls back; `"cpu"` never touches the GPU. A missing library is not an
error for the plugin: it tells you once which package to install and uses what is there.

While a monitor with a video or GIF cover is being shared, the plugin damages the cover area
~60 times a second. Hyprland only produces capture frames when the monitor repaints, so
otherwise the cover would stall on a static monitor.

## Config

```lua
hl.config({
    plugin = {
        no_screen_share_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            path_cover = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
            -- video decode: "auto" (GPU if possible, else CPU), "gpu" (only GPU), "cpu"
            backend = "auto",
            -- render node for GPU decode; empty = first /dev/dri/renderD*
            gpu_device = "",
            -- ms to keep the cover after a closed window's animation ends
            close_hold = 0,
            -- capture clients (exe names) that see hidden windows as they are
            show_to = "",
            -- if set, hide only from these clients; everyone else sees everything
            hide_from = "",
        },
    },
})
```

`path_cover` is the fallback. Without it the plugin uses the first existing file among
`~/.config/hypr/noshare-cover.{gif,jpg,jpeg,png,mp4}`; if there is none, windows without a
cover of their own just get the black box, no error. `~` is expanded.

A window can override the media, speed and loop with a rule. Plugin rule fields are flat,
plain Lua names, so `hl.window_rule` takes them directly, no wrappers:

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/67.mp4", -- media for this window
    no_screen_share_cover_speed = 1.5,                        -- optional
    no_screen_share_cover_loop = false,                       -- optional
    no_screen_share_cover_hold = 300,                         -- optional, overrides close_hold
})
```

Layer-shell surfaces (bars, launchers, wallpapers) work the same way through layer rules:

```lua
hl.layer_rule({
    match = { namespace = "waybar" },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/bar.png",
})
```

The field names of the original plugin (`["no_screen_share_cover:path_cover"]`,
`[":speed"]`, `[":loop"]`) still work.

Without `no_screen_share` the window is not covered. If several rules match, the last value wins.

A window placed on top of a hidden one stays visible in the stream: the cover (or the black box)
is drawn only where the hidden window actually shows. Under a translucent window on top the
cover is drawn too and the window is drawn again over it, with its own opacity and blur, so what
shows through it is the cover, never the hidden window. Layers above windows (bars, launchers,
notifications) are drawn again over the cover the same way.

When a window or layer closes, Hyprland replaces it with a snapshot for the close animation,
and `no_screen_share` does not apply to that snapshot, so without the plugin its content
flashes in the stream. The cover follows the snapshot until the animation ends, then stays for
`close_hold` ms (per window or layer rule: `no_screen_share_cover_hold`). `0` covers just the
animation.

The same hold applies when a window stops being hidden while it stays on screen, e.g. a rule
that matches the title stops matching. A browser changes the window title before it repaints,
so when you switch away from a matching tab the old page would otherwise reach the stream for
a frame or two; set `no_screen_share_cover_hold` (e.g. 300) on such a rule. Clients listed in
`show_to` get no hold.

By default hidden windows are covered in every capture: portal streams (browsers, Discord,
OBS via PipeWire) and clients that capture the screen directly (grim, wf-recorder,
gpu-screen-recorder, OBS with wlrobs). The capture client is told apart by its executable
(`/proc/<pid>/exe` of the Wayland client). `show_to = "grim, hyprshot"` lets those clients see
hidden windows as they are, for example for your own screenshots. `hide_from =
"xdg-desktop-portal-hyprland"` does the opposite: windows are hidden only from the listed
clients (here, only from portal streams) and a client that can't be identified is still
covered. Names are separated by commas or spaces. With both lists empty the plugin never
looks the client up.

The same lists work per window or layer rule, and then only the rule's own lists apply to
that surface:

```lua
hl.window_rule({ match = { class = "org.telegram.desktop" }, no_screen_share = true,
    no_screen_share_show_to = "grim" })               -- screenshots see it, streams don't
hl.layer_rule({ match = { namespace = "waybar" }, no_screen_share = true,
    no_screen_share_hide_from = "xdg-desktop-portal-hyprland" })
```

A hidden background layer (e.g. the wallpaper) is covered over its whole area, including the
windows on top of it, the same as Hyprland's own black box.

Cursor zoom (`cursor:zoom_factor`) is handled too: the stream gets the zoomed image, while
Hyprland places its own `no_screen_share` boxes as if there were no zoom, so they miss the
windows. While the monitor is zoomed the plugin draws all the boxes itself where the windows
actually are: covers, or plain black for windows (and their popups) without a cover.

Errors (missing file, unknown format, broken video, bad `backend`) are shown once as a
Hyprland notification, not every frame. A missing file is picked up automatically as soon
as it appears.

With hyprpm you may see `unknown config key 'plugin.no_screen_share_cover...'` at startup:
the config is read before hyprpm loads the plugin. Hyprland reloads the config after the
plugin is loaded and the error goes away.

## API for other plugins

Overlays that draw previews of windows (e.g. gloview) can ask noshare-cover to cover their
boxes too — with black or with the same cover as the window. Header-only, nothing to link:
[`include/noshare_cover_api.h`](include/noshare_cover_api.h).

```c
#include "noshare_cover_api.h"

static noshare_cover_api nsc;
static uint64_t          client;

// PLUGIN_INIT (or later, once noshare-cover is there)
if (noshare_cover_bind(&nsc) == 0) {
    client = nsc.register_client("my-overlay");
    // optional: hear about noshare-cover unloading and don't pin it in memory
    if (nsc.set_gone_callback && nsc.set_gone_callback(client, on_gone, NULL))
        noshare_cover_drop_handle(&nsc);
}

// each overlay frame, per monitor — atomically replaces this client's rects there
noshare_cover_rect r = {x, y, w, h, rounding, window_address, NOSHARE_COVER_FILL_WINDOW};
nsc.set_rects(client, monitor_id, &r, 1);

// overlay closed:   nsc.set_rects(client, monitor_id, NULL, 0);
// PLUGIN_EXIT:      nsc.unregister_client(client); noshare_cover_unbind(&nsc);
```

Coordinates are global layout pixels (same space as window position/size). Each client owns
its rects; one plugin can't wipe another's. `on_gone` is called from noshare-cover's
`PLUGIN_EXIT` after its `renderMonitor` hook is removed: forget every pointer into it. The v1
functions `noshare_cover_clear_extra_rects` / `noshare_cover_add_extra_rect` still work
unchanged.

If `renderMonitor` is already hooked by another plugin (gloview does that while noshare-cover
isn't loaded), noshare-cover doesn't refuse to load; it waits until the hook is released.
gloview releases it as soon as it sees noshare-cover, so load order doesn't matter.

## Dependencies

**To build**

| What | Why | Arch | Nix |
|---|---|---|---|
| `cargo` / `rustc` (1.89+) | the Rust core | `rust` | in the flake |
| C++ compiler (C++26), `make`, `pkg-config` | the Hyprland shim | `base-devel`, `pkgconf` | in the flake |
| Hyprland headers | the plugin API | `hyprland` (hyprpm installs its own) | from the Hyprland package |
| `nasm` | AV1 decoder assembly (rav1d) | `nasm` | in the flake |
| `clang` / libclang | bindings for the VA-API helper | `clang` | `bindgenHook` |
| `libva`, `gbm` headers | the VA-API helper | `libva`, `mesa` | in the flake |

`make NSC_VAAPI=0` builds without VA-API; then clang, libva and gbm are not needed. `make`
checks everything first and lists what is missing.

**At runtime** (all optional, the plugin loads without any of them)

| What | For | Arch |
|---|---|---|
| NVIDIA driver (`libcuda`, `libnvcuvid`) | GPU decoding on NVIDIA (NVDEC) | `nvidia-utils` |
| `libva` + a VA-API driver | GPU decoding on Intel/AMD | `libva` + `mesa` / `intel-media-driver` |
| `openh264` | H.264 on the CPU | `openh264` |
| `libvpx` | VP8/VP9 on the CPU | `libvpx` |

AV1 on the CPU, images and GIFs need nothing extra. With Nix, `openh264` and `libvpx` are
referenced from the store; the NVIDIA and VA-API drivers come from the system
(`/run/opengl-driver`).

## Install

### hyprpm (Arch and others)

Needs `cargo`, `nasm`, `clang` and `libva` headers to build (Arch:
`pacman -S rust pkgconf nasm clang libva`). If something is missing, `make` says what and
which package to install. Without VA-API: `make NSC_VAAPI=0`.

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

No trailing `/` in the URL: with it hyprpm can't derive the repository name and installs the
plugin outside its own directory. hyprpm builds against the running Hyprland and loads the
plugin itself. Do not also call `hl.plugin.load`.

### Arch

`packaging/arch/PKGBUILD` (`makepkg -si`). Rebuild it after every `hyprland` update: a plugin
built for other headers refuses to load instead of crashing the compositor.

```lua
hl.plugin.load("/usr/lib/hyprland/plugins/libnoshare-cover.so")
```

### Nix

The plugin must be built against the exact Hyprland you run, otherwise it refuses to load with
`built for Hyprland <commit>, running <commit>`. The easiest way is the Home Manager module: it
builds against the system `programs.hyprland.package` when the NixOS module is enabled (that is
the Hyprland started through `/run/wrappers`), otherwise against
`wayland.windowManager.hyprland.package`:

```nix
inputs.noshare-cover = {
  url = "github:gitscout-bot/noshare-cover";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

```nix
imports = [ inputs.noshare-cover.homeManagerModules.default ];
programs.noshare-cover.enable = true;
# adds it to wayland.windowManager.hyprland.plugins; to load it yourself use
# "${config.programs.noshare-cover.package}/lib/libnoshare-cover.so"
```

Without Home Manager, on NixOS:
`inputs.noshare-cover.lib.mkNoshareCover pkgs config.programs.hyprland.package`.

The prebuilt packages only fit specific setups. `packages.default` is built against the flake's
`hyprland` input, so it is right only if you run Hyprland from that same input (add
`inputs.hyprland.follows = "hyprland"` and set `programs.hyprland.package` to it).
`packages.nixpkgs` is built against `hyprland` from the nixpkgs this flake is locked to. There is
also `overlays.default` (`pkgs.hyprlandPlugins.noshare-cover`, built against `final.hyprland`).

To see which Hyprland is running: `hyprctl version`.

## Checking the GPU path

```sh
NOSHARE_COVER_DEBUG=/tmp/nsc.log Hyprland   # or export it in the session
```

Set `backend = "gpu"` and a video `path_cover`, start any screen capture, then read
`/tmp/nsc.log` and the Hyprland notification: with `gpu` the plugin never falls back, so a GPU
problem is reported instead of hidden. Useful: `vainfo` (VA-API driver present), `nvidia-smi`.

## Development

```sh
cargo test                 # core: config, clock, media, demux, decoders, registry, API, ABI layout
NSC_TEST_MEDIA=dir cargo test   # + real files: h264.mp4, av1.mp4, vp9.webm (decoded and checked)
tests/e2e/run.sh ./libnoshare-cover.so <media dir>   # live Hyprland + grim, unload/load cycles
tests/e2e/with-gloview.sh ./libnoshare-cover.so ./gloview.so   # together with gloview
cargo clippy --all-targets -- -D warnings
make                       # libnoshare-cover.so (Hyprland headers via pkg-config; NSC_VAAPI=0 skips VA-API)
nix build                  # hermetic build + tests
```
