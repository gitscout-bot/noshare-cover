// Plugin entry points, the public API for other plugins, and Hyprland's events.

#include "shim.hpp"

namespace nsc {

    namespace {

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
            removeHooks();
            releaseClosing();
            releaseScene();
            // API clients (gloview) drop our pointers and may take over renderMonitor.
            nsc_api_notify_gone();
            releasePaint();
            nsc_shutdown(); // stops and joins decode threads, clears extra rects
            unregisterConfig();
            g_handle = nullptr;
        }

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

            registerConfig();

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

            // last: from here on screencast frames go through the plugin
            installHooks();
            return info;
        }

    } // namespace

    HANDLE         g_handle = nullptr;

    void notify(const std::string& msg, float time) {
        if (g_handle)
            HyprlandAPI::addNotification(g_handle, msg, CHyprColor{1.F, 0.2F, 0.2F, 1.F}, time);
    }

    // NOSHARE_COVER_DEBUG=<file>: step-by-step trace (for debugging on someone else's machine)
    FILE* const g_trace = [] {
        const char* v = std::getenv("NOSHARE_COVER_DEBUG");
        return v && *v ? std::fopen(v, "a") : nullptr;
    }();

    void drainNotifications() {
        std::array<char, 512> buf{};
        while (nsc_take_notification(buf.data(), buf.size()) > 0)
            notify(buf.data());
    }

} // namespace nsc

using namespace nsc;

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

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

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
