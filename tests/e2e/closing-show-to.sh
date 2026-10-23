#!/usr/bin/env bash
# Closing covers and show_to: an allowed client (a copy of grim named grim-share) never
# gets the cover of a hidden window that closed, while a client that isn't allowed (plain
# grim, single shots, no stream) still gets it during the close animation.
#
#   tests/e2e/closing-show-to.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, jq, imagemagick; a seat.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-close.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"
cp "$(readlink -f "$(type -P grim)")" "$WORK/grim-share"

# animations on: a closed window fades out, and that fade-out is what the closing cover hides
cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", show_to = "grim-share", close_hold = 300 } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = true },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true, float = true, size = "600 400" })
LUA

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$1" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }
# foot window, waits until it's mapped; sets FOOT, W H X Y and CENTER (40x40 in the middle)
open_window() {
    foot --app-id cover-me sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    FOOT=$!
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box cover-me)" ] && break; done
    sleep 1
    read -r W H X Y <<<"$(sed 's/[x+]/ /g' <<<"$(win_box cover-me)")"
    CENTER="40x40+$((X + W / 2 - 20))+$((Y + H / 2 - 20))"
}

pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 2

echo "== allowed client after a hidden window closed"
for i in 1 2 3; do
    open_window
    "$WORK/grim-share" "$WORK/a$i-before.png"
    kill "$FOOT"
    sleep 1   # fade-out over, still inside close_hold if the hold started late
    "$WORK/grim-share" "$WORK/a$i-after.png"
    C=$(mean_rgb "$WORK/a$i-before.png" "$CENTER")
    is_cover "$C" && fail "run $i: grim-share got the cover of the open window ($C)" || pass "run $i: grim-share sees the open window ($C)"
    C=$(mean_rgb "$WORK/a$i-after.png" "$CENTER")
    is_cover "$C" && fail "run $i: grim-share got the cover of the closed window ($C)" || pass "run $i: grim-share has no closing cover ($C)"
    sleep 1
done

echo "== client not in show_to during the close animation, no stream"
for i in 1 2 3; do
    open_window
    grim "$WORK/b$i-before.png"
    sleep 0.5
    kill "$FOOT"
    sleep 0.05
    grim "$WORK/b$i-fade.png"
    C=$(mean_rgb "$WORK/b$i-before.png" "$CENTER")
    is_cover "$C" && pass "run $i: grim gets the cover ($C)" || fail "run $i: grim sees the open window ($C)"
    C=$(mean_rgb "$WORK/b$i-fade.png" "$CENTER")
    is_cover "$C" && pass "run $i: grim gets the cover during the fade-out ($C)" || fail "run $i: the closing window reached grim ($C)"
    sleep 1
done

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
