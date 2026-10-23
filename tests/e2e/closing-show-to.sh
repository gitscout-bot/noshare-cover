#!/usr/bin/env bash
# show_to and the covers that outlive a hidden window: closing covers and the hold after a
# window stops being hidden. An allowed client (a copy of grim named grim-share) never gets
# them, everyone else still does, and an allowed client's shot doesn't cost the others the
# hold. Plain grim is the client that isn't allowed.
#
#   tests/e2e/closing-show-to.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, wf-recorder, foot, jq, imagemagick; a seat.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-close.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"
# The grim ELF to copy. nixpkgs wraps many programs in a shell script (wrapProgram): a copy of
# the script would run .grim-wrapped, and the capture client wouldn't be called grim-share.
grim_elf() {
    local p n
    p=$(readlink -f "$(type -P grim)") || return 1
    for n in 1 2 3; do
        [ "$(head -c4 "$p" 2>/dev/null)" = $'\x7fELF' ] && { echo "$p"; return 0; }
        p="$(dirname "$p")/.$(basename "$p")-wrapped"
    done
    echo "grim ($(type -P grim)) is not an ELF binary and no .grim-wrapped ELF was found next to it" >&2
    return 1
}
GRIM_ELF=$(grim_elf) || exit 1
cp "$GRIM_ELF" "$WORK/grim-share"
mkdir -p "$WORK/elsewhere"
cp "$WORK/grim-share" "$WORK/elsewhere/grim-share"
ln -s "$WORK" "$WORK-link" # like /run/current-system/sw/bin pointing into the Nix store

# Closing is slowed down (3 s) so a shot surely lands in the close animation.
cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", show_to = "grim-share" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = true },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.animation({ leaf = "windowsIn", enabled = false })
hl.animation({ leaf = "fadeIn", enabled = false })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 30, bezier = "default", style = "popin 50%" })
hl.animation({ leaf = "fadeOut", enabled = true, speed = 30, bezier = "default" })
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true, float = true, size = "600 400" })
hl.window_rule({ match = { class = "tabs", title = [[.*SECRET.*]] }, no_screen_share = true, no_screen_share_cover_hold = 3000 })
hl.window_rule({ match = { class = "by-path" }, no_screen_share = true, no_screen_share_show_to = "$WORK/grim-share" })
hl.window_rule({ match = { class = "by-link" }, no_screen_share = true, no_screen_share_show_to = "$WORK-link/grim-share" })
LUA

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }
title() { hctl clients -j | jq -r '.[] | select(.class == "tabs") | .title' | head -1; }
# 40x40 in the middle of a "WxH+X+Y" box: a closing window shrinks towards its middle
center() { local w h x y; read -r w h x y <<<"$(sed 's/[x+]/ /g' <<<"$1")"; echo "40x40+$((x + w / 2 - 20))+$((y + h / 2 - 20))"; }
# $1: client (grim or a path), $2: shot name, $3: box, $4: cover|content expected, $5: label
check() {
    local name=$2 c
    "$1" "$WORK/$name.png"
    c=$(mean_rgb "$name" "$(center "$3")")
    if [ "$4" = cover ]; then
        is_cover "$c" && pass "$5 ($c)" || fail "$5: expected the cover ($c)"
    else
        is_cover "$c" && fail "$5: expected the window, got the cover ($c)" || pass "$5 ($c)"
    fi
}
open_win() { # $1: class; sets PID and BOX
    foot --app-id "$1" sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    PID=$!
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$1")" ] && break; done
    sleep 1
    BOX=$(win_box "$1")
}
close_win() { # $1: class; returns once Hyprland has unmapped it (its fade-out runs)
    kill "$PID"
    for _ in $(seq 100); do [ -z "$(win_box "$1")" ] && break; sleep 0.01; done
}
settle() { # let a closing cover expire and the fade-out finish before the next case
    sleep 4
}

pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 2

# The screencopy session counts as sharing for a moment after grim exits, so a close 0.5 s
# after the shot still goes through the close event; 3 s after, nothing is shared any more and
# only the entry from the last shot can bring the cover. This runs first: after a stream the
# output counts as shared for longer.
echo "== closing cover without a stream: grim keeps the cover"
for wait in 0.5 3; do
    for i in 1 2 3; do
        open_win cover-me
        check grim "b$wait-$i-open" "$BOX" cover "close after $wait s, run $i: grim while open"
        sleep "$wait"
        close_win cover-me
        sleep 0.1
        check grim "b$wait-$i-fade" "$BOX" cover "close after $wait s, run $i: grim during the close"
        settle
    done
done

echo "== closing cover during a stream (wf-recorder): grim-share sees the fade-out"
for i in 1 2 3; do
    open_win cover-me
    wf-recorder -f "$WORK/a$i.mkv" -c libx264 -r 10 >/dev/null 2>&1 &
    REC=$!
    sleep 1.5
    close_win cover-me
    sleep 0.2
    check "$WORK/grim-share" "a$i-share" "$BOX" content "run $i: grim-share during the close"
    check grim "a$i-grim" "$BOX" cover "run $i: grim during the close"
    kill -INT "$REC"; wait "$REC" 2>/dev/null
    sleep 0.6 # Hyprland 0.56.2 can crash on exit right after a screencast stops
    settle
done

# a foot window titled "SECRET page" until $WORK/switch-N appears, then "normal page"
open_tab() { # $1: N; sets BOX
    rm -f "$WORK/switch-$1"
    foot --app-id tabs sh -c "printf '\033]2;SECRET page\007'; while [ ! -f $WORK/switch-$1 ]; do sleep 0.02; done; printf '\033]2;normal page\007'; while :; do date; sleep 0.2; done" >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ "$(title)" = "SECRET page" ] && break; done
    sleep 0.5
    BOX=$(win_box tabs)
}
switch_tab() { # $1: N
    touch "$WORK/switch-$1"
    for _ in $(seq 50); do [ "$(title)" = "normal page" ] && break; sleep 0.01; done
}
close_tab() { # $1: N
    pkill -f "switch-$1" 2>/dev/null
    for _ in $(seq 20); do [ -z "$(win_box tabs)" ] && break; sleep 0.1; done
    settle
}

echo "== hold after the title stops matching (no_screen_share_cover_hold = 3000)"
N=0
for i in 1 2 3; do
    N=$((N + 1)); open_tab "$N"
    check grim "c$i-hidden" "$BOX" cover "run $i: grim while the title matches"
    check "$WORK/grim-share" "c$i-share" "$BOX" content "run $i: grim-share while the title matches"
    switch_tab "$N"
    check grim "c$i-held" "$BOX" cover "run $i: grim right after the switch, a grim-share shot in between"
    close_tab "$N"
done
for i in 1 2 3; do
    N=$((N + 1)); open_tab "$N"
    check grim "d$i-hidden" "$BOX" cover "run $i: grim while the title matches"
    switch_tab "$N"
    check "$WORK/grim-share" "d$i-share" "$BOX" content "run $i: grim-share right after the switch gets no hold"
    close_tab "$N"
done

echo "== show_to with a full path"
open_win by-path
check "$WORK/grim-share" e-path "$BOX" content "the listed path sees the window"
check "$WORK/elsewhere/grim-share" e-copy "$BOX" cover "a copy with the same name elsewhere gets the cover"
kill "$PID"
open_win by-link
check "$WORK/grim-share" e-link "$BOX" content "a path through a symlink sees the window"
check "$WORK/elsewhere/grim-share" e-link-copy "$BOX" cover "the copy elsewhere still gets the cover"
kill "$PID"
rm -f "$WORK-link"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
