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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
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
#include "render/pass/RectPassElement.hpp"
#include "render/pass/TexPassElement.hpp"

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

    float windowFade(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
#else
        return w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
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

    HANDLE         g_handle = nullptr;
    // push settings to the core right after every config reload, not on the first
    // screencast frame: the default cover gets warmed up before capture starts
    CHyprSignalListener g_onReload;
    // window/layer close: start the closing cover right when Hyprland unmaps it
    CHyprSignalListener g_onWindowClose;
    CHyprSignalListener g_onLayerClose;

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
    // would freeze. While the monitor is being shared and has a live animation,
    // we damage the cover areas ~60 times per second.
    // The condition is the share itself, not a recent capture frame: otherwise
    // a pause in frames would stop the pump and nothing could wake it up again.
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

    void pushSettings() {
        const std::string path    = g_cfgPath ? g_cfgPath->value() : std::string{};
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

    // Every couple of seconds, drop textures of covers the core has already closed.
    void pruneTextures() {
        if (++g_frameCount % 120 != 0)
            return;
        std::erase_if(g_textures, [](const auto& kv) { return !nsc_cover_alive(kv.first); });
    }

    // The last matching rule wins, same as in Hyprland itself.
    struct SRuleValues {
        std::optional<std::string> path, speed, loop, hold;
        std::optional<std::string> showTo, hideFrom; // per-rule capture client lists, override the global ones
    };

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

    using ApplyZoomFn = void (*)(Monitor::CMonitorZoomController*, CBox&, const Render::SRenderData&);

    // Only the normal monitor render feeds the screencast. m_renderMode is protected in
    // some releases; there we can't tell and take every call.
    template <class R>
    bool normalRender(const R* r) {
        if constexpr (requires { r->m_renderMode; })
            return r->m_renderMode == Render::RENDER_MODE_NORMAL;
        else
            return true;
    }

    void hkApplyZoom(Monitor::CMonitorZoomController* self, CBox& monbox, const Render::SRenderData& rd) {
        reinterpret_cast<ApplyZoomFn>(g_zoomHook->m_original)(self, monbox, rd);
        const auto mon = rd.pMonitor.lock();
        if (!mon || !normalRender(g_pHyprRenderer.get()))
            return;
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

    std::vector<SSuppressed> suppressNoScreenShare() {
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
            if (w && w->m_ruleApplicator)
                off(w->m_ruleApplicator->noScreenShare());
        for (const auto& l : Desktop::layerState()->layers())
            if (l && l->m_ruleApplicator)
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

    // clip: where drawing is allowed (the draw is scissored to it); empty = the whole box
    void drawBlack(const CBox& box, int round = 0, float roundingPower = 2.F, const CRegion& clip = {}) {
        g_pHyprRenderer->draw(CRectPassElement::SRectData{.box = box, .color = CHyprColor{0.F, 0.F, 0.F, 1.F}, .round = round, .roundingPower = roundingPower},
                              clip.empty() ? CRegion{box} : clip);
    }

    // Popups (menus, tooltips) of a hidden window or layer, black like Hyprland does.
    // Only needed when we draw the boxes ourselves (under zoom).
    template <class V>
    void paintPopupBoxes(const V& view, const Vector2D& baseOffset, const PHLMONITOR& mon, const Vector2D& capturePos) {
        const auto head = popupHeadOf(view);
        if (!head)
            return;
        head->breadthfirst(
            [&](SP<Desktop::View::CPopup> popup, void*) {
                if (!popup || !popup->wlSurface() || !popup->wlSurface()->resource() || !viewVisible(popup))
                    return;
                const auto rel = popup->coordsRelativeToParent();
                popup->wlSurface()->resource()->breadthfirst(
                    [&](SP<CWLSurfaceResource> surf, const Vector2D& local, void*) {
                        const auto box = zoomed(mon, CBox{baseOffset + rel + local, surf->m_current.size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
                        if (box.w > 0 && box.h > 0)
                            drawBlack(box);
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
            const auto             rules = ruleValuesFor(w);
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

    // Hyprland emits window.close / layer.closed from unmap, while the window or layer
    // still has its geometry and rules and before its fade-out exists. Starting the
    // closing cover here doesn't depend on the stream sending frames (a static screen
    // sends none, and the frame-based tracking above then only has old data).
    void startClosing(bool isLayer, uintptr_t key, const PHLMONITOR& mon, const CBox& box, SRuleValues&& rules, float rounding, float roundingPower) {
        const auto& mgr = Screenshare::mgr();
        if (!mon || !mgr || !mgr->isOutputBeingSSd(mon))
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

    void onLayerClose(const PHLLS& l) {
        if (!l || !l->m_ruleApplicator || !l->m_ruleApplicator->noScreenShare().valueOrDefault())
            return;
        const auto pos  = l->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto size = l->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        startClosing(true, reinterpret_cast<uintptr_t>(l.get()), l->m_monitor.lock(), CBox{pos, size}, ruleValuesFor(l), 0.F, 2.F);
    }

    // Returns the area it drew over (capture pixels).
    CRegion paintClosing(const PHLMONITOR& mon, const Vector2D& capturePos, std::chrono::steady_clock::time_point now) {
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
                    c.holdUntil = now + holdFor(c.rules);
            }
            if (c.holdUntil && now >= *c.holdUntil)
                continue; // the pump drops it and repaints

            const auto box =
                zoomed(mon, CBox{c.box.x, c.box.y, std::max(c.box.w, 5.0), std::max(c.box.h, 5.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
            if (box.w < 1 || box.h < 1)
                continue;
            const int              round = capturePos != Vector2D{} ? 0 : int(std::lround(c.rounding * mon->m_scale * zoomScale(mon)));
            const nsc_play_request req{
                .rule_path  = c.rules.path ? c.rules.path->c_str() : nullptr,
                .rule_speed = c.rules.speed ? c.rules.speed->c_str() : nullptr,
                .rule_loop  = c.rules.loop ? c.rules.loop->c_str() : nullptr,
            };
            nsc_frame  f{};
            const auto tex = nsc_resolve(&req, &f) ? textureFor(f) : nullptr;
            if (tex)
                g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round, .roundingPower = c.roundingPower}, box);
            else
                drawBlack(box, round, c.roundingPower); // no cover set: at least hide it like no_screen_share would
            drawnArea.add(box);
        }
        return drawnArea;
    }

    // Draw a layer again from its surface textures with its alpha, limited to clip.
    void redrawLayer(const PHLLS& l, const CRegion& clip, const PHLMONITOR& mon, const Vector2D& capturePos) {
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
                    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box, .a = alpha, .clipRegion = clip}, clip);
            },
            nullptr);
    }

    void paintExtraRects(const PHLMONITOR& mon, const Vector2D& capturePos) {
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
                    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round}, box);
                    continue;
                }
            }
            g_pHyprRenderer->draw(
                CRectPassElement::SRectData{
                    .box   = box,
                    .color = CHyprColor{0.F, 0.F, 0.F, 1.F},
                    .round = round,
                },
                box);
        }
    }

    void pumpTick(SP<CEventLoopTimer> self, void*) {
        const auto& mgr    = Screenshare::mgr();
        bool        any    = false;
        const auto  shared = [&](const PHLMONITORREF& ref) {
            const auto mon = ref.lock();
            return mgr && mon && mgr->isOutputBeingSSd(mon);
        };
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
        bool     floating      = false;
        bool     opaque        = false;
    };

    std::optional<SWinGeo> windowGeo(const PHLWINDOW& w, const PHLMONITOR& mon, const Vector2D& capturePos) {
        if (!g_pHyprRenderer->shouldRenderWindow(w, mon) || w->isHidden())
            return std::nullopt;
        const auto  fade = windowFade(w);
        const auto* ws   = w->m_workspace.get();
        if (!ws && fade != 0.F)
            return std::nullopt;

        SWinGeo    g;
        const auto renderOffset = ws && !windowPinned(w) && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        g.size                  = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        g.pos                   = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        g.box = zoomed(mon, CBox{g.pos.x, g.pos.y, std::max(g.size.x, 5.0), std::max(g.size.y, 5.0)}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
        if (g.box.w < 1 || g.box.h < 1)
            return std::nullopt;
        const bool fullscreen = Fullscreen::controller() && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
        const bool dontRound  = capturePos != Vector2D{} || fullscreen;
        g.round               = dontRound ? 0 : int(std::lround(windowRounding(w) * mon->m_scale * zoomScale(mon)));
        g.roundingPower       = dontRound ? 2.F : windowRoundingPower(w);
        g.floating            = windowFloating(w);
        g.opaque              = windowOpaque(w);
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
    void redrawWindow(const PHLWINDOW& w, const SWinGeo& geo, const CRegion& clip, const PHLMONITOR& mon, const Vector2D& capturePos) {
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
        res->breadthfirst(
            [&](SP<CWLSurfaceResource> surf, const Vector2D& local, void*) {
                const auto tex = surf->m_current.texture;
                if (!tex)
                    return;
                const bool main = surf == res;
                const auto box  = zoomed(mon, CBox{base + local, surf->m_current.size}.translate(-mon->m_position).scale(mon->m_scale)).translate(-capturePos);
                if (box.w < 1 || box.h < 1)
                    return;
                g_pHyprRenderer->draw(
                    CTexPassElement::SRenderData{
                        .tex                   = tex,
                        .box                   = box,
                        .a                     = alpha,
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

    // ownBoxes: Hyprland's black boxes were suppressed for this frame (see
    // suppressNoScreenShare), so everything without a cover is drawn black here.
    void paintCovers(Screenshare::CScreenshareFrame* frame, bool ownBoxes) {
        if (!frame || !frame->m_session || !g_pHyprRenderer) {
            NSC_TRACE("skip: frame %p session %d renderer %d\n", static_cast<void*>(frame), frame && frame->m_session ? 1 : 0, g_pHyprRenderer ? 1 : 0);
            return;
        }
        const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
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

        // Windows drawn on top of others: later in the stack (floating over tiled, then
        // list order). A cover is only drawn where its window is actually visible, so an
        // opaque window placed over a hidden one stays visible. Layers (bars) are left
        // alone: they are often translucent and we can't tell.
        struct SVisible {
            PHLWINDOW w;
            SWinGeo   geo;
        };
        std::vector<SVisible> visible;
        for (const auto& w : Desktop::windowState()->windows())
            if (w)
                if (const auto g = windowGeo(w, mon, capturePos))
                    visible.push_back({w, *g});
        // o is stacked above me
        const auto isAbove = [&](size_t o, size_t me) {
            const auto& a = visible[o].geo;
            const auto& b = visible[me].geo;
            return o != me && ((a.floating && !b.floating) || (a.floating == b.floating && o > me));
        };
        const auto visibleRegion = [&](size_t i) {
            CRegion region{visible[i].geo.box};
            // Only opaque windows hide what is below them. Under a translucent one the
            // hidden window would show through, so the cover goes there too and the
            // translucent window is drawn again on top of it (see below).
            for (size_t j = 0; j < visible.size(); ++j)
                if (isAbove(j, i) && visible[j].geo.opaque)
                    subtractRounded(region, visible[j].geo.box, visible[j].geo.round);
            return region;
        };
        std::vector<std::pair<size_t, CRegion>> drawn; // covers (or black) drawn, per window

        std::unordered_set<uintptr_t> seen;
        for (size_t i = 0; i < visible.size(); ++i) {
            const auto& w = visible[i].w;
            if (!w->m_ruleApplicator || !w->m_ruleApplicator->noScreenShare().valueOrDefault())
                continue;

            const auto& geo           = visible[i].geo;
            const auto  pos           = geo.pos;
            const auto  size          = geo.size;
            const auto  box           = geo.box;
            const int   round         = geo.round;
            const auto  roundingPower = geo.roundingPower;
            g_animBoxes.push_back({mon, CBox{pos.x, pos.y, size.x, size.y}});

            const CRegion clip = visibleRegion(i);
            if (clip.empty()) {
                NSC_TRACE("window %s: fully under other windows\n", windowClass(w).c_str());
            }
            if (ownBoxes && !windowIsX11(w)) {
                if (const auto geo = windowClientGeometryPos(w))
                    paintPopupBoxes(w, pos - *geo, mon, capturePos);
            }

            const auto rules = ruleValuesFor(w);
            const auto key   = reinterpret_cast<uintptr_t>(w.get());
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

            const nsc_play_request req{
                .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
            };
            if (clip.empty())
                continue;
            nsc_frame f{};
            if (!nsc_resolve(&req, &f)) {
                NSC_TRACE("window %s: no cover frame yet\n", windowClass(w).c_str());
                if (ownBoxes) {
                    drawBlack(box, round, roundingPower, clip);
                    drawn.emplace_back(i, clip);
                }
                continue;
            }
            const auto tex = textureFor(f);
            if (!tex) {
                NSC_TRACE("window %s: texture failed (%ux%u)\n", windowClass(w).c_str(), f.width, f.height);
                if (ownBoxes) {
                    drawBlack(box, round, roundingPower, clip);
                    drawn.emplace_back(i, clip);
                }
                continue;
            }
            NSC_TRACE("window %s: cover %ux%u at %.0f,%.0f %.0fx%.0f\n", windowClass(w).c_str(), f.width, f.height, box.x, box.y, box.w, box.h);
            g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round, .roundingPower = roundingPower}, clip);
            drawn.emplace_back(i, clip);
        }

        // Translucent windows above a hidden one, bottom to top: draw them again over the
        // cover, so what shows through them (blurred, if they blur) is the cover and never
        // the hidden window. Only over covers below them, minus whatever is above them.
        if (!drawn.empty()) {
            std::vector<size_t> order(visible.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::ranges::stable_sort(order, {}, [&](size_t k) { return visible[k].geo.floating; });
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
                    if (isAbove(k, j) && visible[k].geo.opaque)
                        subtractRounded(region, visible[k].geo.box, visible[k].geo.round);
                for (const auto& [i, clip] : drawn)
                    if (isAbove(i, j))
                        region.subtract(clip);
                if (region.empty())
                    continue;
                NSC_TRACE("window %s: redrawn over a cover\n", windowClass(visible[j].w).c_str());
                redrawWindow(visible[j].w, visible[j].geo, region, mon, capturePos);
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
            g_animBoxes.push_back({mon, CBox{pos.x, pos.y, size.x, size.y}});
            if (ownBoxes)
                paintPopupBoxes(l, pos - l->m_geometry.pos(), mon, capturePos);
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

            const nsc_play_request req{
                .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
            };
            nsc_frame  f{};
            const auto tex = nsc_resolve(&req, &f) ? textureFor(f) : nullptr;
            if (tex) {
                NSC_TRACE("layer %s: cover at %.0f,%.0f %.0fx%.0f\n", l->m_namespace.c_str(), box.x, box.y, box.w, box.h);
                g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box}, box);
            } else if (ownBoxes)
                drawBlack(box);
        }

        trackClosed(mon, seen, now);
        CRegion coverArea = paintClosing(mon, capturePos, now);

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
                    redrawLayer(l, region, mon, capturePos);
                }
            }
        }

        paintExtraRects(mon, capturePos);
        nsc_end_frame();
        if ((!g_animBoxes.empty() && nsc_animating()) || !g_closing.empty())
            startPump();
        pruneTextures();
        drainNotifications();
    }

    // Executable name of the capture client behind this frame: the portal for
    // PipeWire streams (browsers, Discord), or wf-recorder, grim, obs (wlrobs)...
    // when they capture directly. Empty if it can't be found out.
    std::string captureClientExe(Screenshare::CScreenshareFrame* frame) {
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
            std::string comm;
            if (FILE* f = std::fopen(std::format("/proc/{}/comm", pid).c_str(), "r")) {
                if (std::fgets(buf, sizeof(buf), f))
                    comm = buf;
                std::fclose(f);
            }
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
                comm.pop_back();
            return comm;
        }
        std::string exe(buf, n);
        if (exe.ends_with(" (deleted)")) // the binary was updated while running
            exe.resize(exe.size() - 10);
        const auto slash = exe.rfind('/');
        return slash == std::string::npos ? exe : exe.substr(slash + 1);
    }

    // "grim, obs  wf-recorder" -> contains(exe)
    bool listHas(const std::string& list, const std::string& exe) {
        if (exe.empty())
            return false;
        size_t i = 0;
        while (i < list.size()) {
            const auto start = list.find_first_not_of(", \t", i);
            if (start == std::string::npos)
                break;
            const auto end   = list.find_first_of(", \t", start);
            const auto entry = std::string_view(list).substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (entry == exe)
                return true;
            // a name from /proc/pid/comm is cut to 15 chars (xdg-desktop-portal-hyprland -> xdg-desktop-por)
            if (exe.size() == 15 && entry.size() > 15 && entry.starts_with(exe))
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

    // Hidden windows are hidden from every capture by default. show_to lets chosen
    // clients see them (own screenshots); hide_from, if set, hides only from its list
    // (a client that can't be identified is still covered). A window or layer rule with
    // its own no_screen_share_show_to / no_screen_share_hide_from uses only those.
    //
    // The surfaces this client may see get no_screen_share switched off for the cover
    // pass as well, so paintCovers treats them as normal windows: no cover, occlusion
    // and redraws as for any other window.
    std::vector<SSuppressed> revealFor(Screenshare::CScreenshareFrame* frame, std::unordered_set<uintptr_t>& keys) {
        std::vector<SSuppressed>   out;
        const auto                 globalShow = cfgString(g_cfgShowTo);
        const auto                 globalHide = cfgString(g_cfgHideFrom);
        const bool                 anyGlobal  = hasEntries(globalShow) || hasEntries(globalHide);
        std::optional<std::string> exe; // looked up only when some list applies
        const auto                 client = [&]() -> const std::string& {
            if (!exe) {
                exe = captureClientExe(frame);
                NSC_TRACE("capture client: %s\n", exe->empty() ? "(unknown)" : exe->c_str());
            }
            return *exe;
        };
        const auto consider = [&](uintptr_t key, Desktop::Types::COverridableVar<bool>& var, const SRuleValues& rules) {
            const bool        ruleLists = rules.showTo || rules.hideFrom;
            if (!ruleLists && !anyGlobal)
                return;
            const std::string show = ruleLists ? rules.showTo.value_or("") : globalShow;
            const std::string hide = ruleLists ? rules.hideFrom.value_or("") : globalHide;
            bool              reveal = false;
            if (hasEntries(hide))
                reveal = !client().empty() && !listHas(hide, client());
            else if (hasEntries(show))
                reveal = listHas(show, client());
            if (!reveal)
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
    // client may see (show_to / hide_from) are not held.
    std::vector<SSuppressed> holdUnhidden(const std::unordered_set<uintptr_t>& revealed) {
        std::vector<SSuppressed> out;
        const auto               now  = std::chrono::steady_clock::now();
        const auto               keep = [&](uintptr_t key, Desktop::Types::COverridableVar<bool>& var) {
            if (var.valueOrDefault() || revealed.contains(key))
                return;
            const auto it = g_lastCovers.find(key);
            if (it == g_lastCovers.end() || now - it->second.at >= holdFor(it->second.rules))
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

    using RenderMonitorFn = void (*)(Screenshare::CScreenshareFrame*);

    void hkRenderMonitor(Screenshare::CScreenshareFrame* self) {
        NSC_TRACE("hook: renderMonitor\n");
        const auto original = reinterpret_cast<RenderMonitorFn>(g_hook->m_original);
        // Hyprland's boxes are always switched off and drawn by us: under zoom they land
        // in the wrong place, and they are drawn over everything, including windows that
        // sit on top of a hidden one.
        const auto suppressed = suppressNoScreenShare();
        try {
            original(self);
        } catch (...) {
            restoreNoScreenShare(suppressed);
            throw;
        }
        restoreNoScreenShare(suppressed);
        std::unordered_set<uintptr_t> revealedKeys;
        const auto                    revealed = revealFor(self, revealedKeys);
        const auto                    held     = holdUnhidden(revealedKeys);
        const auto                    restore  = [&] {
            restoreNoScreenShare(held);
            restoreNoScreenShare(revealed);
            g_held.clear();
            g_heldRules.clear();
        };
        try {
            paintCovers(self, true);
        } catch (...) {
            restore();
            throw;
        }
        restore();
    }

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
                                                                "Capture clients (exe names) that see hidden windows as they are, e.g. \"grim, obs\"", Config::STRING{});
        g_cfgHideFrom = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:hide_from",
                                                                "If set, hide only from these capture clients (exe names); everyone else sees everything", Config::STRING{});

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
