// Capture frames drawn without transparent surfaces, and the software cursor over covers.

#include "shim.hpp"

namespace nsc {

    namespace {

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

    } // namespace

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

    CSceneSwap::CSceneSwap(Render::CRenderContext& ctx, const PHLMONITOR& mon, const std::unordered_set<uintptr_t>& left) {
        const auto res = mon->resources();
        // locked: the mirror holds the lock screen, nothing to leave out
        if (res.expired() || !res->hasMirrorFB() || mon->isMirror() || (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()))
            return;
        const auto mirror = res->mirrorFB();
        const auto size   = mon->m_transformedSize;
        const auto fb     = mirror->m_size == size ? likeMirror(g_sceneFB, "noshare-cover scene", mirror) : nullptr;
        const auto zoom   = zoomBox(mon);
        const auto out    = !fb || !zoom ? fb : likeMirror(g_sceneZoomFB, "noshare-cover zoomed scene", mirror);
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
            g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, full);
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
                g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}}, full);
                g_pHyprRenderer->draw(ctx, CTexPassElement::SRenderData{.tex = fb->getTexture(), .box = *zoom}, full);
            }
        }
        NSC_TRACE("scene: %s drawn without %zu transparent surface(s)\n", mon->m_name.c_str(), left.size());
        m_mirror = mirror;
        m_real   = std::exchange(SFramebufferAccess::texture(*mirror), out->getTexture());
    }

    CSceneSwap::~CSceneSwap() {
        if (m_mirror)
            SFramebufferAccess::texture(*m_mirror) = m_real;
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
        const auto zoom                = zoomBox(mon);
        ctx.m_data.renderModif         = {};
        ctx.m_data.renderModif.modifs  = {{Render::SRenderModifData::RMOD_TYPE_SCALE, float(zoomScale(mon))},
                                          {Render::SRenderModifData::RMOD_TYPE_TRANSLATE, (zoom ? zoom->pos() : Vector2D{}) - capturePos}};
        ctx.m_data.noSimplify          = true;
        pass.render(ctx, g_painted);
    }

    void releaseScene() {
        g_sceneFB.reset();
        g_sceneZoomFB.reset();
    }

} // namespace nsc
