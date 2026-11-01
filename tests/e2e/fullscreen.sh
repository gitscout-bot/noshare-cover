#!/usr/bin/env bash
# A window that goes fullscreen is drawn over the other windows of its workspace, which
# fade out under it (fadeIn). A hidden window fading out under a fullscreen one must not
# get its cover drawn over the fullscreen window: the capture during the fade must look
# like the capture once it's over.
#
#   tests/e2e/fullscreen.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, jq, imagemagick; a seat. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-fs.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 xc:'#ff00ff' "$WORK/cover.png"
cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = true },
    -- the start-hyprland warning's timer bar would differ between the two shots
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true, disable_watchdog_warning = true },
    debug = { suppress_errors = true },
})
-- only the fade under fullscreen is animated, and slowly (3 s)
hl.curve("linear", { type = "bezier", points = { {0, 0}, {1, 1} } })
hl.animation({ leaf = "fadeIn", enabled = true, speed = 30, bezier = "linear" })
hl.animation({ leaf = "fadeOut", enabled = false })
hl.animation({ leaf = "windowsIn", enabled = false })
hl.animation({ leaf = "windowsMove", enabled = false })
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
LUA

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
crop_rmse() { magick "$WORK/$1.png" -crop "$3" +repage "$WORK/a.png"; magick "$WORK/$2.png" -crop "$3" +repage "$WORK/b.png"
    magick compare -metric RMSE "$WORK/a.png" "$WORK/b.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/'; }

pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 2
for c in plain cover-me; do
    foot --app-id "$c" sh -c 'printf "\033[?25l"; for i in $(seq 40); do printf " line %d of some text to look at\n" $i; done; sleep 1000' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$c")" ] && break; done
    sleep 1
done
HIDDEN=$(win_box cover-me)

hctl dispatch 'hl.dsp.focus({ window = "class:plain" })' >/dev/null
hctl dispatch 'hl.dsp.window.fullscreen()' >/dev/null
sleep 0.6
shot during
sleep 4
shot after
[ "$(win_box plain)" = "$(hctl monitors -j | jq -r '.[0] | "\(.width)x\(.height)+0+0"')" ] || fail "plain didn't go fullscreen ($(win_box plain))"
grep -aq "window cover-me: fully under other windows" "$WORK/trace.log" \
    || fail "the hidden window wasn't seen fading out under the fullscreen one (trace.log)"
d=$(crop_rmse during after "$HIDDEN")
awk "BEGIN{exit !($d < 0.01)}" && pass "no cover over the fullscreen window while the hidden one fades out (RMSE $d)" \
    || fail "the cover is drawn over the fullscreen window during the fade (RMSE $d, $WORK/during.png vs after.png)"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
