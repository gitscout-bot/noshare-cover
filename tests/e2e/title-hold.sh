#!/usr/bin/env bash
# The close hold after a window stops being hidden: a window hidden by a rule that matches its title keeps the cover for a moment
# after the title stops matching (browsers change the title before they repaint, so the old
# page would reach the stream). grim is the capture client.
#
#   tests/e2e/title-hold.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, jq, imagemagick; a seat. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-hold.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"

write_config() { # $1: close_hold, $2: extra rule fields
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", close_hold = $1 } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "tabs", title = [[.*SECRET.*]] }, no_screen_share = true ${2:+, $2} })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }
title() { hctl clients -j | jq -r '.[] | select(.class == "tabs") | .title' | head -1; }

# a foot window titled "SECRET page" until $WORK/switch-N appears, then "normal page"
open_tab() { # $1: N
    rm -f "$WORK/switch-$1"
    foot --app-id tabs sh -c "printf '\033]2;SECRET page\007'; while [ ! -f $WORK/switch-$1 ]; do sleep 0.02; done; printf '\033]2;normal page\007'; while :; do date; sleep 0.2; done" >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ "$(title)" = "SECRET page" ] && break; done
    sleep 0.5
}

# $1: close_hold, $2: rule fields, $3: expect right after the switch (cover|content), $4: label
run_case() {
    write_config "$1" "$2"
    if [ -z "${STARTED:-}" ]; then
        pkill -xu "$(id -u)" Hyprland; sleep 0.5
        Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
        for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
        export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
        sleep 2
        STARTED=1
    else
        hctl reload >/dev/null; sleep 1.5
    fi
    N=$((${N:-0} + 1))
    open_tab "$N"
    local box c
    box=$(win_box tabs)
    grim "$WORK/before.png"
    c=$(mean_rgb before "$box"); is_cover "$c" && pass "$4: covered while the title matches ($c)" || fail "$4: not covered while the title matches ($c)"
    touch "$WORK/switch-$N"
    for _ in $(seq 50); do [ "$(title)" = "normal page" ] && break; sleep 0.01; done
    grim "$WORK/after.png"
    c=$(mean_rgb after "$box")
    if [ "$3" = cover ]; then
        is_cover "$c" && pass "$4: still covered right after the title changed ($c)" || fail "$4: leaked right after the title changed ($c)"
    else
        is_cover "$c" && fail "$4: expected no hold, still covered ($c)" || pass "$4: shown right away without a hold ($c)"
    fi
    sleep 2
    grim "$WORK/later.png"
    c=$(mean_rgb later "$box"); is_cover "$c" && fail "$4: still covered 2 s later ($c)" || pass "$4: shown after the hold ($c)"
    pkill -f "switch-$N" 2>/dev/null
    for _ in $(seq 20); do [ -z "$(win_box tabs)" ] && break; sleep 0.1; done
}

echo "== hold after a window stops being hidden (client: grim)"
run_case 1000 ""                                  cover   "close_hold = 1000"
run_case 0    ""                                  content "close_hold = 0"
run_case 0    "no_screen_share_cover_hold = 1000" cover   "rule no_screen_share_cover_hold = 1000"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
