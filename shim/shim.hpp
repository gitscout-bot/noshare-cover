// noshare-cover's C++ side: the shim between Hyprland and the Rust core (src/), for what
// can't be done without Hyprland's C++ ABI. All media, decoding, clock and lifecycle logic
// is in Rust.
//
//   main.cpp     plugin entry points, the public API for other plugins, event listeners
//   config.cpp   config values, window and layer rule effects, what a surface's rules ask for
//   capture.cpp  the screencast frame hook, who captures, show_to / hide_from, cursor zoom
//   scene.cpp    frames without transparent surfaces, the cursor over covers
//   paint.cpp    covers, blur and pixelate, redrawing what is above them
//   closing.cpp  covers that outlive their surface (closing, hold), the repaint pump
//
// What one file uses from another is declared here; the rest stays inside its file.

#pragma once

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
#include "desktop/view/window/Window.hpp"
#include "desktop/view/window/WindowPresentation.hpp"
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
#include "managers/SessionLockManager.hpp"
#include "pointer/PointerManager.hpp"
#include "render/Context.hpp"
#include "render/scene/MonitorScene.hpp"
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

// Hidden: these are the plugin's own symbols, not an API (that is main.cpp's extern "C").
// Only this namespace: Hyprland's inline globals must stay bound to the compositor's copy.
#pragma GCC visibility push(hidden)
namespace nsc {

    inline std::string windowClass(const PHLWINDOW& w) {
        return std::string{w->metadata().appID()};
    }

    // layers and popups, as ScreenshareFrame checks them
    template <class V>
    bool viewVisible(const V& v) {
        return v->mapped() && v->acceptsInput() && v->alphaNonZero();
    }

    // top-left of the client's own geometry inside its surface (popups are relative to it)
    inline Vector2D windowClientGeometryPos(const PHLWINDOW& w) {
        return w->backend().geometry().box.pos();
    }

    // the layout alpha is how a group hides its inactive tabs
    inline float windowFade(const PHLWINDOW& w) {
        return w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) *
            w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_LAYOUT);
    }

    // everything that makes the window translucent as a whole: fade, fullscreen, opacity rules
    inline float windowAlpha(const PHLWINDOW& w) {
        return windowFade(w) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
    }

    inline bool windowPinned(const PHLWINDOW& w) {
        return static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
    }

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

    // What stands in for a hidden surface: no_screen_share_cover (or path_cover) is media, or
    // one of the words "transparent" (left out of the capture as if it wasn't there, see
    // CSceneSwap), "blur [size]" or "pixelate [block size]" (its own picture, blurred or
    // pixelated, see drawEffect; sizes in capture pixels).
    struct SCoverKind {
        enum eKind : uint8_t { MEDIA, TRANSPARENT, BLUR, PIXELATE };
        eKind kind   = MEDIA;
        int   amount = 0;
    };

    // The last matching rule wins, same as in Hyprland itself.
    struct SRuleValues {
        std::optional<std::string> path, speed, loop, hold;
        std::optional<std::string> showTo, hideFrom; // per-rule capture client lists, override the global ones
    };

    // Hyprland's own black boxes land in the wrong place under zoom and are drawn over
    // windows stacked above the hidden one, and can't be erased once drawn. While the
    // original renderMonitor runs, no_screen_share is switched off on the highest
    // priority slot (restored exactly right after), and we draw every box ourselves:
    // covers, or plain black where no cover is set.
    struct SSuppressed {
        Desktop::Types::COverridableVar<bool>* var = nullptr;
        std::optional<bool>                    prev;
    };

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

    // A window as it lands in the capture image.
    struct SWinGeo {
        Vector2D pos, size; // global logical, pos includes the workspace render offset
        CBox     box;       // capture pixels
        int      round         = 0;
        float    roundingPower = 2.F;
        int      layer         = 0; // stacking, see windowGeo
        bool     opaque        = false;
    };

    // Hidden surfaces on mon with a transparent cover that this client doesn't see (after
    // revealFor and holdUnhidden), and whether one of them is still closing.
    struct STransparentOn {
        std::unordered_set<uintptr_t> keys;
        bool                          closing = false;
    };

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
        ~CSceneSwap();
        CSceneSwap(const CSceneSwap&)            = delete;
        CSceneSwap& operator=(const CSceneSwap&) = delete;

        bool        drawn() const {
            return !!m_mirror;
        }

      private:
        SP<Render::IFramebuffer> m_mirror;
        SP<Render::ITexture>     m_real;
    };

    // main.cpp
    extern HANDLE g_handle;
    void          notify(const std::string& msg, float time = 4000);
    // NOSHARE_COVER_DEBUG=<file>: step-by-step trace (for debugging on someone else's machine)
    extern FILE* const g_trace;
    void               drainNotifications();

    // config.cpp
    extern SP<Config::Values::CStringValue> g_cfgPath;
    extern SP<Config::Values::CBoolValue>   g_cfgLoop;
    extern SP<Config::Values::CFloatValue>  g_cfgSpeed;
    extern SP<Config::Values::CStringValue> g_cfgBackend;
    extern SP<Config::Values::CStringValue> g_cfgGpu;
    extern SP<Config::Values::CIntValue>    g_cfgCloseHold;
    extern SP<Config::Values::CStringValue> g_cfgShowTo;
    extern SP<Config::Values::CStringValue> g_cfgHideFrom;
    extern SP<Config::Values::CBoolValue>   g_cfgCursorOverCovers;
    SCoverKind coverKind(std::string_view value);
    SCoverKind coverKind(const SRuleValues& rules);
    bool transparentCover(const SRuleValues& rules);
    SRuleValues ruleValuesFor(const PHLWINDOW& w);
    SRuleValues ruleValuesFor(const PHLLS& l);
    std::chrono::milliseconds holdFor(const SRuleValues& rules);
    void pushSettings();
    std::string cfgString(const SP<Config::Values::CStringValue>& cfg);
    void registerConfig();
    void unregisterConfig();

    // capture.cpp
    void                installHooks();
    void                removeHooks();
    bool beingCaptured(const PHLMONITOR& mon);
    CBox zoomed(const PHLMONITOR& mon, const CBox& b);
    double zoomScale(const PHLMONITOR& mon);
    std::optional<CBox> zoomBox(const PHLMONITOR& mon); // the zoomed monitor image, if zoomed
    std::vector<SSuppressed> suppressNoScreenShare(const std::unordered_set<uintptr_t>& keep = {});
    void restoreNoScreenShare(const std::vector<SSuppressed>& suppressed);

    // scene.cpp
    STransparentOn transparentOn(const PHLMONITOR& mon, const Vector2D& capturePos, const SCaptureClient& client);
    void cursorOverCovers(Render::CRenderContext& ctx, const PHLMONITOR& mon, const Vector2D& capturePos);
    void releaseScene();

    // paint.cpp
    extern CRegion g_painted;
    std::optional<SWinGeo> windowGeo(const PHLWINDOW& w, const PHLMONITOR& mon, const Vector2D& capturePos);
    void paintCover(Render::CRenderContext& ctx, const SP<Render::ITexture>& image, const std::string& what, const SRuleValues& rules, const CBox& box, const Vector2D& capturePos, int round,
                    float roundingPower, const CRegion& clip = {});
    void paintCovers(Render::CRenderContext& ctx, Screenshare::CScreenshareFrame* frame, const SCaptureClient& client, const std::unordered_set<uintptr_t>& revealed, bool left,
                     const SP<Render::ITexture>& image);
    void releasePaint();

    // closing.cpp
    extern std::vector<SAnimBox>                      g_animBoxes;
    extern std::unordered_map<uintptr_t, SLastCover>  g_lastCovers;
    extern std::unordered_set<uintptr_t>              g_held;
    extern std::unordered_map<uintptr_t, SRuleValues> g_heldRules;
    extern std::vector<SClosing>                      g_closing;
    void trackClosed(const PHLMONITOR& mon, const std::unordered_set<uintptr_t>& seen, std::chrono::steady_clock::time_point now);
    CRegion paintClosing(Render::CRenderContext& ctx, const PHLMONITOR& mon, const Vector2D& capturePos, std::chrono::steady_clock::time_point now, const SCaptureClient& client,
                         bool left, const SP<Render::ITexture>& image);
    void startPump();
    void onWindowClose(const PHLWINDOW& w);
    void onLayerClose(const PHLLS& l);
    void prewarmWindow(const PHLWINDOW& w);
    void prewarmLayer(const PHLLS& l);
    void releaseClosing();

} // namespace nsc
#pragma GCC visibility pop

#define NSC_TRACE(...) \
    do { \
        if (::nsc::g_trace) { \
            std::fprintf(::nsc::g_trace, "[noshare-cover] " __VA_ARGS__); \
            std::fflush(::nsc::g_trace); \
        } \
    } while (0)
