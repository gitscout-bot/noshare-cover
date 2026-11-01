#!/usr/bin/env bash
# no_screen_share_cover = "pixelate N" / "blur N": the hidden window is shown pixelated or
# blurred. Checked against a capture that shows the window as it is (show_to = "grim"): the
# average colors stay, the detail goes (blocks for pixelate). Then cursor_over_covers: a
# software cursor over a cover stays in the capture, and is gone with the option off.
#
#   tests/e2e/effects.sh <libnoshare-cover.so>
#
# Requires: Hyprland 0.57+, grim, foot, jq, imagemagick; a seat. The cursor part needs a
# software cursor (a nested Hyprland has one). Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-fx.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 xc:'#ff00ff' "$WORK/cover.png"

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
rmse() { magick compare -metric RMSE "$WORK/$1.png" "$WORK/$2.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/'; }
below() { awk "BEGIN{exit !($1 < $2)}"; }

start() { # $1: the hidden window's cover, $2: its content (mosaic/plain), $3: cursor zoom
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    cursor = { zoom_factor = ${3:-1} },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true, disable_watchdog_warning = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "fx-me" }, no_screen_share = true, no_screen_share_cover = "$1" })
LUA
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
    foot --app-id plain sh -c 'printf "\033[?25l"; sleep 1000' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box plain)" ] && break; done
    # a mosaic of colored cells: detail an effect can't keep
    local mosaic='i=0; while [ $i -lt 6000 ]; do printf "\033[48;5;%dm  " $(( (i * 7919) % 216 + 16 )); i=$((i + 1)); done;'
    [ "${2:-mosaic}" = plain ] && mosaic=
    foot --app-id fx-me sh -c "printf '\033[?25l'; $mosaic sleep 1000" >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box fx-me)" ] && break; done
    sleep 1.5
    BOX=$(win_box fx-me)
    IFS='x+' read -r W H _ _ <<<"$BOX"
}

# $1 cover word (pixelate/blur), $2 amount
check_effect() {
    local a=$1-$2
    start "$1 $2"
    hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "grim" } } })' >/dev/null
    shot "$a-real"
    hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "" } } })' >/dev/null
    shot "$a"
    magick "$WORK/$a-real.png" -crop "$BOX" +repage "$WORK/$a-real.png"
    magick "$WORK/$a.png" -crop "$BOX" +repage "$WORK/$a.png"
    grep -aq "window fx-me: $1 $2" "$WORK/trace.log" || fail "$1: not in the trace"
    local d
    d=$(rmse "$a" "$a-real")
    below 0.1 "$d" && pass "$1: the window itself isn't shown (RMSE $d to it)" || fail "$1: too close to the window itself (RMSE $d)"
    # what it keeps: the window's average colors, over cells of one block, or of a few blur radii
    local cell=$2; [ "$1" = blur ] && cell=$(($2 * 4))
    local gw=$(( (W + cell / 2) / cell )) gh=$(( (H + cell / 2) / cell ))
    magick "$WORK/$a.png" -filter box -resize "${gw}x${gh}!" "$WORK/$a-avg.png"
    magick "$WORK/$a-real.png" -filter box -resize "${gw}x${gh}!" "$WORK/$a-real-avg.png"
    d=$(rmse "$a-avg" "$a-real-avg")
    local most=0.02; [ "$1" = blur ] && most=0.04
    below "$d" "$most" && pass "$1: keeps the window's average colors (RMSE $d)" || fail "$1: average colors differ from the window's (RMSE $d)"
    # what it drops: pixelate is made of those blocks, blur has no detail left
    if [ "$1" = pixelate ]; then
        magick "$WORK/$a.png" -filter box -resize "${gw}x${gh}!" -sample "${W}x${H}!" "$WORK/$a-blocks.png"
        d=$(rmse "$a" "$a-blocks")
        below "$d" 0.01 && pass "$1: made of ${2}px blocks (RMSE $d)" || fail "$1: not made of ${2}px blocks (RMSE $d, $WORK/$a.png)"
    else
        magick "$WORK/$a.png" -blur 0x3 "$WORK/$a-soft.png"
        d=$(rmse "$a" "$a-soft")
        below "$d" 0.02 && pass "$1: no detail left (RMSE $d to itself blurred more)" || fail "$1: still has detail (RMSE $d, $WORK/$a.png)"
    fi
}

echo "== pixelate"
check_effect pixelate 32
echo "== blur"
check_effect blur 24

# The cursor over a cover must be where and as Hyprland draws it: compared with a capture
# that shows the window as it is (the cursor doesn't move in between, so neither does zoom).
check_cursor() { # $1: cursor zoom
    local a=cursor-zoom$1
    start "$WORK/cover.png" plain "$1"
    # the hidden window alone, under the cursor where it starts (the middle). The zoom
    # doesn't follow a cursor moved by a dispatcher, so it's left there.
    kill "$(hctl clients -j | jq -r '.[] | select(.class == "plain") | .pid')"
    sleep 1
    hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "grim" } } })' >/dev/null
    shot "$a-real"
    hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "" } } })' >/dev/null
    shot "$a"
    hctl eval 'hl.config({ plugin = { no_screen_share_cover = { cursor_over_covers = false } } })' >/dev/null
    shot "$a-off"
    # the cursor's pixels: whatever isn't the window's background, or the cover's color
    magick "$WORK/$a-real.png" -fuzz 6% -fill black -opaque '#242424' -fill white +opaque black "$WORK/$a-real-mask.png"
    magick "$WORK/$a.png" -fuzz 6% -fill black -opaque '#ff00ff' -opaque '#242424' -fill white +opaque black "$WORK/$a-mask.png"
    magick "$WORK/$a-off.png" -fuzz 6% -fill black -opaque '#ff00ff' -opaque '#242424' -fill white +opaque black "$WORK/$a-off-mask.png"
    # where and how big (its shadow blends differently into the two backgrounds)
    local real got off
    real=$(magick "$WORK/$a-real-mask.png" -trim -format '%wx%h%O' info: 2>/dev/null)
    got=$(magick "$WORK/$a-mask.png" -trim -format '%wx%h%O' info: 2>/dev/null)
    off=$(magick "$WORK/$a-off-mask.png" -format '%[fx:int(mean*w*h+0.5)]' info:)
    case "$real" in 1x1*|"") fail "zoom $1: no cursor in the capture without the cover ($WORK/$a-real.png)" ;; esac
    # same place; the size may differ by the edge pixels under zoom (Hyprland scales the
    # whole frame, the cursor here is drawn scaled)
    local rw rh rx ry gw gh gx gy
    IFS='x+' read -r rw rh rx ry <<<"$real"
    IFS='x+' read -r gw gh gx gy <<<"$got"
    if [ "$gx,$gy" = "$rx,$ry" ] && [ $((gw - rw)) -le 2 ] && [ $((rw - gw)) -le 2 ] && [ $((gh - rh)) -le 2 ] && [ $((rh - gh)) -le 2 ]; then
        pass "zoom $1: the cursor stays over the cover, where and as Hyprland draws it ($got, Hyprland $real)"
    else
        fail "zoom $1: the cursor over the cover is $got, Hyprland draws it at $real ($WORK/$a.png vs $a-real.png)"
    fi
    [ "$off" -eq 0 ] && pass "zoom $1: cursor_over_covers = false, the cover hides it" || fail "zoom $1: cursor_over_covers = false still shows $off px ($WORK/$a-off.png)"
}

echo "== cursor over covers"
check_cursor 1
check_cursor 2

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
