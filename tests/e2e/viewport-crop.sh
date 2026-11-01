#!/usr/bin/env bash
# A translucent window over a hidden one is drawn again over the cover. Chromium keeps an
# oversized buffer and shows a wp_viewporter crop of it; drawn whole, the page shrank into
# a corner of the window and blur filled the rest. The browser in the capture must look
# like Hyprland's own rendering of it (taken once the hidden window is closed).
#
#   tests/e2e/viewport-crop.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, chromium, jq, imagemagick; a seat. Skipped without chromium.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
command -v chromium >/dev/null || { echo "no chromium, skipping"; exit 0; }
WORK=$(mktemp -d /tmp/nsc-vp.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 pattern:verticalbricks -fill red -opaque black -fill '#2040ff' -opaque white "$WORK/cover.png"
cat > "$WORK/page.html" <<HTML
<html><body style="margin:0"><div style="height:100vh;background:repeating-linear-gradient(45deg,#fff 0 40px,#dfe8ff 40px 80px)"></div></body></html>
HTML
# a browser-like setup: nearly opaque (0.99 counts as translucent), blur on, ignore_opacity
cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = true, ignore_opacity = true } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
hl.window_rule({ match = { class = "^(chromium)$" }, opacity = "0.99 0.99" })
LUA

hctl() { hyprctl -i 0 "$@"; }
box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0]) \(.size[1]) \(.at[0]) \(.at[1])"' | head -1; }

pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 1
foot --app-id cover-me sh -c 'sleep 600' >/dev/null 2>&1 &
for _ in $(seq 40); do sleep 0.25; [ -n "$(box cover-me)" ] && break; done
chromium --ozone-platform=wayland --user-data-dir="$WORK/chromium" --no-first-run --no-default-browser-check \
    --password-store=basic "file://$WORK/page.html" > "$WORK/chromium.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; [ -n "$(box chromium)" ] && break; done
sleep 3
# float it over the hidden window and shrink it: Chromium keeps the bigger buffer and crops it
hctl dispatch 'hl.dsp.focus({ window = "class:chromium" })' >/dev/null
hctl dispatch 'hl.dsp.window.float({ action = "toggle" })' >/dev/null
hctl dispatch 'hl.dsp.window.resize({ x = 700, y = 800 })' >/dev/null
hctl dispatch 'hl.dsp.window.move({ x = 300, y = 120 })' >/dev/null
sleep 2

read -r W H X Y <<<"$(box chromium)"
INNER="$((W - 40))x$((H - 40))+$((X + 20))+$((Y + 20))"
grim "$WORK/covered.png"
kill "$(hctl clients -j | jq -r '.[] | select(.class == "cover-me") | .pid')"
sleep 1.5
grim "$WORK/real.png"
magick "$WORK/covered.png" -crop "$INNER" +repage "$WORK/a.png"
magick "$WORK/real.png" -crop "$INNER" +repage "$WORK/b.png"
D=$(magick compare -metric RMSE "$WORK/a.png" "$WORK/b.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/')
awk "BEGIN{exit !($D < 0.03)}" && pass "browser over the cover looks as Hyprland draws it (RMSE $D)" \
    || fail "browser over the cover differs from Hyprland's rendering (RMSE $D, $WORK/covered.png vs real.png)"

pkill -f "user-data-dir=$WORK/chromium" 2>/dev/null; sleep 0.5
pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "ALL PASSED ($WORK)" || echo "$FAILS FAILED ($WORK)"
exit "$FAILS"
