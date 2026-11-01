// Covers that outlive what they cover: closing windows and layers, surfaces that just
// stopped being hidden; and the pump that keeps animated and closing covers repainted.

#include "shim.hpp"

namespace nsc {

    namespace {

        SP<CEventLoopTimer> g_pump;
        constexpr auto      PUMP_EVERY       = std::chrono::milliseconds(16);
        constexpr auto      FADE_BIND_WINDOW = std::chrono::milliseconds(150);

        // Something last covered longer ago than this gets a closing cover only if its
        // fade-out is found (see trackClosed).
        constexpr auto STALE_COVER = std::chrono::seconds(1);

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

        void stopPump() {
            if (!g_pump)
                return;
            g_pump->cancel();
            if (g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(g_pump);
            g_pump.reset();
        }

    } // namespace

    std::vector<SAnimBox>                     g_animBoxes;
    std::unordered_map<uintptr_t, SLastCover> g_lastCovers;
    std::vector<SClosing>                     g_closing;
    // Surfaces kept covered for a moment after they stopped being hidden (see holdUnhidden),
    // with the rules they had while hidden. Valid only during one capture frame.
    std::unordered_set<uintptr_t>              g_held;
    std::unordered_map<uintptr_t, SRuleValues> g_heldRules;

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
                gone         = !w || !w->mapped();
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

    void onWindowClose(const PHLWINDOW& w) {
        if (!w || !w->m_ruleApplicator || !w->m_ruleApplicator->noScreenShare().valueOrDefault())
            return;
        const auto* ws           = w->m_workspace.get();
        const auto  renderOffset = ws && !windowPinned(w) && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        const auto  pos          = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        const auto  size         = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        startClosing(false, reinterpret_cast<uintptr_t>(w.get()), w->m_monitor.lock(), CBox{pos, size}, ruleValuesFor(w), w->presentation().rounding(), w->presentation().roundingPower());
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
    CRegion paintClosing(Render::CRenderContext& ctx, const PHLMONITOR& mon, const Vector2D& capturePos, std::chrono::steady_clock::time_point now, const SCaptureClient& client,
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

    void startPump() {
        if (!g_pEventLoopManager)
            return;
        if (!g_pump) {
            g_pump = makeShared<CEventLoopTimer>(PUMP_EVERY, pumpTick, nullptr);
            g_pEventLoopManager->addTimer(g_pump);
        } else if (!g_pump->armed())
            g_pump->updateTimeout(PUMP_EVERY);
    }

    void releaseClosing() {
        stopPump();
        g_lastCovers.clear();
        g_closing.clear();
    }

} // namespace nsc
