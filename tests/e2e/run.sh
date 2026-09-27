#!/usr/bin/env bash
# End-to-end check on a live Hyprland: the plugin loads, a no_screen_share window
# is covered in the capture (grim), video plays, the window rule works, a window
# without the rule is left alone, and unload/load neither leaks nor crashes.
#
# Requires: Hyprland, grim, foot, jq, imagemagick; a seat (seatd or logind).
# Works without a GPU on llvmpipe (tested on a VM with bochs-drm).
#
#   tests/e2e/run.sh <libnoshare-cover.so> <clips directory>
#
# Clips: h264.mp4, av1.mp4, vp9.webm (any, 2 s or longer). Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
MEDIA=$(realpath "${2:?clips directory}")
WORK=$(mktemp -d /tmp/nsc-e2e.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"
magick -size 320x180 xc:'#ff8800' "$WORK/rule.png"
magick -size 320x180 xc:'#00c040' "$WORK/layer.png"
# 0.35 s per frame, 1.05 s cycle (see the GIF check below for the shot interval)
magick -delay 35 -size 64x64 xc:red xc:lime xc:blue -loop 0 "$WORK/anim.gif"

write_config() { # $1: default media, $2: media for the foot-rule rule
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$1", backend = "auto" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    -- the config error overlay (e.g. plugin fields while it is unloaded) hangs
    -- the main thread in renderText on llvmpipe with 0.56.2; it's a Hyprland bug,
    -- reproducible without the plugin; the test doesn't need the overlay
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
hl.window_rule({ match = { class = "cover-rule" }, no_screen_share = true, no_screen_share_cover = "$2" })
-- swaybg wallpaper layer: cover from a layer rule
hl.layer_rule({ match = { namespace = "wallpaper" }, no_screen_share = true, no_screen_share_cover = "$WORK/layer.png" })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
# mean color of a window rectangle by class: "r g b" 0..255
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
diff_rmse() { magick compare -metric RMSE "$WORK/$1.png" "$WORK/$2.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/'; }
is_black() { read -r r g b <<<"$1"; [ "$r" -lt 8 ] && [ "$g" -lt 8 ] && [ "$b" -lt 8 ]; }
hypr_pid() { pgrep -xu "$(id -u)" Hyprland | head -1; }

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    # HYPR_ARGS: extra flags (in a container as root: --i-am-really-stupid)
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
}

open_win() { # class
    foot --app-id "$1" sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$1")" ] && break; done
    sleep 1
}

echo "== start"
write_config "$WORK/cover.png" "$WORK/rule.png"
start_hyprland
[ -n "$(hypr_pid)" ] && pass "Hyprland started" || { fail "Hyprland failed to start"; tail -20 "$WORK/hypr.log"; tail -25 "$(ls -t "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log 2>/dev/null | head -1)" 2>/dev/null; exit 1; }
hctl plugin list | grep -q noshare-cover && pass "plugin loaded" || fail "plugin missing from hyprctl plugin list"

echo "== image"
open_win cover-me
open_win plain
shot still
BOX=$(win_box cover-me); C=$(mean_rgb still "$BOX")
read -r r g b <<<"$C"
[ "$r" -gt 100 ] && [ "$b" -gt 200 ] && pass "no_screen_share window is covered ($C)" || fail "capture does not show the cover: $C"
P=$(mean_rgb still "$(win_box plain)")
is_black "$P" && fail "window without the rule is covered too" || pass "window without the rule is untouched ($P)"

echo "== window rule"
open_win cover-rule
shot rule
C=$(mean_rgb rule "$(win_box cover-rule)"); read -r r g b <<<"$C"
[ "$r" -gt 200 ] && [ "$g" -gt 100 ] && [ "$g" -lt 170 ] && [ "$b" -lt 40 ] && pass "no_screen_share_cover from the rule ($C)" || fail "rule did not apply: $C"

check_video() { # file, label, [seconds between shots]
    write_config "$1" "$WORK/rule.png"; hctl reload >/dev/null; sleep 2
    shot v1; sleep "${3:-1.2}"; shot v2
    local box c d; box=$(win_box cover-me)
    magick "$WORK/v1.png" -crop "$box" +repage "$WORK/v1c.png"; magick "$WORK/v2.png" -crop "$box" +repage "$WORK/v2c.png"
    local c1; c1=$(mean_rgb v1 "$box"); c=$(mean_rgb v2 "$box"); d=$(diff_rmse v1c v2c)
    if is_black "$c1" || is_black "$c"; then fail "$2: capture is black ($c1 / $c)"; tail -3 "$WORK/trace.log"
    elif awk "BEGIN{exit !($d > 0.005)}"; then pass "$2: video is playing (RMSE between frames $d)"
    else fail "$2: frame does not change (RMSE $d)"; fi
}

echo "== layer rule"
if command -v swaybg >/dev/null; then
    swaybg -c '#202020' >/dev/null 2>&1 &
    BGPID=$!
    sleep 2
    shot layer
    # with full-screen tiling there is no window-free spot on screen, so we don't
    # rely on a corner under the windows: the plugin draws the layer first and windows
    # on top, so check the trace and a window-free pixel if there is one; otherwise the trace
    if grep -q "layer wallpaper: cover" "$WORK/trace.log"; then
        pass "cover on the wallpaper layer from the layer rule (per trace)"
    else
        fail "layer rule did not apply"; tail -5 "$WORK/trace.log"
    fi
    kill $BGPID 2>/dev/null
else
    echo "  (no swaybg, skipping)"
fi

echo "== video and GIF"
check_video "$MEDIA/h264.mp4" "H.264 (mp4)"
check_video "$MEDIA/av1.mp4" "AV1 (mp4)"
check_video "$MEDIA/vp9.webm" "VP9 (webm)"
# GIF: 3 frames x 0.35 s. The gap between shots must be longer than a frame and
# shorter than the 1.05 s cycle, otherwise whether the two shots differ depends
# on where the timeline started (1.2 s is only 0.15 s into the next cycle).
check_video "$WORK/anim.gif" "GIF" 0.5

echo "== unload / load"
PID=$(hypr_pid)
rss() { awk '/VmRSS/{print $2}' "/proc/$PID/status"; }
thr() { ls "/proc/$PID/task" | wc -l; }
check_video "$MEDIA/h264.mp4" "before cycles" >/dev/null
R0=$(rss); T0=$(thr)
for i in $(seq 8); do
    hctl plugin unload "$PLUGIN" >/dev/null
    shot un; C=$(mean_rgb un "$(win_box cover-me)")
    is_black "$C" || { fail "after unload #$i the window is not black ($C)"; break; }
    hctl plugin load "$PLUGIN" >/dev/null; hctl reload >/dev/null; sleep 1.5
    shot re; C=$(mean_rgb re "$(win_box cover-me)")
    is_black "$C" && { fail "no cover after load #$i"; break; }
done
[ -n "$(hypr_pid)" ] && pass "Hyprland alive after 8 unload/load cycles" || fail "Hyprland crashed"
R1=$(rss); T1=$(thr)
echo "     RSS: ${R0} -> ${R1} KB, threads: ${T0} -> ${T1}"
[ "$T1" -le "$((T0 + 1))" ] && pass "threads do not pile up" || fail "thread count grew: $T0 -> $T1"
[ "$R1" -le "$((R0 + 60000))" ] && pass "no significant memory growth (+$((R1 - R0)) KB)" || fail "RSS grew by $((R1 - R0)) KB"

echo "== final"
hctl plugin unload "$PLUGIN" >/dev/null
sleep 0.5
[ -n "$(hypr_pid)" ] && pass "final unload is clean" || fail "Hyprland crashed on unload"
pkill -xu "$(id -u)" Hyprland; sleep 1
grep -qiE "Hyprland has crashed|SIGSEGV|SIGABRT|signal 11|core dumped|plugin .* crashed" "$WORK/hypr.log" && fail "Hyprland log shows crashes" || pass "no crashes in Hyprland log"

echo
[ "$FAILS" -eq 0 ] && echo "ALL PASSED ($WORK)" || echo "FAILURES: $FAILS ($WORK)"
exit "$FAILS"
