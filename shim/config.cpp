// Config values, window and layer rule effects, and what the rules of a surface ask for.

#include "shim.hpp"

namespace nsc {

    namespace {

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

        template <typename T, typename... Args>
        SP<T> makeValue(const char* name, const char* desc, Args&&... def) {
            auto v = Config::Values::makeConfigValue<T>(name, desc, std::forward<Args>(def)...);
            if (!v || !HyprlandAPI::addConfigValueV2(g_handle, v))
                notify(std::string("noshare-cover: failed to register config value ") + name, 5000);
            return v;
        }

    } // namespace

    SP<Config::Values::CStringValue> g_cfgPath;
    SP<Config::Values::CBoolValue>   g_cfgLoop;
    SP<Config::Values::CFloatValue>  g_cfgSpeed;
    SP<Config::Values::CStringValue> g_cfgBackend;
    SP<Config::Values::CStringValue> g_cfgGpu;
    SP<Config::Values::CIntValue>    g_cfgCloseHold;
    SP<Config::Values::CStringValue> g_cfgShowTo;
    SP<Config::Values::CStringValue> g_cfgHideFrom;
    SP<Config::Values::CBoolValue>   g_cfgCursorOverCovers;

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

    }

    SCoverKind coverKind(const SRuleValues& rules) {
        return coverKind(rules.path ? *rules.path : g_cfgPath ? g_cfgPath->value() : std::string{});
    }

    bool transparentCover(const SRuleValues& rules) {
        return coverKind(rules).kind == SCoverKind::TRANSPARENT;
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

    std::chrono::milliseconds holdFor(const SRuleValues& rules) {
        long long ms = g_cfgCloseHold ? g_cfgCloseHold->value() : 0;
        if (rules.hold) {
            try {
                ms = std::stoll(*rules.hold);
            } catch (...) {}
        }
        return std::chrono::milliseconds(std::clamp<long long>(ms, 0, 60000));
    }

    std::string cfgString(const SP<Config::Values::CStringValue>& cfg) {
        return cfg ? cfg->value() : std::string{};
    }

    void registerConfig() {
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
    }

    void unregisterConfig() {
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
    }

} // namespace nsc
