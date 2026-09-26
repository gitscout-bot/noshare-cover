#!/usr/bin/env bash
# show_to / hide_from: which capture clients get the cover, globally and per window or
# layer rule. grim is the capture client here (it captures the output directly), the cover
# is a magenta-cyan gradient. Two hidden windows (cover-me, cover-two) and a hidden
# wallpaper layer (swaybg), seen in the gap around the tiled windows.
#
#   tests/e2e/capture-filter.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, swaybg, jq, imagemagick; a seat. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-filter.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"

# $1: global show_to, $2: global hide_from, $3: extra fields for the cover-two rule,
# $4: extra fields for the wallpaper layer rule, "off" = wallpaper not hidden (a hidden
# background layer is covered over the windows on top of it, see README)
write_config() {
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", show_to = "$1", hide_from = "$2" } },
    general = { gaps_in = 0, gaps_out = 60, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
hl.window_rule({ match = { class = "cover-two" }, no_screen_share = true ${3:+, $3} })
$( [ "$4" = off ] || echo "hl.layer_rule({ match = { namespace = \"wallpaper\" }, no_screen_share = true ${4:+, $4} })" )
LUA
}

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }
GAP="40x40+10+10" # inside gaps_out, only the wallpaper is there

open_win() {
    foot --app-id "$1" sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$1")" ] && break; done
}

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
    swaybg -c '#206020' >/dev/null 2>&1 &
    open_win cover-me
    open_win cover-two
    sleep 1
}

expect() { # what (cover|content), measured color, label
    if [ "$1" = cover ]; then
        is_cover "$2" && pass "$3: covered ($2)" || fail "$3: expected the cover, got $2"
    else
        is_cover "$2" && fail "$3: expected it as it is, got the cover ($2)" || pass "$3: as it is ($2)"
    fi
}

# global show_to, global hide_from, cover-two rule, layer rule, expect cover-me, cover-two, wallpaper, label
check() {
    write_config "$1" "$2" "$3" "$4"
    if [ -z "${STARTED:-}" ]; then start_hyprland; STARTED=1; else hctl reload >/dev/null; sleep 1.5; fi
    grim "$WORK/shot.png"
    expect "$5" "$(mean_rgb shot "$(win_box cover-me)")" "$8: cover-me"
    expect "$6" "$(mean_rgb shot "$(win_box cover-two)")" "$8: cover-two"
    expect "$7" "$(mean_rgb shot "$GAP")" "$8: wallpaper layer"
}

echo "== capture filter, global lists (client: grim)"
check ""                "" "" "" cover   cover   cover   "no lists"
check "grim"            "" "" "" content content content "show_to = grim"
check "wf-recorder obs" "" "" "" cover   cover   cover   "show_to without grim"
check ""                "grim" "" "" cover   cover   cover   "hide_from = grim"
check ""                "xdg-desktop-portal-hyprland, wf-recorder" "" "" content content content "hide_from without grim"
check " , "             "" "" "" cover   cover   cover   "show_to of separators only"

echo "== per rule lists"
check "" "" 'no_screen_share_show_to = "grim"' off cover content content "rule show_to on cover-two only"
check "grim" "" 'no_screen_share_show_to = "obs"' "" content cover content "rule show_to overrides the global one"
check "" "" 'no_screen_share_hide_from = "obs"' off cover content content "rule hide_from without grim"
check "grim" "" 'no_screen_share_hide_from = "grim"' "" content cover content "rule hide_from = grim beats global show_to"
check "" "" "" 'no_screen_share_show_to = "grim"' cover cover content "layer rule show_to"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
