// The screencast frame: the renderMonitor hook, who captures and what they may see
// (show_to / hide_from), and where cursor zoom puts things.

#include "shim.hpp"

namespace nsc {

    namespace {

        // renderMonitor may be hooked by another plugin (gloview takes it while
        // noshare-cover isn't loaded). In that case don't fail the load, keep retrying
        // the hook: gloview releases the function once it sees noshare-cover after a
        // config reload. The timer is ours and is removed in PLUGIN_EXIT.
        void*               g_hookTarget = nullptr;
        SP<CEventLoopTimer> g_hookRetry;
        constexpr auto      HOOK_RETRY_EVERY = std::chrono::milliseconds(500);
        CFunctionHook*      g_hook           = nullptr;

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
        using ApplyZoomFn = void (*)(Monitor::CMonitorZoomController*, Render::CRenderContext&, CBox&);

        void hkApplyZoom(Monitor::CMonitorZoomController* self, Render::CRenderContext& ctx, CBox& monbox) {
            reinterpret_cast<ApplyZoomFn>(g_zoomHook->m_original)(self, ctx, monbox);
            if (const auto mon = ctx.m_data.pMonitor.lock(); mon && ctx.m_mode == Render::RENDER_MODE_NORMAL)
                recordZoom(mon, monbox);
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

        // comm names already told about (see sees)
        std::unordered_set<std::string> g_commWarned;

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
                if (w && w->m_ruleApplicator && w->mapped())
                    keep(reinterpret_cast<uintptr_t>(w.get()), w->m_ruleApplicator->noScreenShare());
            for (const auto& l : Desktop::layerState()->layers())
                if (l && l->m_ruleApplicator && viewVisible(l))
                    keep(reinterpret_cast<uintptr_t>(l.get()), l->m_ruleApplicator->noScreenShare());
            if (!out.empty())
                NSC_TRACE("holding the cover of %zu surface(s) that just stopped being hidden\n", out.size());
            return out;
        }

        // original: Hyprland's renderMonitor for this frame
        template <class F>
        void renderMonitorWithCovers(Screenshare::CScreenshareFrame* self, Render::CRenderContext& ctx, const F& original) {
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
                std::optional<CSceneSwap> scene;
                const auto                mon = ctx.m_data.pMonitor.lock();
                if (mon && self && self->m_session) {
                    const auto gone = transparentOn(mon, self->m_session->m_captureBox.pos(), client);
                    if (!gone.keys.empty() || gone.closing)
                        left = scene.emplace(ctx, mon, gone.keys, self->m_overlayCursor).drawn();
                }
                if (mon && !mon->resources().expired())
                    image = mon->resources()->getMirrorTexture(); // the scene, if it was swapped in
                // Hyprland's boxes are always switched off and drawn by us: under zoom they land
                // in the wrong place, and they are drawn over everything, including windows that
                // sit on top of a hidden one.
                const auto                          suppressed = suppressNoScreenShare();
                const Hyprutils::Utils::CScopeGuard unsuppress{[&] { restoreNoScreenShare(suppressed); }};
                original();
            }
            paintCovers(ctx, self, client, revealedKeys, left, image);
            if (const auto mon = ctx.m_data.pMonitor.lock(); mon && self && self->m_session)
                cursorOverCovers(ctx, mon, self->m_session->m_captureBox.pos(), self->m_overlayCursor);
        }

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
                g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, region);
                renderMonitorWithCovers(self, ctx, original);
            }
            g_pHyprRenderer->draw(ctx, CTexPassElement::SRenderData{.tex = off->getTexture(), .box = full}, region);
        }

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

    // keep: surfaces (by address) that stay hidden
    std::vector<SSuppressed> suppressNoScreenShare(const std::unordered_set<uintptr_t>& keep) {
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

    void installHooks() {
        void* target = nullptr;
        for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, "renderMonitor")) {
            if (match.demangled.find("CScreenshareFrame::renderMonitor") != std::string::npos) {
                target = match.address;
                break;
            }
        }
        if (!target)
            throw std::runtime_error("noshare-cover: CScreenshareFrame::renderMonitor not found");

        // Optional: without it covers just don't follow cursor zoom.
        for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, "applyZoomTransform")) {
            if (match.demangled.find("CMonitorZoomController::applyZoomTransform") == std::string::npos)
                continue;
            g_zoomHook = HyprlandAPI::createFunctionHook(g_handle, match.address, reinterpret_cast<void*>(&hkApplyZoom));
            if (g_zoomHook && !g_zoomHook->hook()) {
                HyprlandAPI::removeFunctionHook(g_handle, g_zoomHook);
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
    }

    // Before anything else on unload: Hyprland cleans up hooks only after PLUGIN_EXIT, and
    // a screencast frame in between must not land in the unloaded core.
    void removeHooks() {
        stopHookRetry();
        if (g_hook) {
            HyprlandAPI::removeFunctionHook(g_handle, g_hook);
            g_hook = nullptr;
        }
        if (g_zoomHook) {
            HyprlandAPI::removeFunctionHook(g_handle, g_zoomHook);
            g_zoomHook = nullptr;
        }
        g_zoom.clear();
        g_offscreen.reset();
    }

    std::optional<CBox> zoomBox(const PHLMONITOR& mon) {
        const auto it = g_zoom.find(mon->m_id);
        return it == g_zoom.end() ? std::nullopt : std::optional{it->second.box};
    }

} // namespace nsc
