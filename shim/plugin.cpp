// noshare-cover: thin shim between Hyprland and the Rust core.
//
// Only what can't be done without the Hyprland C++ ABI lives here:
//   - the CScreenshareFrame::renderMonitor hook (screencast frame);
//   - registering config values and window rule effects;
//   - window geometry and drawing via the render pass;
//   - turning core frames into textures (pixels or dmabuf, no cairo).
// All media, decoding, clock and lifecycle logic is in Rust (src/).

#include "../include/noshare_cover.h"

#include <drm_fourcc.h>

#include <algorithm>
#include <any>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unistd.h>

#ifndef NSC_VERSION // set by the Makefile from Cargo.toml
#define NSC_VERSION "unknown"
#endif
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "config/ConfigValue.hpp"
#include "config/values/ConfigValues.hpp"
#include "desktop/rule/Engine.hpp"
#include "desktop/rule/layerRule/LayerRule.hpp"
#include "desktop/rule/windowRule/WindowRule.hpp"
#include "desktop/state/FadingOutState.hpp"
#include "desktop/state/LayerState.hpp"
#include "desktop/view/LayerSurface.hpp"
#include "desktop/view/Popup.hpp"
#include "desktop/view/WLSurface.hpp"
#include "desktop/state/WindowState.hpp"
// main (0.57-dev) split the window into Window + WindowPresentation; release 0.56 still has a single class.
#if __has_include("desktop/view/window/Window.hpp")
#include "desktop/view/window/Window.hpp"
#include "desktop/view/window/WindowPresentation.hpp"
#define NSC_SPLIT_WINDOW 1
#else
#include "desktop/view/Window.hpp"
#endif
#include "event/EventBus.hpp"
#include "managers/eventLoop/EventLoopManager.hpp"
#include "managers/eventLoop/EventLoopTimer.hpp"
#include "managers/fullscreen/FullscreenController.hpp"
#include "output/Monitor.hpp"
#include "output/MonitorZoomController.hpp"
#include "plugins/PluginAPI.hpp"
#include "protocols/XDGShell.hpp"
#include "protocols/core/Compositor.hpp"
#include "protocols/types/Buffer.hpp"
#include "render/Framebuffer.hpp"
#include "render/Renderer.hpp"
// 0.57: the render state is passed around explicitly (see RenderCtx)
#if __has_include("render/Context.hpp")
#include "managers/SessionLockManager.hpp"
#include "pointer/PointerManager.hpp"
#include "render/Context.hpp"
#include "render/scene/MonitorScene.hpp"
#define NSC_RENDER_CONTEXT 1
#endif
#include "render/pass/ClearPassElement.hpp"
#include "render/pass/RectPassElement.hpp"
#include "render/pass/TexPassElement.hpp"

#include <hyprutils/utils/ScopeGuard.hpp>

#include <aquamarine/buffer/Buffer.hpp>

// The screencast frame's m_session is private, and upstream offers no public event
// for drawing into the screencast frame. Everything this header pulls in is already
// included above, so `private public` only affects its own classes.
#define private public
#include "managers/screenshare/ScreenshareManager.hpp"
#undef private

namespace {

    // Window API differences between release and main are confined to this block.
    // Members that moved between Hyprland versions. `requires` on a template parameter
    // lets the compiler pick whatever exists in the headers we build against.
    template <class W>
    std::string windowClass(const W& w) {
        if constexpr (requires { w->m_class; })
            return w->m_class; // 0.56
        else
            return std::string{w->metadata().appID()}; // main
    }

    // layers and popups
    template <class V>
    bool viewVisible(const V& v) {
        if constexpr (requires { v->visible(); })
            return v->visible(); // 0.56
        else
            return v->mapped() && v->acceptsInput() && v->alphaNonZero(); // main, as ScreenshareFrame does
    }

    template <class V>
    SP<Desktop::View::CPopup> popupHeadOf(const V& v) {
        if constexpr (requires { v->popupHead(); })
            return v->popupHead(); // main
        else
            return v->m_popupHead; // 0.56
    }

    // fully opaque: nothing below shows through it (alpha rules, transparent terminals...)
    template <class W>
    bool windowOpaque(const W& w) {
        if constexpr (requires { w->presentation().opaque(); })
            return w->presentation().opaque(); // main
        else
            return w->opaque(); // 0.56
    }

    template <class W>
    bool windowFloating(const W& w) {
        if constexpr (requires { w->m_isFloating; })
            return w->m_isFloating; // 0.56
        else
            return w->isFloating(); // main
    }

    template <class W>
    bool windowIsX11(const W& w) {
        if constexpr (requires { w->m_isX11; })
            return w->m_isX11; // 0.56
        else
            return w->backend().isX11(); // main
    }

    // top-left of the client's own geometry inside its surface (popups are relative to it)
    template <class W>
    std::optional<Vector2D> windowClientGeometryPos(const W& w) {
        if constexpr (requires { w->m_xdgSurface; }) {
            if (!w->m_xdgSurface)
                return std::nullopt;
            return w->m_xdgSurface->m_current.geometry.pos(); // 0.56
        } else
            return w->backend().geometry().box.pos(); // main
    }

    template <class W>
    bool windowMapped(const W& w) {
        if constexpr (requires { w->mapped(); })
            return w->mapped(); // main
        else
            return w->m_isMapped; // 0.56
    }

    // the layout alpha is how a group hides its inactive tabs
    float windowFade(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
            w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
#else
        return w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
            w->alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
#endif
    }

    // everything that makes the window translucent as a whole: fade, fullscreen, opacity rules
    float windowAlpha(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return windowFade(w) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
#else
        return windowFade(w) * w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
#endif
    }

    bool windowPinned(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
#else
        return w->m_pinned;
#endif
    }

    float windowRounding(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().rounding();
#else
        return w->rounding();
#endif
    }

    float windowRoundingPower(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().roundingPower();
#else
        return w->roundingPower();
#endif
    }

    // The render state of the frame being drawn. 0.57 hands it to the screencast frame,
    // the zoom transform and every draw call; 0.56 keeps it in the renderer, so there
    // it's an empty tag and the helpers below go to g_pHyprRenderer.
#ifdef NSC_RENDER_CONTEXT
    using RenderCtx = Render::CRenderContext;
#else
    struct RenderCtx {};
#endif

    Render::SRenderData& renderData(RenderCtx& ctx) {
#ifdef NSC_RENDER_CONTEXT
        return ctx.m_data;
#else
        return g_pHyprRenderer->m_renderData;
#endif
    }

    template <class T>
    void drawElement(RenderCtx& ctx, const T& data, const CRegion& damage) {
#ifdef NSC_RENDER_CONTEXT
        g_pHyprRenderer->draw(ctx, data, damage);
#else
        g_pHyprRenderer->draw(data, damage);
#endif
    }

    HANDLE         g_handle = nullptr;
    // push settings to the core right after every config reload, not on the first
    // screencast frame: the default cover gets warmed up before capture starts
    CHyprSignalListener g_onReload;
    // window/layer close: start the closing cover right when Hyprland unmaps it
    CHyprSignalListener g_onWindowClose;
    CHyprSignalListener g_onLayerClose;
    CHyprSignalListener g_onWindowOpen;
    CHyprSignalListener g_onWindowRules;
    CHyprSignalListener g_onLayerOpen;
    CHyprSignalListener g_onLayerRules;

    // renderMonitor may be hooked by another plugin (gloview takes it while
    // noshare-cover isn't loaded). In that case don't fail the load, keep retrying
    // the hook: gloview releases the function once it sees noshare-cover after a
    // config reload. The timer is ours and is removed in PLUGIN_EXIT.
    void*                  g_hookTarget = nullptr;
    SP<CEventLoopTimer>    g_hookRetry;
    constexpr auto         HOOK_RETRY_EVERY = std::chrono::milliseconds(500);

    // Hyprland renders a capture frame only when the monitor repaints, and it
    // repaints only on damage. A video cover doesn't damage anything by itself,
    // so on a static monitor (e.g. while you work on another one) the stream
    // would freeze. While the monitor is being captured and has a live animation,
    // we damage the cover areas ~60 times per second.
    // A stream waiting for its next frame counts as capturing (see beingCaptured),
    // not only a recent frame: otherwise a pause in frames would stop the pump and
    // nothing could wake it up again.
    struct SAnimBox {
        PHLMONITORREF mon;
        CBox          box; // global logical coordinates
    };
    std::vector<SAnimBox> g_animBoxes;
    SP<CEventLoopTimer>   g_pump;
    constexpr auto        PUMP_EVERY = std::chrono::milliseconds(16);
    CFunctionHook* g_hook   = nullptr;

    SP<Config::Values::CStringValue> g_cfgPath;
    SP<Config::Values::CBoolValue>   g_cfgLoop;
    SP<Config::Values::CFloatValue>  g_cfgSpeed;
    SP<Config::Values::CStringValue> g_cfgBackend;
    SP<Config::Values::CStringValue> g_cfgGpu;
    SP<Config::Values::CIntValue>    g_cfgCloseHold;
    SP<Config::Values::CStringValue> g_cfgShowTo;
    SP<Config::Values::CStringValue> g_cfgHideFrom;
    SP<Config::Values::CBoolValue>   g_cfgCursorOverCovers;

    using EffectId = Desktop::Rule::CWindowRuleEffectContainer::storageType;

    // Rule effects. Upstream's Lua config passes only flat fields to plugins
    // (string/bool/number, tables are rejected), so the primary names are plain
    // Lua identifiers, no wrappers around hl.window_rule:
    //   hl.window_rule({ match = {...}, no_screen_share = true,
    //                    no_screen_share_cover = "~/x.mp4", no_screen_share_cover_speed = 1.5 })
    // Colon names come from the original plugin, so old configs keep working.
    enum eField : uint8_t { FIELD_PATH, FIELD_SPEED, FIELD_LOOP, FIELD_HOLD, FIELD_SHOW_TO, FIELD_HIDE_FROM };
    struct SEffect {
        const char* name;
        eField      field;
        EffectId    id = 0;
    };
    std::array<SEffect, 9> g_effects = {{
        {"no_screen_share_cover", FIELD_PATH},
        {"no_screen_share_cover_speed", FIELD_SPEED},
        {"no_screen_share_cover_loop", FIELD_LOOP},
        {"no_screen_share_cover_hold", FIELD_HOLD},
        {"no_screen_share_show_to", FIELD_SHOW_TO},
        {"no_screen_share_hide_from", FIELD_HIDE_FROM},
        {"no_screen_share_cover:path_cover", FIELD_PATH},
        {"no_screen_share_cover:speed", FIELD_SPEED},
        {"no_screen_share_cover:loop", FIELD_LOOP},
    }};
    // Same fields for layer rules (bars, launchers and other layer-shell surfaces):
    //   hl.layer_rule({ match = { namespace = "waybar" }, no_screen_share = true, no_screen_share_cover = "~/x.png" })
    std::array<SEffect, 9> g_layerEffects = {{
        {"no_screen_share_cover", FIELD_PATH},
        {"no_screen_share_cover_speed", FIELD_SPEED},
        {"no_screen_share_cover_loop", FIELD_LOOP},
        {"no_screen_share_cover_hold", FIELD_HOLD},
        {"no_screen_share_show_to", FIELD_SHOW_TO},
        {"no_screen_share_hide_from", FIELD_HIDE_FROM},
        {"no_screen_share_cover:path_cover", FIELD_PATH},
        {"no_screen_share_cover:speed", FIELD_SPEED},
        {"no_screen_share_cover:loop", FIELD_LOOP},
    }};

    // Texture cache keyed by cover id. Cleared entirely when the core epoch changes.
    struct SCachedTexture {
        SP<Render::ITexture> tex;
        uint64_t             generation = 0;
        uint32_t             w = 0, h = 0;
        bool                 dmabuf = false;
    };
    std::unordered_map<uint64_t, SCachedTexture> g_textures;
    uint64_t                                     g_epoch      = 0;
    uint64_t                                     g_frameCount = 0;

    void notify(const std::string& msg, float time = 4000) {
        if (g_handle)
            HyprlandAPI::addNotification(g_handle, msg, CHyprColor{1.F, 0.2F, 0.2F, 1.F}, time);
    }

    // NOSHARE_COVER_DEBUG=<file>: step-by-step trace (for debugging on someone else's machine)
    FILE* const g_trace = [] {
        const char* v = std::getenv("NOSHARE_COVER_DEBUG");
        return v && *v ? std::fopen(v, "a") : nullptr;
    }();
#define NSC_TRACE(...) \
    do { \
        if (g_trace) { \
            std::fprintf(g_trace, "[noshare-cover] " __VA_ARGS__); \
            std::fflush(g_trace); \
        } \
    } while (0)

    void drainNotifications() {
        std::array<char, 512> buf{};
        while (nsc_take_notification(buf.data(), buf.size()) > 0)
            notify(buf.data());
    }

    // What stands in for a hidden surface: no_screen_share_cover (or path_cover) is media, or
    // one of the words "transparent" (left out of the capture as if it wasn't there, see
    // CSceneSwap), "blur [size]" or "pixelate [block size]" (its own picture, blurred or
    // pixelated, see drawEffect; sizes in capture pixels).
    struct SCoverKind {
        enum eKind : uint8_t { MEDIA, TRANSPARENT, BLUR, PIXELATE };
        eKind kind   = MEDIA;
        int   amount = 0;
    };

    SCoverKind coverKind(std::string_view value) {
        const auto trim = [](std::string_view v) {
            const auto start = v.find_first_not_of(" \t");
            return start == std::string_view::npos ? std::string_view{} : v.substr(start, v.find_last_not_of(" \t") - start + 1);
        };
        value                 = trim(value);
        const auto       end  = std::min(value.find_first_of(" \t"), value.size());
        const auto       rest = trim(value.substr(end));
        std::string word{value.substr(0, end)};
        std::ranges::transform(word, word.begin(), [](unsigned char c) { return std::tolower(c); });

        constexpr std::array<std::tuple<std::string_view, SCoverKind::eKind, int>, 3> WORDS = {{
            {"transparent", SCoverKind::TRANSPARENT, 0},
            {"blur", SCoverKind::BLUR, 24},
            {"pixelate", SCoverKind::PIXELATE, 20},
        }};
        for (const auto& [name, kind, def] : WORDS) {
            if (word != name)
                continue;
            if (rest.empty())
                return {kind, def};
            int n = 0;
            if (kind != SCoverKind::TRANSPARENT && std::from_chars(rest.data(), rest.data() + rest.size(), n) == std::from_chars_result{rest.data() + rest.size(), std::errc{}})
                return {kind, std::clamp(n, 2, 512)};
        }
        return {}; // a file, even if it happens to be called "blur x"
    }

    void pushSettings() {
        std::string path = g_cfgPath ? g_cfgPath->value() : std::string{};
        if (coverKind(path).kind != SCoverKind::MEDIA)
            path.clear(); // the core never gets the words: such surfaces are never resolved
        const std::string backend = g_cfgBackend ? g_cfgBackend->value() : std::string{"auto"};
        const std::string gpu     = g_cfgGpu ? g_cfgGpu->value() : std::string{};

        const nsc_settings s{
            .path_cover = path.c_str(),
            .loop       = g_cfgLoop ? g_cfgLoop->value() : true,
            .speed      = g_cfgSpeed ? static_cast<double>(g_cfgSpeed->value()) : 1.0,
            .backend    = backend.c_str(),
            .gpu_device = gpu.c_str(),
        };
        nsc_set_settings(&s); // the core diffs against the previous settings and resets covers only on change

        if (const auto epoch = nsc_epoch(); epoch != g_epoch) {
            g_textures.clear();
            g_epoch = epoch;
        }
    }

    // Core frame -> Hyprland texture. A new texture only when the frame changed.
    SP<Render::ITexture> textureFor(const nsc_frame& f) {
        auto& c = g_textures[f.cover_id];
        if (c.tex && c.generation == f.generation)
            return c.tex;

        if (f.kind == NSC_FRAME_CPU && f.pixels) {
            auto* px = const_cast<uint8_t*>(f.pixels);
            if (c.tex && !c.dmabuf && c.w == f.width && c.h == f.height && c.tex->ok()) {
                const CRegion damage{0.0, 0.0, double(f.width), double(f.height)};
                c.tex->update(f.fourcc, px, f.stride, damage);
            } else {
                c.tex = g_pHyprRenderer->createTexture(f.fourcc, px, f.stride, Vector2D{double(f.width), double(f.height)});
            }
            c.dmabuf = false;
        } else if (f.kind == NSC_FRAME_DMABUF && f.plane_count > 0 && f.plane_count <= 4) {
            Aquamarine::SDMABUFAttrs attrs;
            attrs.success  = true;
            attrs.size     = Vector2D{double(f.width), double(f.height)};
            attrs.format   = f.fourcc;
            attrs.modifier = f.modifier;
            attrs.planes   = int(f.plane_count);
            for (uint32_t i = 0; i < f.plane_count; ++i) {
                attrs.fds[i]     = f.planes[i].fd;
                attrs.offsets[i] = f.planes[i].offset;
                attrs.strides[i] = f.planes[i].stride;
            }
            c.tex    = g_pHyprRenderer->createTexture(attrs);
            c.dmabuf = true;
        } else {
            return nullptr;
        }

        c.w          = f.width;
        c.h          = f.height;
        c.generation = f.generation;
        return (c.tex && c.tex->ok()) ? c.tex : nullptr;
    }

    // The last matching rule wins, same as in Hyprland itself.
    struct SRuleValues {
        std::optional<std::string> path, speed, loop, hold;
        std::optional<std::string> showTo, hideFrom; // per-rule capture client lists, override the global ones
    };

    SCoverKind coverKind(const SRuleValues& rules) {
        return coverKind(rules.path ? *rules.path : g_cfgPath ? g_cfgPath->value() : std::string{});
    }

    bool transparentCover(const SRuleValues& rules) {
        return coverKind(rules).kind == SCoverKind::TRANSPARENT;
    }

    // Before 0.57 (no render context, no capture scenes) the words only get the black box.
    void kindUnsupported() {
#ifndef NSC_RENDER_CONTEXT
        static bool told = false;
        if (!std::exchange(told, true))
            notify("noshare-cover: \"transparent\", \"blur\" and \"pixelate\" covers need Hyprland 0.57 or newer, the surface gets the black box", 10000);
#endif
    }

    // Cursor zoom (cursor:zoom_factor). The screencast copies the monitor image after
    // the zoom, but Hyprland's no_screen_share boxes (and, before this, our covers) use
    // unzoomed coordinates, so under zoom they miss the windows. Hyprland computes the
    // zoom in CMonitorZoomController::applyZoomTransform right before it saves the image
    // for the screencast; we record the result per monitor and map every box through it.
    struct SZoom {
        CBox     box;  // the zoomed monitor image, in monitor pixels
        Vector2D full; // unzoomed monitor size, in pixels
    };
    std::unordered_map<MONITORID, SZoom> g_zoom;
    CFunctionHook*                       g_zoomHook = nullptr;

    void recordZoom(const PHLMONITOR& mon, const CBox& monbox) {
        const auto full = mon->m_transformedSize;
        if (std::abs(monbox.x) < 0.5 && std::abs(monbox.y) < 0.5 && std::abs(monbox.w - full.x) < 0.5 && std::abs(monbox.h - full.y) < 0.5)
            g_zoom.erase(mon->m_id);
        else {
            const auto it = g_zoom.find(mon->m_id);
            if (it == g_zoom.end() || it->second.box != monbox)
                NSC_TRACE("zoom %s: %.0f,%.0f %.0fx%.0f of %.0fx%.0f\n", mon->m_name.c_str(), monbox.x, monbox.y, monbox.w, monbox.h, full.x, full.y);
            g_zoom[mon->m_id] = SZoom{.box = monbox, .full = full};
        }
    }

    // Only the normal monitor render feeds the screencast.
#ifdef NSC_RENDER_CONTEXT
    using ApplyZoomFn = void (*)(Monitor::CMonitorZoomController*, Render::CRenderContext&, CBox&);

    void hkApplyZoom(Monitor::CMonitorZoomController* self, Render::CRenderContext& ctx, CBox& monbox) {
        reinterpret_cast<ApplyZoomFn>(g_zoomHook->m_original)(self, ctx, monbox);
        if (const auto mon = ctx.m_data.pMonitor.lock(); mon && ctx.m_mode == Render::RENDER_MODE_NORMAL)
            recordZoom(mon, monbox);
    }
#else
    using ApplyZoomFn = void (*)(Monitor::CMonitorZoomController*, CBox&, const Render::SRenderData&);

    // m_renderMode is protected in some releases; there we can't tell and take every call.
    template <class R>
    bool normalRender(const R* r) {
        if constexpr (requires { r->m_renderMode; })
            return r->m_renderMode == Render::RENDER_MODE_NORMAL;
        else
            return true;
    }

    void hkApplyZoom(Monitor::CMonitorZoomController* self, CBox& monbox, const Render::SRenderData& rd) {
        reinterpret_cast<ApplyZoomFn>(g_zoomHook->m_original)(self, monbox, rd);
        if (const auto mon = rd.pMonitor.lock(); mon && normalRender(g_pHyprRenderer.get()))
            recordZoom(mon, monbox);
    }
#endif

    // Unzoomed monitor pixel box -> where it ends up in the screencast image.
    CBox zoomed(const PHLMONITOR& mon, const CBox& b) {
        const auto it = g_zoom.find(mon->m_id);
        if (it == g_zoom.end() || it->second.full.x <= 0 || it->second.full.y <= 0)
            return b;
        const auto&  z  = it->second;
        const double sx = z.box.w / z.full.x;
        const double sy = z.box.h / z.full.y;
        return CBox{z.box.x + b.x * sx, z.box.y + b.y * sy, b.w * sx, b.h * sy};
    }

    double zoomScale(const PHLMONITOR& mon) {
        const auto it = g_zoom.find(mon->m_id);
        return it == g_zoom.end() || it->second.full.x <= 0 ? 1.0 : it->second.box.w / it->second.full.x;
    }

    // Hyprland's own black boxes land in the wrong place under zoom and are drawn over
    // windows stacked above the hidden one, and can't be erased once drawn. While the
    // original renderMonitor runs, no_screen_share is switched off on the highest
    // priority slot (restored exactly right after), and we draw every box ourselves:
    // covers, or plain black where no cover is set.
    struct SSuppressed {
        Desktop::Types::COverridableVar<bool>* var = nullptr;
        std::optional<bool>                    prev;
    };

    // keep: surfaces (by address) that stay hidden
    std::vector<SSuppressed> suppressNoScreenShare(const std::unordered_set<uintptr_t>& keep = {}) {
        std::vector<SSuppressed> out;
        const auto               off = [&](Desktop::Types::COverridableVar<bool>& var) {
            if (!var.valueOrDefault())
                return;
            std::optional<bool> prev;
            if (var.hasValue() && var.getPriority() == Desktop::Types::PRIORITY_SET_PROP)
                prev = var.value();
            var.set(false, Desktop::Types::PRIORITY_SET_PROP);
            out.push_back({&var, prev});
        };
        for (const auto& w : Desktop::windowState()->windows())
            if (w && w->m_ruleApplicator && !keep.contains(reinterpret_cast<uintptr_t>(w.get())))
                off(w->m_ruleApplicator->noScreenShare());
        for (const auto& l : Desktop::layerState()->layers())
            if (l && l->m_ruleApplicator && !keep.contains(reinterpret_cast<uintptr_t>(l.get())))
                off(l->m_ruleApplicator->noScreenShare());
        return out;
    }

    void restoreNoScreenShare(const std::vector<SSuppressed>& suppressed) {
        for (const auto& e : suppressed) {
            if (e.prev)
                e.var->set(*e.prev, Desktop::Types::PRIORITY_SET_PROP);
            else
                e.var->unset(Desktop::Types::PRIORITY_SET_PROP);
        }
    }

    // Everything drawn over Hyprland's image in this capture frame (capture pixels): covers,
    // effects, black boxes. A software cursor under it is drawn again (cursorOverCovers).
    CRegion g_painted;

    // clip: where drawing is allowed (the draw is scissored to it); empty = the whole box
    void drawBlack(RenderCtx& ctx, const CBox& box, int round = 0, float roundingPower = 2.F, const CRegion& clip = {}) {
        const CRegion area = clip.empty() ? CRegion{box} : clip;
        g_painted.add(area);
        drawElement(ctx, CRectPassElement::SRectData{.box = box, .color = CHyprColor{0.F, 0.F, 0.F, 1.F}, .round = round, .roundingPower = roundingPower}, area);
    }

#ifdef NSC_RENDER_CONTEXT
    // drawEffect's intermediate buffers by size, dropped after a while unused (pruneTextures)
    struct SFxBuffer {
        SP<Render::IFramebuffer> fb;
        uint64_t                 used = 0;
    };
    std::map<std::pair<int, int>, SFxBuffer> g_fxBuffers;

    SP<Render::IFramebuffer> fxBuffer(const Vector2D& size, const SP<Render::ITexture>& like) {
        auto& b = g_fxBuffers[{int(size.x), int(size.y)}];
        if (!b.fb)
            b.fb = g_pHyprRenderer->createFB("noshare-cover effect");
        if (!b.fb || !b.fb->alloc(int(size.x), int(size.y)))
            return nullptr;
        b.fb->setImageDescription(like->m_imageDescription);
        b.used = g_frameCount;
        return b.fb;
    }
#endif

    // Blur or pixelate what the frame shows at box, taken from the image Hyprland draws the
    // frame from (see renderMonitorWithCovers), and draw it back there, only within clip.
    // The picture is halved with linear filtering, which averages 2x2 pixels, until one
    // pixel stands for `amount`; pixelate scales that up with nearest neighbour (blocks),
    // blur goes back up through the same sizes with linear filtering (smooth).
    bool drawEffect(RenderCtx& ctx, const SP<Render::ITexture>& image, const SCoverKind& kind, const CBox& box, const Vector2D& capturePos, int round, float roundingPower,
                    const CRegion& clip) {
#ifdef NSC_RENDER_CONTEXT
        if (!image || box.w < 1 || box.h < 1)
            return false;
        const CBox     src = box.copy().translate(capturePos); // in the image
        const Vector2D target{std::max(1.0, std::round(src.w / kind.amount)), std::max(1.0, std::round(src.h / kind.amount))};
        // first to target * 2^k, the nearest such size to src, so that every halving after it
        // is exact: one pixel from 2x2
        const int             k = std::clamp(int(std::lround(std::log2(std::max(src.w / target.x, src.h / target.y)))), 0, 12);
        std::vector<Vector2D> steps;
        for (int i = k; i >= 0; --i)
            steps.push_back(target * double(1 << i));
        if (kind.kind == SCoverKind::BLUR)
            for (int i = 1; i <= k; ++i)
                steps.push_back(target * double(1 << i));

        SP<Render::ITexture> tex  = image;
        CBox                 from = src; // the part of tex that is wanted
        {
            const auto                          viewport = ctx.m_data.fbSize;
            const Hyprutils::Utils::CScopeGuard restoreViewport{[&] { g_pHyprRenderer->setViewport(0, 0, viewport.x, viewport.y); }};
            const auto                          state = ctx.saveDrawState();
            ctx.m_data.renderModif.enabled            = false;
            ctx.m_data.useNearestNeighbor             = false;
            for (const auto& size : steps) {
                const auto fb = fxBuffer(size, image);
                if (!fb)
                    return false;
                const auto    bound = g_pHyprRenderer->bindTempFB(ctx, fb);
                const CRegion all{CBox{{}, size}};
                ctx.m_data.fbSize = size;
                g_pHyprRenderer->setProjectionType(ctx, Render::RPT_EXPORT);
                g_pHyprRenderer->setViewport(0, 0, size.x, size.y);
                const double sx = size.x / from.w, sy = size.y / from.h;
                drawElement(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 1.F}}, all); // beyond the image (off screen)
                drawElement(ctx, CTexPassElement::SRenderData{.tex = tex, .box = {-from.x * sx, -from.y * sy, tex->m_size.x * sx, tex->m_size.y * sy}}, all);
                tex  = fb->getTexture();
                from = CBox{{}, size};
            }
        }
        const bool nearest            = std::exchange(ctx.m_data.useNearestNeighbor, kind.kind == SCoverKind::PIXELATE);
        const Hyprutils::Utils::CScopeGuard restoreFilter{[&] { ctx.m_data.useNearestNeighbor = nearest; }};
        drawElement(ctx, CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round, .roundingPower = roundingPower}, clip);
        return true;
#else
        return false;
#endif
    }

    // One hidden surface's cover at box (capture pixels), only within clip (empty: the whole
    // box): its media, its own picture blurred or pixelated, or black when there is nothing
    // to show. what: the surface, for the trace.
    void paintCover(RenderCtx& ctx, const SP<Render::ITexture>& image, const std::string& what, const SRuleValues& rules, const CBox& box, const Vector2D& capturePos, int round,
                    float roundingPower, const CRegion& clip = {}) {
        const auto    kind = coverKind(rules);
        const CRegion area = clip.empty() ? CRegion{box} : clip;
        switch (kind.kind) {
            case SCoverKind::MEDIA: {
                const nsc_play_request req{
                    .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                    .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                    .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
                };
                nsc_frame f{};
                if (const auto tex = nsc_resolve(&req, &f) ? textureFor(f) : nullptr) {
                    NSC_TRACE("%s: cover %ux%u at %.0f,%.0f %.0fx%.0f\n", what.c_str(), f.width, f.height, box.x, box.y, box.w, box.h);
                    g_painted.add(area);
                    drawElement(ctx, CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round, .roundingPower = roundingPower}, area);
                    return;
                }
                NSC_TRACE("%s: no cover frame yet\n", what.c_str());
                break;
            }
            case SCoverKind::BLUR:
            case SCoverKind::PIXELATE:
                if (drawEffect(ctx, image, kind, box, capturePos, round, roundingPower, area)) {
                    NSC_TRACE("%s: %s %d at %.0f,%.0f %.0fx%.0f\n", what.c_str(), kind.kind == SCoverKind::BLUR ? "blur" : "pixelate", kind.amount, box.x, box.y, box.w, box.h);
                    g_painted.add(area);
                    return;
                }
                kindUnsupported();
                break;
            case SCoverKind::TRANSPARENT: kindUnsupported(); break; // and still in the image
        }
        drawBlack(ctx, box, round, roundingPower, area);
    }

    // Popups (menus, tooltips) of a hidden window or layer: blurred or pixelated like it,
    // otherwise black like Hyprland does.
    template <class V>
    void paintPopupBoxes(RenderCtx& ctx, const V& view, const Vector2D& baseOffset, const PHLMONITOR& mon, const Vector2D& capturePos, const SP<Render::ITexture>& image,
                         const SCoverKind& kind) {
        const auto head = popupHeadOf(view);
        if (!head)
            return;
        const bool effect = kind.kind == SCoverKind::BLUR || kind.kind == SCoverKind::PIXELATE;
        head->breadthfirst(
            [&](SP<Desktop::View::CPopup> popup, void*) {
                if (!popup || !popup->wlSurface() || !popup->wlSurface()->resource() || !viewVisible(popup))
                    return;
                const auto rel = popup->coordsRelativeToParent();
                popup->wlSurface()->resource()->breadthfirst(
                    [&](SP<CWLSurfaceResource> surf, const Vector2D& local, void*) {
                        const auto box = zoomed(mon, CBox{baseOffset + rel + local, surf->m_current.size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
                        if (box.w < 1 || box.h < 1)
                            return;
                        if (effect && drawEffect(ctx, image, kind, box, capturePos, 0, 2.F, CRegion{box}))
                            g_painted.add(box);
                        else
                            drawBlack(ctx, box);
                    },
                    nullptr);
            },
            nullptr);
    }

    // Closing windows and layers. Hyprland replaces a closed window or layer with a
    // snapshot fade-out (Desktop::fadingOutState()), and no_screen_share doesn't apply
    // to it, so the content shows up in the stream for the whole close animation. We
    // remember everything covered in the last frame; when something disappears, the
    // cover follows its fade-out until the animation ends, then stays for
    // close_hold ms (or the rule's no_screen_share_cover_hold).
    struct SLastCover {
        PHLWINDOWREF                          win;   // set for windows
        PHLLSREF                              layer; // set for layers
        bool                                  isLayer = false;
        PHLMONITORREF                         mon;
        CBox                                  box; // global logical coordinates
        SRuleValues                           rules;
        float                                 rounding      = 0.F;
        float                                 roundingPower = 2.F;
        std::chrono::steady_clock::time_point at; // frame it was last covered in
    };
    struct SClosing {
        bool                                                 isLayer = false;
        PHLMONITORREF                                        mon;
        CBox                                                 box; // last known, global logical
        SRuleValues                                          rules;
        float                                                rounding      = 0.F;
        float                                                roundingPower = 2.F;
        WP<Desktop::IFadeout>                                fade;
        std::chrono::steady_clock::time_point                bindUntil; // give up looking for the fade-out after this
        std::optional<std::chrono::steady_clock::time_point> holdUntil; // set once the fade-out is over
    };
    // Who a capture client is: its executable, or only the name from /proc/<pid>/comm
    // when the executable can't be read.
    struct SClientId {
        std::string path; // full path of the executable, empty for comm
        std::string name; // basename of path, or comm (first 15 chars, the process sets it itself)
        bool        fromComm = false;
    };

    // The client behind one capture frame, for show_to / hide_from.
    struct SCaptureClient {
        Screenshare::CScreenshareFrame*  frame = nullptr;
        std::string                      globalShow, globalHide;
        mutable std::optional<SClientId> id; // looked up only when some list applies

        // whether it sees a hidden surface with these rules as it is
        bool sees(const SRuleValues& rules) const;
    };

    std::unordered_map<uintptr_t, SLastCover> g_lastCovers;
    // Surfaces kept covered for a moment after they stopped being hidden (see holdUnhidden),
    // with the rules they had while hidden. Valid only during one capture frame.
    std::unordered_set<uintptr_t>              g_held;
    std::unordered_map<uintptr_t, SRuleValues> g_heldRules;
    std::vector<SClosing>                     g_closing;
    constexpr auto                            FADE_BIND_WINDOW = std::chrono::milliseconds(150);
    // Something last covered longer ago than this gets a closing cover only if its
    // fade-out is found (see trackClosed).
    constexpr auto STALE_COVER = std::chrono::seconds(1);

    // The last matching rule wins, as in Hyprland itself.
    template <class RuleT, class TargetT, size_t N>
    SRuleValues collectRuleValues(const TargetT& target, Desktop::Rule::eRuleType type, const std::array<SEffect, N>& ids) {
        SRuleValues out;
        const auto& engine = Desktop::Rule::ruleEngine();
        if (!engine)
            return out;
        for (const auto& rule : engine->rules()) {
            if (!rule || rule->type() != type)
                continue;
            const auto typed = dynamicPointerCast<RuleT>(rule);
            if (!typed || !typed->matches(target))
                continue;
            for (const auto& effect : typed->effects()) {
                if (effect.raw.empty())
                    continue;
                for (const auto& fx : ids) {
                    if (!fx.id || effect.key != fx.id)
                        continue;
                    switch (fx.field) {
                        case FIELD_PATH: out.path = effect.raw; break;
                        case FIELD_SPEED: out.speed = effect.raw; break;
                        case FIELD_LOOP: out.loop = effect.raw; break;
                        case FIELD_HOLD: out.hold = effect.raw; break;
                        case FIELD_SHOW_TO: out.showTo = effect.raw; break;
                        case FIELD_HIDE_FROM: out.hideFrom = effect.raw; break;
                    }
                }
            }
        }
        return out;
    }

    SRuleValues ruleValuesFor(const PHLWINDOW& w) {
        if (const auto it = g_heldRules.find(reinterpret_cast<uintptr_t>(w.get())); it != g_heldRules.end())
            return it->second;
        return collectRuleValues<Desktop::Rule::CWindowRule>(w, Desktop::Rule::RULE_TYPE_WINDOW, g_effects);
    }

    SRuleValues ruleValuesFor(const PHLLS& l) {
        if (const auto it = g_heldRules.find(reinterpret_cast<uintptr_t>(l.get())); it != g_heldRules.end())
            return it->second;
        return collectRuleValues<Desktop::Rule::CLayerRule>(l, Desktop::Rule::RULE_TYPE_LAYER, g_layerEffects);
    }

    // Window cover for another plugin's rect: same rules as for the window itself.
    SP<Render::ITexture> coverForWindowAddress(uint64_t address) {
        if (!address)
            return nullptr;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || reinterpret_cast<uintptr_t>(w.get()) != address)
                continue;
            const auto rules = ruleValuesFor(w);
            if (coverKind(rules).kind != SCoverKind::MEDIA)
                return nullptr; // another plugin's picture of it can only be blacked out
            const nsc_play_request req{
                .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
            };
            nsc_frame f{};
            return nsc_resolve(&req, &f) ? textureFor(f) : nullptr;
        }
        return nullptr;
    }

    std::chrono::milliseconds holdFor(const SRuleValues& rules) {
        long long ms = g_cfgCloseHold ? g_cfgCloseHold->value() : 0;
        if (rules.hold) {
            try {
                ms = std::stoll(*rules.hold);
            } catch (...) {}
        }
        return std::chrono::milliseconds(std::clamp<long long>(ms, 0, 60000));
    }

    bool isWindowFade(const SP<Desktop::IFadeout>& f) {
        const auto plane = f->plane();
        return plane == Desktop::FADEOUT_PLANE_WINDOW_TILED || plane == Desktop::FADEOUT_PLANE_WINDOW_FLOATING || plane == Desktop::FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN;
    }

    bool isLayerFade(const SP<Desktop::IFadeout>& f) {
        const auto plane = f->plane();
        return plane == Desktop::FADEOUT_PLANE_LAYER_BACKGROUND || plane == Desktop::FADEOUT_PLANE_LAYER_BOTTOM || plane == Desktop::FADEOUT_PLANE_LAYER_TOP ||
            plane == Desktop::FADEOUT_PLANE_LAYER_OVERLAY;
    }

    // The fade-out Hyprland created for a closed window or layer: one of the same kind
    // on the same monitor, not taken by another closing cover, nearest to where it was.
    // The match is fuzzy: IFadeout doesn't keep its source, so this goes by plane and
    // geometry only and can pick the fade-out of something else that closed nearby
    // (e.g. for a layer that went invisible without layer.closed), and then the cover
    // follows the wrong fade-out.
    SP<Desktop::IFadeout> findFadeFor(const SClosing& c, const PHLMONITOR& mon) {
        const auto& state = Desktop::fadingOutState();
        if (!state)
            return nullptr;
        SP<Desktop::IFadeout> best;
        double                bestDist = std::max(c.box.w, c.box.h);
        const auto            center   = c.box.middle();
        for (const auto& f : state->fadeouts()) {
            if (!f || f->done() || !(c.isLayer ? isLayerFade(f) : isWindowFade(f)) || f->monitor().lock() != mon)
                continue;
            if (std::ranges::any_of(g_closing, [&](const SClosing& o) { return o.fade.lock() == f; }))
                continue;
            const double d = f->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT).middle().distance(center);
            if (d <= bestDist) {
                best     = f;
                bestDist = d;
            }
        }
        return best;
    }

    // Covered windows and layers of this monitor that disappeared since the last frame:
    // closed ones get a closing cover, moved or hidden ones are just forgotten.
    void trackClosed(const PHLMONITOR& mon, const std::unordered_set<uintptr_t>& seen, std::chrono::steady_clock::time_point now) {
        for (auto it = g_lastCovers.begin(); it != g_lastCovers.end();) {
            const auto& last = it->second;
            const auto  m    = last.mon.lock();
            if (m && m != mon) {
                ++it;
                continue;
            }
            if (m && seen.contains(it->first)) {
                ++it;
                continue;
            }
            bool gone = false;
            if (last.isLayer) {
                const auto l = last.layer.lock();
                gone         = !l || !viewVisible(l);
            } else {
                const auto w = last.win.lock();
                gone         = !w || !windowMapped(w);
            }
            if (m && gone) {
                SClosing c{
                    .isLayer       = last.isLayer,
                    .mon           = mon,
                    .box           = last.box,
                    .rules         = last.rules,
                    .rounding      = last.rounding,
                    .roundingPower = last.roundingPower,
                    .bindUntil     = now + FADE_BIND_WINDOW,
                };
                // The stream may have had no frames for a while (static screen), so an old
                // entry can still be something closed just now. Keep it only if its fade-out
                // is running; otherwise it closed long ago and there's nothing to hide.
                const bool fresh = now - last.at < STALE_COVER;
                if (!fresh)
                    c.fade = findFadeFor(c, mon);
                if (fresh || c.fade.lock()) {
                    g_closing.push_back(std::move(c));
                    NSC_TRACE("%s closed: cover kept at %.0f,%.0f %.0fx%.0f\n", last.isLayer ? "layer" : "window", last.box.x, last.box.y, last.box.w, last.box.h);
                }
            }
            it = g_lastCovers.erase(it);
        }
    }

    void startPump();

    // Whether a client captures mon right now: a frame went out in the last half second
    // (m_sharing), or one waits for the next commit. Hyprland's isOutputBeingSSd only asks
    // whether a session exists, and a wlr-screencopy session (xdg-desktop-portal-hyprland,
    // wf-recorder) is kept until the monitor goes away, even after its client has gone: after
    // the first portal stream it stayed true until logout, and animated covers on screen got
    // the monitor repainted 60 times a second with nothing capturing it.
    bool beingCaptured(const PHLMONITOR& mon) {
        const auto& mgr = Screenshare::mgr();
        if (!mon || !mgr)
            return false;
        const auto ofMonitor = [&](const Screenshare::CScreenshareSession* s) {
            return s && !s->m_stopped && (s->m_type == Screenshare::SHARE_MONITOR || s->m_type == Screenshare::SHARE_REGION) && s->m_monitor == mon;
        };
        // sessions and frames are owned by unique pointers: their weak pointers are only
        // checked and read, never locked (hyprutils asserts)
        return std::ranges::any_of(mgr->m_sessions, [&](const auto& s) { return !s.expired() && s->m_sharing && ofMonitor(s.get()); }) ||
            std::ranges::any_of(mgr->m_pendingFrames, [&](const auto& f) { return !f.expired() && !f->done() && !f->m_session.expired() && ofMonitor(f->m_session.get()); });
    }

    // Hyprland emits window.close / layer.closed from unmap, while the window or layer
    // still has its geometry and rules and before its fade-out exists. Starting the
    // closing cover here doesn't depend on the stream sending frames (a static screen
    // sends none, and the frame-based tracking above then only has old data).
    void startClosing(bool isLayer, uintptr_t key, const PHLMONITOR& mon, const CBox& box, SRuleValues&& rules, float rounding, float roundingPower) {
        if (!beingCaptured(mon))
            return;
        g_lastCovers.erase(key); // don't start it a second time from the next frame
        g_closing.push_back(SClosing{
            .isLayer       = isLayer,
            .mon           = mon,
            .box           = box,
            .rules         = std::move(rules),
            .rounding      = rounding,
            .roundingPower = roundingPower,
            .bindUntil     = std::chrono::steady_clock::now() + FADE_BIND_WINDOW,
        });
        NSC_TRACE("%s close event: cover kept at %.0f,%.0f %.0fx%.0f\n", isLayer ? "layer" : "window", box.x, box.y, box.w, box.h);
        startPump();
    }

    void onWindowClose(const PHLWINDOW& w) {
        if (!w || !w->m_ruleApplicator || !w->m_ruleApplicator->noScreenShare().valueOrDefault())
            return;
        const auto* ws           = w->m_workspace.get();
        const auto  renderOffset = ws && !windowPinned(w) && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        const auto  pos          = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        const auto  size         = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        startClosing(false, reinterpret_cast<uintptr_t>(w.get()), w->m_monitor.lock(), CBox{pos, size}, ruleValuesFor(w), windowRounding(w), windowRoundingPower(w));
    }

    // Covers are opened on a loader thread, so the first capture of a new cover
    // would only get the black box: a screenshot has no later frame to catch up
    // in. Start opening as soon as a hidden window or layer gets its rules.
    void prewarm(const SRuleValues& rules) {
        if (coverKind(rules).kind != SCoverKind::MEDIA)
            return;
        // settings normally arrive with the first capture frame; without them the
        // warmed cover would be dropped as soon as they do (settings change = reset)
        pushSettings();
        const nsc_play_request req{
            .rule_path  = rules.path ? rules.path->c_str() : nullptr,
            .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
            .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
        };
        nsc_prewarm(&req);
    }

    void prewarmWindow(const PHLWINDOW& w) {
        if (w && w->m_ruleApplicator && w->m_ruleApplicator->noScreenShare().valueOrDefault())
            prewarm(ruleValuesFor(w));
    }

    void prewarmLayer(const PHLLS& l) {
        if (l && l->m_ruleApplicator && l->m_ruleApplicator->noScreenShare().valueOrDefault())
            prewarm(ruleValuesFor(l));
    }

    void onLayerClose(const PHLLS& l) {
        if (!l || !l->m_ruleApplicator || !l->m_ruleApplicator->noScreenShare().valueOrDefault())
            return;
        const auto pos  = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto size = l->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        startClosing(true, reinterpret_cast<uintptr_t>(l.get()), l->m_monitor.lock(), CBox{pos, size}, ruleValuesFor(l), 0.F, 2.F);
    }

    // Returns the area it drew over (capture pixels). Clients that may see the surface
    // (show_to / hide_from) get no closing cover: they see its fade-out as it is.
    // left: transparent surfaces were left out of this frame, their fade-outs too.
    // image: what Hyprland draws the frame from, for blur and pixelate.
    CRegion paintClosing(RenderCtx& ctx, const PHLMONITOR& mon, const Vector2D& capturePos, std::chrono::steady_clock::time_point now, const SCaptureClient& client,
                         bool left, const SP<Render::ITexture>& image) {
        CRegion drawnArea;
        for (auto& c : g_closing) {
            if (c.mon.lock() != mon)
                continue;
            if (!c.holdUntil) {
                auto fade = c.fade.lock();
                if (!fade && now < c.bindUntil) {
                    fade   = findFadeFor(c, mon);
                    c.fade = fade;
                }
                if (fade && !fade->done())
                    c.box = fade->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
                else if (fade || now >= c.bindUntil)
                    // without a fade-out the hold starts when the search for one ends, right
                    // after the close, not at the next frame: with single shots (nothing
                    // renders in between) that can be seconds later. A fade-out that ended
                    // with no frame since it was found still counts from now: a late extra
                    // cover in that rare case, never a missing one.
                    c.holdUntil = (fade ? now : c.bindUntil) + holdFor(c.rules);
            }
            if (c.holdUntil && now >= *c.holdUntil)
                continue; // the pump drops it and repaints
            if (client.sees(c.rules))
                continue;
            if (left && transparentCover(c.rules))
                continue;

            const auto box =
                zoomed(mon, CBox{c.box.x, c.box.y, std::max(c.box.w, 5.0), std::max(c.box.h, 5.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
            if (box.w < 1 || box.h < 1)
                continue;
            const int round = capturePos != Vector2D{} ? 0 : int(std::lround(c.rounding * mon->m_scale * zoomScale(mon)));
            paintCover(ctx, image, c.isLayer ? "closing layer" : "closing window", c.rules, box, capturePos, round, c.roundingPower);
            drawnArea.add(box);
        }
        return drawnArea;
    }

    // The part of a surface's buffer the client shows (its wp_viewporter source), as texture
    // UVs, computed like Hyprland's calculateUVForSurface; nullopt for the whole buffer.
    std::optional<std::pair<Vector2D, Vector2D>> viewportUV(const SP<CWLSurfaceResource>& surf) {
        const auto& st = surf->m_current;
        if (!st.viewport.hasSource || st.bufferSize.x <= 0 || st.bufferSize.y <= 0)
            return std::nullopt;
        // viewporter order: buffer transform, then buffer scale, then the source crop
        const Vector2D size = st.transform % 2 == 1 ? Vector2D{st.bufferSize.y, st.bufferSize.x} : st.bufferSize;
        CBox           src  = st.viewport.source;
        src.scale(st.scale);
        src.transform(Math::wlTransformToHyprutils(Math::invertTransform(st.transform)), size.x, size.y);
        const Vector2D topLeft{src.x / st.bufferSize.x, src.y / st.bufferSize.y};
        const Vector2D bottomRight{(src.x + src.width) / st.bufferSize.x, (src.y + src.height) / st.bufferSize.y};
        if (bottomRight.x < 0.00001 || bottomRight.y < 0.00001)
            return std::nullopt;
        return std::pair{topLeft, bottomRight};
    }

    // Draw a client surface's texture into box (its logical size), cropped to what the
    // client shows. Chromium keeps an oversized buffer and shows a viewporter crop of it:
    // drawn whole, the page shrank into a corner of the window and blur filled the rest.
    void drawSurfaceTexture(RenderCtx& ctx, const SP<CWLSurfaceResource>& surf, CTexPassElement::SRenderData data, const CRegion& damage) {
        const auto uv = viewportUV(surf);
        if (!uv) {
            drawElement(ctx, data, damage);
            return;
        }
        auto&                         rd          = renderData(ctx);
        const auto                    topLeft     = rd.primarySurfaceUVTopLeft;
        const auto                    bottomRight = rd.primarySurfaceUVBottomRight;
        Hyprutils::Utils::CScopeGuard restore{[&] {
            rd.primarySurfaceUVTopLeft     = topLeft;
            rd.primarySurfaceUVBottomRight = bottomRight;
        }};
        rd.primarySurfaceUVTopLeft     = uv->first;
        rd.primarySurfaceUVBottomRight = uv->second;
        data.allowCustomUV             = true;
        drawElement(ctx, data, damage);
    }

    // Draw a layer again from its surface textures with its alpha, limited to clip.
    void redrawLayer(RenderCtx& ctx, const PHLLS& l, const CRegion& clip, const PHLMONITOR& mon, const Vector2D& capturePos) {
        const auto res = l->resource();
        if (!res)
            return;
        float alpha = 1.F;
        for (uint8_t k = 0; k < Desktop::View::LS_ALPHA_LAST; ++k)
            alpha *= l->alpha().get(k)->value();
        const auto base = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        res->breadthfirst(
            [&](SP<CWLSurfaceResource> surf, const Vector2D& local, void*) {
                const auto tex = surf->m_current.texture;
                if (!tex)
                    return;
                const auto box = zoomed(mon, CBox{base + local, surf->m_current.size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
                if (box.w >= 1 && box.h >= 1)
                    drawSurfaceTexture(ctx, surf, CTexPassElement::SRenderData{.tex = tex, .box = box, .a = alpha, .clipRegion = clip}, clip);
            },
            nullptr);
    }

    // Every couple of seconds, drop textures of covers the core has already closed, and
    // effect buffers that weren't used in that time.
    void pruneTextures() {
        if (++g_frameCount % 120 != 0)
            return;
        std::erase_if(g_textures, [](const auto& kv) { return !nsc_cover_alive(kv.first); });
#ifdef NSC_RENDER_CONTEXT
        std::erase_if(g_fxBuffers, [](const auto& kv) { return kv.second.used + 120 < g_frameCount; });
#endif
    }

    void paintExtraRects(RenderCtx& ctx, const PHLMONITOR& mon, const Vector2D& capturePos) {
        // Usually a few dozen overlay tiles; if there are more, take as many as reported.
        std::vector<nsc_extra_rect> rects(64);
        size_t                      n = nsc_extra_rects(mon->m_id, rects.data(), rects.size());
        if (n > rects.size()) {
            rects.resize(n);
            n = std::min(nsc_extra_rects(mon->m_id, rects.data(), rects.size()), rects.size());
        }

        for (size_t i = 0; i < n; ++i) {
            const auto& r     = rects[i];
            const auto  box   = zoomed(mon, CBox{r.x, r.y, std::max(r.w, 1.0), std::max(r.h, 1.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
            const int   round = int(std::lround(r.rounding * mon->m_scale * zoomScale(mon)));
            if (box.w < 1 || box.h < 1)
                continue;

            if (r.fill == 1) {
                if (const auto tex = coverForWindowAddress(r.window)) {
                    g_painted.add(box);
                    drawElement(ctx, CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round}, box);
                    continue;
                }
            }
            drawBlack(ctx, box, round);
        }
    }

    void pumpTick(SP<CEventLoopTimer> self, void*) {
        bool       any    = false;
        const auto shared = [](const PHLMONITORREF& ref) { return beingCaptured(ref.lock()); };
        if (g_pHyprRenderer) {
            if (nsc_animating()) {
                for (const auto& a : g_animBoxes) {
                    if (!shared(a.mon))
                        continue;
                    g_pHyprRenderer->damageBox(a.box);
                    any = true;
                }
            }
            // Closing covers: keep repainting while they're up, and once more after
            // they expire so the stream drops them.
            const auto now = std::chrono::steady_clock::now();
            for (const auto& c : g_closing) {
                if (!shared(c.mon))
                    continue;
                g_pHyprRenderer->damageBox(c.box);
                any = true;
            }
            std::erase_if(g_closing, [&](const SClosing& c) { return !shared(c.mon) || (c.holdUntil && now >= *c.holdUntil); });
        }
        if (!any) {
            NSC_TRACE("pump: idle, nothing to repaint or nothing captured\n");
            g_animBoxes.clear();
            g_closing.clear();
            self->updateTimeout(std::nullopt); // sharing stopped, go idle
            return;
        }
        self->updateTimeout(PUMP_EVERY);
    }

    void startPump() {
        if (!g_pEventLoopManager)
            return;
        if (!g_pump) {
            g_pump = makeShared<CEventLoopTimer>(PUMP_EVERY, pumpTick, nullptr);
            g_pEventLoopManager->addTimer(g_pump);
        } else if (!g_pump->armed())
            g_pump->updateTimeout(PUMP_EVERY);
    }

    void stopPump() {
        if (!g_pump)
            return;
        g_pump->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g_pump);
        g_pump.reset();
    }

    // A window as it lands in the capture image.
    struct SWinGeo {
        Vector2D pos, size; // global logical, pos includes the workspace render offset
        CBox     box;       // capture pixels
        int      round         = 0;
        float    roundingPower = 2.F;
        int      layer         = 0; // stacking, see windowGeo
        bool     opaque        = false;
    };

    std::optional<SWinGeo> windowGeo(const PHLWINDOW& w, const PHLMONITOR& mon, const Vector2D& capturePos) {
        if (!g_pHyprRenderer->shouldRenderWindow(w, mon) || w->isHidden())
            return std::nullopt;
        const auto  fade = windowFade(w);
        const auto* ws   = w->m_workspace.get();
        // nothing on screen: no workspace, or faded out behind a fullscreen or maximized
        // window (shouldRenderWindow still lets those through while the workspace slides in)
        if (!ws || fade == 0.F)
            return std::nullopt;

        SWinGeo    g;
        const auto renderOffset = ws && !windowPinned(w) && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        g.size                  = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        g.pos                   = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        g.box = zoomed(mon, CBox{g.pos.x, g.pos.y, std::max(g.size.x, 5.0), std::max(g.size.y, 5.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
        if (g.box.w < 1 || g.box.h < 1)
            return std::nullopt;
        const auto& fsc        = Fullscreen::controller();
        const bool  fullscreen = fsc && fsc->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
        const bool  dontRound  = capturePos != Vector2D{} || fullscreen;
        g.round                = dontRound ? 0 : int(std::lround(windowRounding(w) * mon->m_scale * zoomScale(mon)));
        g.roundingPower        = dontRound ? 2.F : windowRoundingPower(w);
        // Hyprland's order (renderWorkspaceWindows[Fullscreen]): tiled, floating, and on a
        // workspace with a fullscreen window that window over both, with the floating
        // windows that go over it (allowed there, or fading out under it) on top. The special
        // workspace the same way over the regular one, pinned windows over everything.
        const bool fsWorkspace = fsc && fsc->hasFullscreen(w->m_workspace);
        int        stack       = windowFloating(w) ? 1 : 0;
        if (fsWorkspace && fsc->isFullscreen(w))
            stack = 2;
        else if (fsWorkspace && stack == 1 && w->shouldRenderOverFullscreen())
            stack = 3;
        g.layer = windowPinned(w) ? 8 : (w->onSpecialWorkspace() ? 4 : 0) + stack;
        // Hyprland's opaque() ignores the layout alpha: a group tab or monocle window that
        // is fading out on a switch would still count as opaque and cut the cover below it
        g.opaque              = fade >= 1.F && windowOpaque(w);
        return g;
    }

    // Take an opaque box out of a region, but keep its r x r corners: a rounded window
    // is transparent there, and whatever is below would show through.
    void subtractRounded(CRegion& region, const CBox& b, int r) {
        r = std::clamp(r, 0, int(std::min(b.w, b.h) / 2));
        region.subtract(CBox{b.x + r, b.y, b.w - 2 * r, b.h});
        region.subtract(CBox{b.x, b.y + r, b.w, b.h - 2 * r});
    }

    // Draw a window again from its surface textures, with its alpha, rounding and blur,
    // limited to clip. Used for translucent windows above a hidden one: the cover went
    // over them, and this puts them back on top of it.
    void redrawWindow(RenderCtx& ctx, const PHLWINDOW& w, const SWinGeo& geo, const CRegion& clip, const PHLMONITOR& mon, const Vector2D& capturePos) {
        const auto res = w->resource();
        if (!res)
            return;
        Vector2D base = geo.pos;
        if (!windowIsX11(w)) {
            const auto client = windowClientGeometryPos(w);
            if (!client)
                return;
            base -= *client;
        }
        static auto PBLUR = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
        const bool  blur  = *PBLUR && !(w->m_ruleApplicator && w->m_ruleApplicator->noBlur().valueOrDefault());
        const float alpha = windowAlpha(w);
        const float fade  = windowFade(w);
        res->breadthfirst(
            [&](SP<CWLSurfaceResource> surf, const Vector2D& local, void*) {
                const auto tex = surf->m_current.texture;
                if (!tex)
                    return;
                const bool main = surf == res;
                const auto box  = zoomed(mon, CBox{base + local, surf->m_current.size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
                if (box.w < 1 || box.h < 1)
                    return;
                drawSurfaceTexture(ctx, surf,
                                   CTexPassElement::SRenderData{
                                       .tex                   = tex,
                                       .box                   = box,
                                       .a                     = alpha,
                                       .blurA                 = fade, // the blur fades with the window, as in Hyprland
                                       .round                 = main ? geo.round : 0,
                                       .roundingPower         = geo.roundingPower,
                                       .blur                  = main && blur,
                                       .blockBlurOptimization = true, // blur what is really under it (the cover), not the cached wallpaper
                                       // the damage clip alone isn't enough: the blur pass draws the blurred
                                       // background over the whole box unless clipRegion is set
                                       .clipRegion = clip,
                                   },
                                   clip);
            },
            nullptr);
    }

    // Hyprland's black boxes are switched off for the frame (see suppressNoScreenShare), so
    // everything without a cover is drawn black here.
    // revealed: hidden surfaces this client sees as they are (see revealFor).
    // left: surfaces with a transparent cover were left out of this frame's image (see
    // CSceneSwap), so nothing goes over them; otherwise they get the black box.
    // image: what Hyprland draws the frame from, for blur and pixelate.
    void paintCovers(RenderCtx& ctx, Screenshare::CScreenshareFrame* frame, const SCaptureClient& client, const std::unordered_set<uintptr_t>& revealed, bool left,
                     const SP<Render::ITexture>& image) {
        if (!frame || !frame->m_session || !g_pHyprRenderer) {
            NSC_TRACE("skip: frame %p session %d renderer %d\n", static_cast<void*>(frame), frame && frame->m_session ? 1 : 0, g_pHyprRenderer ? 1 : 0);
            return;
        }
        const auto mon = renderData(ctx).pMonitor.lock();
        if (!mon) {
            NSC_TRACE("skip: no monitor in render data\n");
            return;
        }

        pushSettings();
        nsc_begin_frame();
        // rebuild this monitor's covers, leave other monitors alone
        std::erase_if(g_animBoxes, [&](const SAnimBox& a) { return a.mon.expired() || a.mon.lock() == mon; });
        NSC_TRACE("frame: monitor %s\n", mon->m_name.c_str());

        const auto capturePos = frame->m_session->m_captureBox.pos();
        const auto now        = std::chrono::steady_clock::now();

        // Windows drawn on top of others: later in Hyprland's stacking (see windowGeo), then
        // in list order.
        // A cover is only drawn where its window is actually visible, so an opaque window
        // placed over a hidden one stays visible. Layers (bars) are left alone: they are
        // often translucent and we can't tell.
        struct SVisible {
            PHLWINDOW                  w;
            SWinGeo                    geo;
            std::optional<SRuleValues> rules;        // set for hidden windows
            bool                       gone = false; // left out of the image, as if it wasn't there
        };
        std::vector<SVisible> visible;
        for (const auto& w : Desktop::windowState()->windows()) {
            const auto g = w ? windowGeo(w, mon, capturePos) : std::nullopt;
            if (!g)
                continue;
            auto& v = visible.emplace_back(SVisible{.w = w, .geo = *g});
            if (w->m_ruleApplicator && w->m_ruleApplicator->noScreenShare().valueOrDefault()) {
                v.rules = ruleValuesFor(w);
                v.gone  = left && transparentCover(*v.rules);
            }
        }
        // o is stacked above me
        const auto isAbove = [&](size_t o, size_t me) {
            const auto& a = visible[o].geo;
            const auto& b = visible[me].geo;
            return o != me && (a.layer > b.layer || (a.layer == b.layer && o > me));
        };
        const auto visibleRegion = [&](size_t i) {
            CRegion region{visible[i].geo.box};
            // Only opaque windows hide what is below them. Under a translucent one the
            // hidden window would show through, so the cover goes there too and the
            // translucent window is drawn again on top of it (see below).
            for (size_t j = 0; j < visible.size(); ++j)
                if (isAbove(j, i) && visible[j].geo.opaque && !visible[j].gone)
                    subtractRounded(region, visible[j].geo.box, visible[j].geo.round);
            return region;
        };
        std::vector<std::pair<size_t, CRegion>> drawn; // covers (or black) drawn, per window

        std::unordered_set<uintptr_t> seen;
        for (size_t i = 0; i < visible.size(); ++i) {
            if (!visible[i].rules)
                continue;
            const auto& w             = visible[i].w;
            const auto& rules         = *visible[i].rules;
            const auto& geo           = visible[i].geo;
            const auto  pos           = geo.pos;
            const auto  size          = geo.size;
            const auto  box           = geo.box;
            const int   round         = geo.round;
            const auto  roundingPower = geo.roundingPower;

            // kept for left out windows too: their fade-out on close and the hold
            const auto key = reinterpret_cast<uintptr_t>(w.get());
            seen.insert(key);
            if (!g_held.contains(key)) // a held cover keeps the time and rules it had
                g_lastCovers[key] = SLastCover{
                .win           = w,
                .mon           = mon,
                .box           = CBox{pos.x, pos.y, size.x, size.y},
                .rules         = rules,
                .rounding      = windowRounding(w),
                .roundingPower = windowRoundingPower(w),
                .at            = now,
            };
            if (visible[i].gone)
                continue;
            g_animBoxes.push_back({mon, CBox{pos.x, pos.y, size.x, size.y}});

            const CRegion clip = visibleRegion(i);
            if (clip.empty()) {
                NSC_TRACE("window %s: fully under other windows\n", windowClass(w).c_str());
            }
            if (!windowIsX11(w)) {
                if (const auto geo = windowClientGeometryPos(w))
                    paintPopupBoxes(ctx, w, pos - *geo, mon, capturePos, image, coverKind(rules));
            }
            if (clip.empty())
                continue;
            paintCover(ctx, image, "window " + windowClass(w), rules, box, capturePos, round, roundingPower, clip);
            drawn.emplace_back(i, clip);
        }

        // Translucent windows above a hidden one, bottom to top: draw them again over the
        // cover, so what shows through them (blurred, if they blur) is the cover and never
        // the hidden window. Only over covers below them, minus whatever is above them.
        if (!drawn.empty()) {
            std::vector<size_t> order(visible.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::ranges::stable_sort(order, {}, [&](size_t k) { return visible[k].geo.layer; });
            for (const size_t j : order) {
                // Never redraw a hidden window from its real content. This also catches
                // windows that are only translucent for a moment (fading in on open).
                const auto& wj = visible[j].w;
                if (visible[j].geo.opaque || !wj->m_ruleApplicator || wj->m_ruleApplicator->noScreenShare().valueOrDefault())
                    continue;
                CRegion region;
                for (const auto& [i, clip] : drawn)
                    if (isAbove(j, i))
                        region.add(clip);
                if (region.empty())
                    continue;
                region.intersect(CRegion{visible[j].geo.box});
                for (size_t k = 0; k < visible.size(); ++k)
                    if (isAbove(k, j) && visible[k].geo.opaque && !visible[k].gone)
                        subtractRounded(region, visible[k].geo.box, visible[k].geo.round);
                for (const auto& [i, clip] : drawn)
                    if (isAbove(i, j))
                        region.subtract(clip);
                if (region.empty())
                    continue;
                NSC_TRACE("window %s: redrawn over a cover\n", windowClass(visible[j].w).c_str());
                redrawWindow(ctx, visible[j].w, visible[j].geo, region, mon, capturePos);
            }
        }


        // Layers (layer-shell) with no_screen_share: same geometry as Hyprland's
        // black rect, no rounding.
        for (const auto& l : Desktop::layerState()->layers()) {
            if (!l || !l->m_ruleApplicator || !l->m_ruleApplicator->noScreenShare().valueOrDefault() || !viewVisible(l))
                continue;
            const auto pos  = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto size = l->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto box  = zoomed(mon, CBox{pos.x, pos.y, std::max(size.x, 5.0), std::max(size.y, 5.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
            if (box.w < 1 || box.h < 1)
                continue;
            const auto rules = ruleValuesFor(l);
            const auto key   = reinterpret_cast<uintptr_t>(l.get());
            seen.insert(key);
            if (!g_held.contains(key)) // a held cover keeps the time and rules it had
                g_lastCovers[key] = SLastCover{
                .layer   = l,
                .isLayer = true,
                .mon     = mon,
                .box     = CBox{pos.x, pos.y, size.x, size.y},
                .rules   = rules,
                .at      = now,
            };
            if (left && transparentCover(rules))
                continue;
            g_animBoxes.push_back({mon, CBox{pos.x, pos.y, size.x, size.y}});
            paintPopupBoxes(ctx, l, pos - l->m_geometry.pos(), mon, capturePos, image, coverKind(rules));
            paintCover(ctx, image, "layer " + l->m_namespace, rules, box, capturePos, 0, 2.F);
        }

        // Surfaces shown as they are to this client are still hidden and on screen: keep
        // their entries (and the time they were last covered) for the other clients, or a
        // screenshot between two stream frames would drop the hold of a window that stops
        // being hidden right after it.
        seen.insert(revealed.begin(), revealed.end());
        trackClosed(mon, seen, now);
        CRegion coverArea = paintClosing(ctx, mon, capturePos, now, client, left, image);

        // Layers above windows (bars, launchers, notifications) are drawn by Hyprland on
        // top of the hidden window, and the covers went over them. Draw them again on top,
        // only where a window cover was drawn, top then overlay. Hidden layers never.
        for (const auto& [i, clip] : drawn)
            coverArea.add(clip);
        if (!coverArea.empty()) {
            for (const uint32_t level : {2U, 3U}) {
                for (const auto& l : Desktop::layerState()->layers()) {
                    if (!l || l->m_layer != level || !viewVisible(l) || l->m_monitor.lock() != mon)
                        continue;
                    if (!l->m_ruleApplicator || l->m_ruleApplicator->noScreenShare().valueOrDefault())
                        continue;
                    const auto pos  = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
                    const auto size = l->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
                    CRegion    region{zoomed(mon, CBox{pos, size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos)};
                    region.intersect(coverArea);
                    if (region.empty())
                        continue;
                    NSC_TRACE("layer %s: redrawn over a cover\n", l->m_namespace.c_str());
                    redrawLayer(ctx, l, region, mon, capturePos);
                }
            }
        }

        paintExtraRects(ctx, mon, capturePos);
        nsc_end_frame();
        if ((!g_animBoxes.empty() && nsc_animating()) || !g_closing.empty())
            startPump();
        pruneTextures();
        drainNotifications();
    }

    // Capture client behind this frame: the portal for PipeWire streams (browsers,
    // Discord), or wf-recorder, grim, obs (wlrobs)... when they capture directly.
    // Empty name if it can't be found out.
    SClientId captureClient(Screenshare::CScreenshareFrame* frame) {
        // the session is owned by a unique pointer in the manager: its weak pointer can't be
        // lock()ed (hyprutils asserts), only checked and read
        if (!frame || frame->m_session.expired())
            return {};
        const auto* session = frame->m_session.get();
        if (!session || !session->m_client)
            return {};
        pid_t pid = 0;
        uid_t uid = 0;
        gid_t gid = 0;
        wl_client_get_credentials(session->m_client, &pid, &uid, &gid);
        if (pid <= 0)
            return {};
        char       buf[4096];
        const auto n = readlink(std::format("/proc/{}/exe", pid).c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) {
            // exe needs ptrace read access, which some setups deny; comm is always
            // readable (first 15 chars of the executable name)
            NSC_TRACE("capture client pid %d: /proc/pid/exe: %s, falling back to comm\n", (int)pid, std::strerror(errno));
            SClientId id{.fromComm = true};
            if (FILE* f = std::fopen(std::format("/proc/{}/comm", pid).c_str(), "r")) {
                if (std::fgets(buf, sizeof(buf), f))
                    id.name = buf;
                std::fclose(f);
            }
            while (!id.name.empty() && (id.name.back() == '\n' || id.name.back() == '\r'))
                id.name.pop_back();
            return id;
        }
        SClientId id{.path = std::string(buf, n)};
        if (id.path.ends_with(" (deleted)")) // the binary was updated while running
            id.path.resize(id.path.size() - 10);
        const auto slash = id.path.rfind('/');
        id.name          = slash == std::string::npos ? id.path : id.path.substr(slash + 1);
        return id;
    }

    // A full-path entry with ~ expanded and symlinks resolved, the way /proc/<pid>/exe
    // shows it (/run/current-system/sw/bin/grim -> /nix/store/<hash>-grim/bin/grim).
    // Resolved again after a second, not on every frame: a NixOS switch or a package
    // update moves the target while the config stays the same.
    struct SResolvedPath {
        std::string                           path;
        std::chrono::steady_clock::time_point at;
    };
    std::unordered_map<std::string, SResolvedPath> g_resolvedPaths;

    const std::string& resolvedPath(std::string_view entry) {
        const auto now       = std::chrono::steady_clock::now();
        auto [it, inserted] = g_resolvedPaths.try_emplace(std::string(entry));
        if (inserted || now - it->second.at >= std::chrono::seconds(1)) {
            std::string raw(entry);
            if (raw.starts_with("~/"))
                if (const char* home = std::getenv("HOME"))
                    raw = home + raw.substr(1);
            std::error_code ec;
            const auto      canon = std::filesystem::weakly_canonical(raw, ec);
            it->second            = {ec ? raw : canon.string(), now};
        }
        return it->second.path;
    }

    // "grim, /usr/bin/obs  wf-recorder" -> contains(id). An entry with a slash is a full
    // path and matches only that executable, so a copy elsewhere doesn't pass.
    bool listHas(const std::string& list, const SClientId& id) {
        if (id.name.empty())
            return false;
        size_t i = 0;
        while (i < list.size()) {
            const auto start = list.find_first_not_of(", \t", i);
            if (start == std::string::npos)
                break;
            const auto end   = list.find_first_of(", \t", start);
            const auto entry = std::string_view(list).substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (id.fromComm) {
                // show_to never lets a comm name through (see sees), so for hide_from a
                // match only hides more: compare with the entry's file name, cut to 15
                // chars like comm (xdg-desktop-portal-hyprland -> xdg-desktop-por)
                const auto file = entry.substr(entry.rfind('/') + 1);
                if (file == id.name || (id.name.size() == 15 && file.size() > 15 && file.starts_with(id.name)))
                    return true;
            } else if (entry.contains('/') ? resolvedPath(entry) == id.path : entry == id.name)
                return true;
            i = end;
        }
        return false;
    }

    bool hasEntries(const std::string& list) {
        return list.find_first_not_of(", \t") != std::string::npos;
    }

    std::string cfgString(const SP<Config::Values::CStringValue>& cfg) {
        return cfg ? cfg->value() : std::string{};
    }

    // comm names already told about (see sees)
    std::unordered_set<std::string> g_commWarned;

    // Hidden windows are hidden from every capture by default. show_to lets chosen
    // clients see them (own screenshots); hide_from, if set, hides only from its list
    // (a client that can't be identified is still covered). A window or layer rule with
    // its own no_screen_share_show_to / no_screen_share_hide_from uses only those.
    //
    // A name from comm is set by the process itself, so it never passes show_to (that
    // client counts as unknown); for hide_from it's enough to hide from it.
    bool SCaptureClient::sees(const SRuleValues& rules) const {
        const bool        ruleLists = rules.showTo || rules.hideFrom;
        const std::string show      = ruleLists ? rules.showTo.value_or("") : globalShow;
        const std::string hide      = ruleLists ? rules.hideFrom.value_or("") : globalHide;
        if (!hasEntries(show) && !hasEntries(hide))
            return false;
        if (!id) {
            id = captureClient(frame);
            NSC_TRACE("capture client: %s%s\n", id->name.empty() ? "(unknown)" : id->name.c_str(), id->fromComm ? " (comm)" : "");
        }
        if (hasEntries(hide))
            return !id->name.empty() && !listHas(hide, *id);
        if (id->fromComm) {
            // it would have been let through by its name: say once why it isn't
            if (listHas(show, *id) && g_commWarned.insert(id->name).second)
                notify(std::format("noshare-cover: capture client \"{}\" is known only by its process name "
                                   "(its /proc/<pid>/exe can't be read), show_to doesn't apply to it",
                                   id->name),
                       10000);
            return false;
        }
        return listHas(show, *id);
    }

    // The surfaces this client may see get no_screen_share switched off for the cover
    // pass as well, so paintCovers treats them as normal windows: no cover, occlusion
    // and redraws as for any other window.
    std::vector<SSuppressed> revealFor(const SCaptureClient& client, std::unordered_set<uintptr_t>& keys) {
        std::vector<SSuppressed> out;
        const auto               consider = [&](uintptr_t key, Desktop::Types::COverridableVar<bool>& var, const SRuleValues& rules) {
            if (!client.sees(rules))
                return;
            std::optional<bool> prev;
            if (var.hasValue() && var.getPriority() == Desktop::Types::PRIORITY_SET_PROP)
                prev = var.value();
            var.set(false, Desktop::Types::PRIORITY_SET_PROP);
            out.push_back({&var, prev});
            keys.insert(key);
        };
        for (const auto& w : Desktop::windowState()->windows())
            if (w && w->m_ruleApplicator && w->m_ruleApplicator->noScreenShare().valueOrDefault())
                consider(reinterpret_cast<uintptr_t>(w.get()), w->m_ruleApplicator->noScreenShare(), ruleValuesFor(w));
        for (const auto& l : Desktop::layerState()->layers())
            if (l && l->m_ruleApplicator && l->m_ruleApplicator->noScreenShare().valueOrDefault())
                consider(reinterpret_cast<uintptr_t>(l.get()), l->m_ruleApplicator->noScreenShare(), ruleValuesFor(l));
        if (!out.empty())
            NSC_TRACE("capture client sees %zu hidden surface(s) as they are\n", out.size());
        return out;
    }

    // A surface that stops being hidden while it's still on screen keeps its last cover for
    // the same close_hold / no_screen_share_cover_hold as a closing one: a browser changes
    // the window title before it repaints, so with a rule that matches the title the old
    // page would reach the stream for a frame or two when you switch away. Surfaces this
    // client may see (show_to / hide_from) are not held, with the rules they had while hidden.
    std::vector<SSuppressed> holdUnhidden(const std::unordered_set<uintptr_t>& revealed, const SCaptureClient& client) {
        std::vector<SSuppressed> out;
        const auto               now  = std::chrono::steady_clock::now();
        const auto               keep = [&](uintptr_t key, Desktop::Types::COverridableVar<bool>& var) {
            if (var.valueOrDefault() || revealed.contains(key))
                return;
            const auto it = g_lastCovers.find(key);
            if (it == g_lastCovers.end() || now - it->second.at >= holdFor(it->second.rules) || client.sees(it->second.rules))
                return;
            std::optional<bool> prev;
            if (var.hasValue() && var.getPriority() == Desktop::Types::PRIORITY_SET_PROP)
                prev = var.value();
            var.set(true, Desktop::Types::PRIORITY_SET_PROP);
            out.push_back({&var, prev});
            g_held.insert(key);
            g_heldRules[key] = it->second.rules;
        };
        for (const auto& w : Desktop::windowState()->windows())
            if (w && w->m_ruleApplicator && windowMapped(w))
                keep(reinterpret_cast<uintptr_t>(w.get()), w->m_ruleApplicator->noScreenShare());
        for (const auto& l : Desktop::layerState()->layers())
            if (l && l->m_ruleApplicator && viewVisible(l))
                keep(reinterpret_cast<uintptr_t>(l.get()), l->m_ruleApplicator->noScreenShare());
        if (!out.empty())
            NSC_TRACE("holding the cover of %zu surface(s) that just stopped being hidden\n", out.size());
        return out;
    }

#ifdef NSC_RENDER_CONTEXT
    // Hidden surfaces on mon with a transparent cover that this client doesn't see (after
    // revealFor and holdUnhidden), and whether one of them is still closing.
    struct STransparentOn {
        std::unordered_set<uintptr_t> keys;
        bool                          closing = false;
    };

    STransparentOn transparentOn(const PHLMONITOR& mon, const Vector2D& capturePos, const SCaptureClient& client) {
        STransparentOn out;
        const auto     hidden = [](const auto& v) { return v && v->m_ruleApplicator && v->m_ruleApplicator->noScreenShare().valueOrDefault(); };
        for (const auto& w : Desktop::windowState()->windows())
            if (hidden(w) && windowGeo(w, mon, capturePos) && transparentCover(ruleValuesFor(w)))
                out.keys.insert(reinterpret_cast<uintptr_t>(w.get()));
        for (const auto& l : Desktop::layerState()->layers())
            if (hidden(l) && viewVisible(l) && l->m_monitor.lock() == mon && transparentCover(ruleValuesFor(l)))
                out.keys.insert(reinterpret_cast<uintptr_t>(l.get()));
        const auto now     = std::chrono::steady_clock::now();
        const auto applies = [&](const SRuleValues& rules) { return transparentCover(rules) && !client.sees(rules); };
        out.closing        = std::ranges::any_of(g_closing, [&](const SClosing& c) { return c.mon.lock() == mon && (!c.holdUntil || now < *c.holdUntil) && applies(c.rules); }) ||
            // gone since the last frame: trackClosed makes it a closing one only after this
            // check, and the first frame of its fade-out must not show it either
            std::ranges::any_of(g_lastCovers, [&](const auto& kv) { return !out.keys.contains(kv.first) && kv.second.mon.lock() == mon && applies(kv.second.rules); });
        return out;
    }

    // IFramebuffer keeps its texture protected; a pointer to the member formed through a
    // derived class reaches it.
    struct SFramebufferAccess : Render::IFramebuffer {
        static SP<Render::ITexture>& texture(Render::IFramebuffer& fb) {
            return fb.*(&SFramebufferAccess::m_tex);
        }
    };

    SP<Render::IFramebuffer> g_sceneFB, g_sceneZoomFB;

    // fb allocated like the monitor's mirror image
    SP<Render::IFramebuffer> likeMirror(SP<Render::IFramebuffer>& fb, const char* name, const SP<Render::IFramebuffer>& mirror) {
        if (!fb)
            fb = g_pHyprRenderer->createFB(name);
        if (!fb || !fb->alloc(int(mirror->m_size.x), int(mirror->m_size.y), mirror->m_drmFormat))
            return nullptr;
        fb->setImageDescription(mirror->imageDescription());
        return fb;
    }

    // A monitor capture copies the monitor's mirror image, the frame as it was shown. To
    // leave surfaces out of it, the monitor is drawn again the way Hyprland draws its
    // frame, as a capture scene (m_renderingCapture) in which only those surfaces keep
    // no_screen_share: Hyprland skips them there, with their popups and fade-outs. Every
    // other hidden window is drawn as it is, with its border and shadow, and gets its
    // cover afterwards as usual. The result stands in for the mirror's texture while this
    // lives, so Hyprland's own copy (region, scale, cursor) takes it as it takes the mirror.
    class CSceneSwap {
      public:
        CSceneSwap(Render::CRenderContext& ctx, const PHLMONITOR& mon, const std::unordered_set<uintptr_t>& left);
        ~CSceneSwap() {
            if (m_mirror)
                SFramebufferAccess::texture(*m_mirror) = m_real;
        }
        CSceneSwap(const CSceneSwap&)            = delete;
        CSceneSwap& operator=(const CSceneSwap&) = delete;

        bool        drawn() const {
            return !!m_mirror;
        }

      private:
        SP<Render::IFramebuffer> m_mirror;
        SP<Render::ITexture>     m_real;
    };

    CSceneSwap::CSceneSwap(Render::CRenderContext& ctx, const PHLMONITOR& mon, const std::unordered_set<uintptr_t>& left) {
        const auto res = mon->resources();
        // locked: the mirror holds the lock screen, nothing to leave out
        if (res.expired() || !res->hasMirrorFB() || mon->isMirror() || (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()))
            return;
        const auto mirror = res->mirrorFB();
        const auto size   = mon->m_transformedSize;
        const auto fb     = mirror->m_size == size ? likeMirror(g_sceneFB, "noshare-cover scene", mirror) : nullptr;
        const auto zoom   = g_zoom.find(mon->m_id);
        const auto out    = !fb || zoom == g_zoom.end() ? fb : likeMirror(g_sceneZoomFB, "noshare-cover zoomed scene", mirror);
        if (!out) {
            NSC_TRACE("scene: can't draw %s without the transparent surfaces\n", mon->m_name.c_str());
            return;
        }

        {
            const auto                          shown = suppressNoScreenShare(left);
            const Hyprutils::Utils::CScopeGuard hideAgain{[&] { restoreNoScreenShare(shown); }};
            const auto                          state    = ctx.saveDrawState(); // render data, backdrops and the blur flag below
            const bool                          capture  = ctx.m_renderingCapture;
            const bool                          feedback = ctx.m_blockSurfaceFeedback;
            const Hyprutils::Utils::CScopeGuard flags{[&] {
                ctx.m_renderingCapture     = capture;
                ctx.m_blockSurfaceFeedback = feedback;
            }};
            ctx.m_renderingCapture     = true;
            ctx.m_blockSurfaceFeedback = true;  // the surfaces had their frame already
            mon->m_blurFBShouldRender  = false; // the cached blur is the main frame's to update

            const auto    bound = g_pHyprRenderer->bindTempFB(ctx, fb);
            const CRegion full{CBox{{}, size}};
            ctx.m_data.mainFB      = fb;
            ctx.m_data.damage      = full;
            ctx.m_data.finalDamage = full;
            g_pHyprRenderer->setProjectionType(ctx, Render::RPT_MONITOR);
            g_pHyprRenderer->setViewport(0, 0, size.x, size.y);
            drawElement(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, full);
            {
                // into a pass of its own: the frame's pass is drawn when the capture ends, over the covers
                Render::CRenderPass pass;
                const auto          redirect = g_pHyprRenderer->redirectPass(ctx, &pass);
                const auto          now      = Time::steadyNow();
                Render::CMonitorScene{mon}.draw(ctx, now);
                // a software cursor is drawn into the frame, so the mirror image has it
                if (g_pHyprRenderer->shouldRenderCursor()) {
                    CRegion damage = full;
                    Pointer::mgr()->renderSoftwareCursorsFor(ctx, mon, now, damage);
                }
                pass.render(ctx, full);
            }
            if (out != fb) {
                // the mirror image is taken after the cursor zoom (see recordZoom)
                const auto boundOut = g_pHyprRenderer->bindTempFB(ctx, out);
                drawElement(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, full);
                drawElement(ctx, CTexPassElement::SRenderData{.tex = fb->getTexture(), .box = zoom->second.box}, full);
            }
        }
        NSC_TRACE("scene: %s drawn without %zu transparent surface(s)\n", mon->m_name.c_str(), left.size());
        m_mirror = mirror;
        m_real   = std::exchange(SFramebufferAccess::texture(*mirror), out->getTexture());
    }

    // A software cursor is part of the monitor's image, and the covers went over it: draw
    // it again on top, only where something was painted (elsewhere the image still has it).
    // A hardware cursor that the client asked for is added by Hyprland when the capture
    // ends, over everything already.
    void cursorOverCovers(Render::CRenderContext& ctx, const PHLMONITOR& mon, const Vector2D& capturePos) {
        if (!g_cfgCursorOverCovers || !g_cfgCursorOverCovers->value() || g_painted.empty() || !Pointer::mgr()->softwareLockedFor(mon) ||
            !g_pHyprRenderer->shouldRenderCursor())
            return;
        const auto          state = ctx.saveDrawState();
        Render::CRenderPass pass;
        {
            const auto redirect = g_pHyprRenderer->redirectPass(ctx, &pass);
            CRegion    damage   = g_painted;
            Pointer::mgr()->renderSoftwareCursorsFor(ctx, mon, Time::steadyNow(), damage, Pointer::mgr()->untransformedPosition() - mon->m_position, true, true);
        }
        // where the image has it: under cursor zoom that's the zoomed frame (see recordZoom)
        const auto zoom                = g_zoom.find(mon->m_id);
        ctx.m_data.renderModif         = {};
        ctx.m_data.renderModif.modifs  = {{Render::SRenderModifData::RMOD_TYPE_SCALE, float(zoomScale(mon))},
                                          {Render::SRenderModifData::RMOD_TYPE_TRANSLATE, (zoom == g_zoom.end() ? Vector2D{} : zoom->second.box.pos()) - capturePos}};
        ctx.m_data.noSimplify          = true;
        pass.render(ctx, g_painted);
    }
#endif

    // original: Hyprland's renderMonitor for this frame
    template <class F>
    void renderMonitorWithCovers(Screenshare::CScreenshareFrame* self, RenderCtx& ctx, const F& original) {
        NSC_TRACE("hook: renderMonitor\n");
        std::unordered_set<uintptr_t> revealedKeys;
        const SCaptureClient          client{.frame = self, .globalShow = cfgString(g_cfgShowTo), .globalHide = cfgString(g_cfgHideFrom)};
        const auto                    revealed = revealFor(client, revealedKeys);
        const auto                    held     = holdUnhidden(revealedKeys, client);
        const Hyprutils::Utils::CScopeGuard restore{[&] {
            restoreNoScreenShare(held);
            restoreNoScreenShare(revealed);
            g_held.clear();
            g_heldRules.clear();
        }};
        g_painted = CRegion{};
        bool                 left = false;
        SP<Render::ITexture> image; // what Hyprland draws the frame from, for blur and pixelate
        {
#ifdef NSC_RENDER_CONTEXT
            std::optional<CSceneSwap> scene;
            const auto                mon = ctx.m_data.pMonitor.lock();
            if (mon && self && self->m_session) {
                const auto gone = transparentOn(mon, self->m_session->m_captureBox.pos(), client);
                if (!gone.keys.empty() || gone.closing)
                    left = scene.emplace(ctx, mon, gone.keys).drawn();
            }
            if (mon && !mon->resources().expired())
                image = mon->resources()->getMirrorTexture(); // the scene, if it was swapped in
#endif
            // Hyprland's boxes are always switched off and drawn by us: under zoom they land
            // in the wrong place, and they are drawn over everything, including windows that
            // sit on top of a hidden one.
            const auto                          suppressed = suppressNoScreenShare();
            const Hyprutils::Utils::CScopeGuard unsuppress{[&] { restoreNoScreenShare(suppressed); }};
            original();
        }
        paintCovers(ctx, self, client, revealedKeys, left, image);
#ifdef NSC_RENDER_CONTEXT
        if (const auto mon = ctx.m_data.pMonitor.lock(); mon && self && self->m_session)
            cursorOverCovers(ctx, mon, self->m_session->m_captureBox.pos());
#endif
    }

#ifdef NSC_RENDER_CONTEXT
    // A capture into a client's dmabuf (portal streams: Discord, OBS) renders straight into a
    // GL renderbuffer, which has no texture. Blur reads the render target, so under a
    // translucent window over a cover Hyprland fell back to its cached blur of the wallpaper.
    // With blur on, such a frame is drawn into a framebuffer of our own and copied over at
    // the end, the way Hyprland draws a stored frame (m_tempFB).
    SP<Render::IFramebuffer> g_offscreen;

    SP<Render::IFramebuffer> offscreenFor(const SP<Render::IFramebuffer>& target) {
        static auto PBLUR = CConfigValue<Config::INTEGER>("decoration:blur:enabled");
        if (!*PBLUR || !target || target->getTexture() || target->m_size.x < 1 || target->m_size.y < 1)
            return nullptr;
        if (!g_offscreen)
            g_offscreen = g_pHyprRenderer->createFB("noshare-cover capture");
        const auto format = target->m_drmFormat != DRM_FORMAT_INVALID ? target->m_drmFormat : DRM_FORMAT_ARGB8888;
        if (!g_offscreen || !g_offscreen->alloc(int(target->m_size.x), int(target->m_size.y), format))
            return nullptr; // not drawable in this format: render as before
        g_offscreen->setImageDescription(target->imageDescription());
        return g_offscreen;
    }

    void hkRenderMonitor(Screenshare::CScreenshareFrame* self, Render::CRenderContext& ctx) {
        using Fn            = void (*)(Screenshare::CScreenshareFrame*, Render::CRenderContext&);
        const auto original = [&] { reinterpret_cast<Fn>(g_hook->m_original)(self, ctx); };
        const auto target   = ctx.m_data.currentFB;
        const auto off      = offscreenFor(target);
        if (!off) {
            renderMonitorWithCovers(self, ctx, original);
            return;
        }
        const CBox    full{{}, target->m_size};
        const CRegion region{full};
        {
            const auto bound = g_pHyprRenderer->bindTempFB(ctx, off);
            drawElement(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, region);
            renderMonitorWithCovers(self, ctx, original);
        }
        drawElement(ctx, CTexPassElement::SRenderData{.tex = off->getTexture(), .box = full}, region);
    }
#else
    void hkRenderMonitor(Screenshare::CScreenshareFrame* self) {
        using Fn = void (*)(Screenshare::CScreenshareFrame*);
        RenderCtx ctx;
        renderMonitorWithCovers(self, ctx, [&] { reinterpret_cast<Fn>(g_hook->m_original)(self); });
    }
#endif

    template <typename T, typename... Args>
    SP<T> makeValue(const char* name, const char* desc, Args&&... def) {
        auto v = Config::Values::makeConfigValue<T>(name, desc, std::forward<Args>(def)...);
        if (!v || !HyprlandAPI::addConfigValueV2(g_handle, v))
            notify(std::string("noshare-cover: failed to register config value ") + name, 5000);
        return v;
    }

} // namespace

// Public ABI for other plugins (include/noshare_cover_api.h). The Rust archive
// is linked hidden, so the symbols are exported here via thin wrappers.
#define NSC_PUBLIC extern "C" __attribute__((visibility("default")))
NSC_PUBLIC uint32_t noshare_cover_api_version() {
    return nsc_api_api_version();
}
NSC_PUBLIC uint64_t noshare_cover_register_client(const char* name) {
    return nsc_api_register_client(name);
}
NSC_PUBLIC void noshare_cover_unregister_client(uint64_t client) {
    nsc_api_unregister_client(client);
}
NSC_PUBLIC bool noshare_cover_set_rects(uint64_t client, int monitor_id, const noshare_cover_rect* rects, size_t count) {
    return nsc_api_set_rects(client, monitor_id, rects, count);
}
NSC_PUBLIC bool noshare_cover_clear_client_rects(uint64_t client) {
    return nsc_api_clear_client_rects(client);
}
NSC_PUBLIC void noshare_cover_clear_extra_rects() {
    nsc_api_clear_extra_rects();
}
NSC_PUBLIC void noshare_cover_add_extra_rect(int monitor_id, double x, double y, double w, double h, double rounding) {
    nsc_api_add_extra_rect(monitor_id, x, y, w, h, rounding);
}

NSC_PUBLIC bool noshare_cover_set_gone_callback(uint64_t client, void (*cb)(void*), void* user) {
    return nsc_api_set_gone_callback(client, cb, user);
}

namespace {
    bool tryInstallHook() {
        if (g_hook)
            return true;
        auto* h = HyprlandAPI::createFunctionHook(g_handle, g_hookTarget, reinterpret_cast<void*>(&hkRenderMonitor));
        if (h && h->hook()) {
            g_hook = h;
            return true;
        }
        if (h)
            HyprlandAPI::removeFunctionHook(g_handle, h);
        return false;
    }

    void stopHookRetry() {
        if (!g_hookRetry)
            return;
        g_hookRetry->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g_hookRetry);
        g_hookRetry.reset();
    }
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace {
    // Everything PLUGIN_EXIT undoes. Also run when PLUGIN_INIT throws: Hyprland then
    // unloads the plugin with eject=true and does NOT call PLUGIN_EXIT, so anything
    // registered before the throw (signal listeners, timers, rule effects) would be left
    // pointing into an unloaded .so and crash the compositor on the next config reload.
    void cleanupAll() {
        g_onReload.reset();
        g_onWindowClose.reset();
        g_onLayerClose.reset();
        g_onWindowOpen.reset();
        g_onWindowRules.reset();
        g_onLayerOpen.reset();
        g_onLayerRules.reset();
        stopHookRetry();
        stopPump();
        // Remove the hook first: Hyprland cleans up hooks only after PLUGIN_EXIT, and a
        // screencast frame in between must not land in the unloaded core.
        if (g_hook) {
            HyprlandAPI::removeFunctionHook(g_handle, g_hook);
            g_hook = nullptr;
        }
        if (g_zoomHook) {
            HyprlandAPI::removeFunctionHook(g_handle, g_zoomHook);
            g_zoomHook = nullptr;
        }
        g_zoom.clear();
#ifdef NSC_RENDER_CONTEXT
        g_offscreen.reset();
        g_sceneFB.reset();
        g_sceneZoomFB.reset();
        g_fxBuffers.clear();
#endif
        // API clients (gloview) drop our pointers and may take over renderMonitor.
        nsc_api_notify_gone();
        g_textures.clear();
        g_lastCovers.clear();
        g_closing.clear();
        nsc_shutdown(); // stops and joins decode threads, clears extra rects

        if (const auto& fx = Desktop::Rule::windowEffects()) {
            for (const auto& e : g_effects)
                if (e.id)
                    fx->unregisterEffect(e.id);
        }
        for (auto& e : g_effects)
            e.id = 0;
        if (const auto& fx = Desktop::Rule::layerEffects()) {
            for (const auto& e : g_layerEffects)
                if (e.id)
                    fx->unregisterEffect(e.id);
        }
        for (auto& e : g_layerEffects)
            e.id = 0;
        g_cfgPath.reset();
        g_cfgLoop.reset();
        g_cfgSpeed.reset();
        g_cfgBackend.reset();
        g_cfgGpu.reset();
        g_cfgCloseHold.reset();
        g_cfgShowTo.reset();
        g_cfgHideFrom.reset();
        g_cfgCursorOverCovers.reset();
        g_handle = nullptr;
    }
} // namespace

namespace {
    PLUGIN_DESCRIPTION_INFO initImpl(HANDLE handle) {
        g_handle = handle;
        const PLUGIN_DESCRIPTION_INFO info{"noshare-cover", "image or video instead of the no_screen_share black box", "gitscout-bot", NSC_VERSION};

        // A plugin built against other headers reads wrong field offsets and
        // crashes the compositor. Bail out right away: Hyprland catches the exception,
        // unloads the plugin and shows the reason.
        const std::string running{__hyprland_api_get_hash()};
        const std::string built{__hyprland_api_get_client_hash()};
        if (running != built) {
            const std::string why = "noshare-cover: built for Hyprland " + built.substr(0, 7) + ", running " + running.substr(0, 7) +
                ". Rebuild against the running Hyprland (hyprpm update; on Nix use programs.noshare-cover.enable)";
            notify(why, 15000);
            throw std::runtime_error(why);
        }

        if (!nsc_init())
            throw std::runtime_error("noshare-cover: core init failed");

        for (auto& fx : g_effects)
            fx.id = Desktop::Rule::windowEffects()->registerEffect(fx.name);
        for (auto& fx : g_layerEffects)
            fx.id = Desktop::Rule::layerEffects()->registerEffect(fx.name);

        g_cfgPath    = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:path_cover", "Default media for no_screen_share windows", Config::STRING{});
        g_cfgLoop    = makeValue<Config::Values::CBoolValue>("plugin:no_screen_share_cover:loop", "Loop gif and video", true);
        g_cfgSpeed   = makeValue<Config::Values::CFloatValue>("plugin:no_screen_share_cover:speed", "Playback speed for gif and video", 1.F);
        g_cfgBackend = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:backend", "Video decode backend: auto, gpu or cpu", Config::STRING{"auto"});
        g_cfgGpu     = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:gpu_device", "Render node for GPU decode, empty = first one", Config::STRING{});
        g_cfgCloseHold = makeValue<Config::Values::CIntValue>("plugin:no_screen_share_cover:close_hold", "Keep the cover this many ms after a closed window's animation ends",
                                                              Config::INTEGER{0});
        g_cfgShowTo   = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:show_to",
                                                                "Capture clients (executable names or full paths) that see hidden windows as they are, e.g. \"grim, /usr/bin/obs\"", Config::STRING{});
        g_cfgHideFrom = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:hide_from",
                                                                "If set, hide only from these capture clients (executable names or full paths); everyone else sees everything", Config::STRING{});
        g_cfgCursorOverCovers = makeValue<Config::Values::CBoolValue>("plugin:no_screen_share_cover:cursor_over_covers",
                                                                      "Draw a software cursor that the capture shows over the covers too", true);

        void* target = nullptr;
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, "renderMonitor")) {
            if (match.demangled.find("CScreenshareFrame::renderMonitor") != std::string::npos) {
                target = match.address;
                break;
            }
        }
        if (!target)
            throw std::runtime_error("noshare-cover: CScreenshareFrame::renderMonitor not found");

        g_onWindowClose = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) { onWindowClose(w); });
        g_onLayerClose  = Event::bus()->m_events.layer.closed.listen([](PHLLS l) { onLayerClose(l); });
        g_onWindowOpen  = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { prewarmWindow(w); });
        g_onWindowRules = Event::bus()->m_events.window.updateRules.listen([](PHLWINDOW w) { prewarmWindow(w); });
        g_onLayerOpen   = Event::bus()->m_events.layer.opened.listen([](PHLLS l) { prewarmLayer(l); });
        g_onLayerRules  = Event::bus()->m_events.layer.updateRules.listen([](PHLLS l) { prewarmLayer(l); });
        // windows already open when the plugin loads (hyprpm reload, config reload)
        for (const auto& w : Desktop::windowState()->windows())
            prewarmWindow(w);
        for (const auto& l : Desktop::layerState()->layers())
            prewarmLayer(l);

        g_onReload = Event::bus()->m_events.config.reloaded.listen([] {
            pushSettings();
            drainNotifications();
        });

        // Optional: without it covers just don't follow cursor zoom.
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, "applyZoomTransform")) {
            if (match.demangled.find("CMonitorZoomController::applyZoomTransform") == std::string::npos)
                continue;
            g_zoomHook = HyprlandAPI::createFunctionHook(handle, match.address, reinterpret_cast<void*>(&hkApplyZoom));
            if (g_zoomHook && !g_zoomHook->hook()) {
                HyprlandAPI::removeFunctionHook(handle, g_zoomHook);
                g_zoomHook = nullptr;
            }
            break;
        }
        NSC_TRACE("zoom hook: %s\n", g_zoomHook ? "on" : "off");

        g_hookTarget = target;
        if (!tryInstallHook()) {
            // Hooked by another plugin. Wait until it's released (newer gloview does this
            // right after a config reload); until then no covers are drawn.
            NSC_TRACE("renderMonitor busy, retrying\n");
            g_hookRetry = makeShared<CEventLoopTimer>(
                HOOK_RETRY_EVERY,
                [](SP<CEventLoopTimer> self, void*) {
                    if (tryInstallHook()) {
                        NSC_TRACE("renderMonitor hooked after retry\n");
                        self->updateTimeout(std::nullopt);
                        return;
                    }
                    self->updateTimeout(HOOK_RETRY_EVERY);
                },
                nullptr);
            g_pEventLoopManager->addTimer(g_hookRetry);
        }
        return info;
    }
} // namespace

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    try {
        return initImpl(handle);
    } catch (...) {
        cleanupAll();
        throw;
    }
}


APICALL EXPORT void PLUGIN_EXIT() {
    cleanupAll();
}
