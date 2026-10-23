#!/usr/bin/env bash
# show_to while a stream is running: a region screenshot from an allowed client (a copy of
# grim named grim-share) shows the hidden window, the stream (wf-recorder) keeps the cover
# during and after the shot.
#
#   tests/e2e/shot-during-stream.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, wf-recorder, ffmpeg, foot, jq, imagemagick; a seat.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-shot.XXXXXX)
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

cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", show_to = "grim-share" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
LUA

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$1" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }

pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 2
foot --app-id cover-me sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box cover-me)" ] && break; done
sleep 1
BOX=$(win_box cover-me)
# region inside the window, like slurp would give (x,y WxH)
read -r W H X Y <<<"$(sed 's/[x+]/ /g' <<<"$BOX")"
REGION="$((X + 20)),$((Y + 20)) $((W - 40))x$((H - 40))"

echo "== screenshot during a stream"
wf-recorder -f "$WORK/stream.mkv" -c libx264 -r 10 >/dev/null 2>&1 &
REC=$!
sleep 2
"$WORK/grim-share" -g "$REGION" "$WORK/shot.png"
sleep 2
grim "$WORK/plain.png"
sleep 1
kill -INT "$REC"; wait "$REC" 2>/dev/null
sleep 0.6   # Hyprland 0.56.2 can crash on exit right after a screencast stops

C=$(mean_rgb "$WORK/shot.png" "$((W - 40))x$((H - 40))+0+0")
is_cover "$C" && fail "grim-share region shot shows the cover ($C)" || pass "grim-share region shot shows the window ($C)"
C=$(mean_rgb "$WORK/plain.png" "$BOX")
is_cover "$C" && pass "plain grim still gets the cover ($C)" || fail "plain grim sees the window ($C)"

ffmpeg -loglevel error -i "$WORK/stream.mkv" -vf fps=5 "$WORK/f%03d.png"
N=$(ls "$WORK"/f*.png 2>/dev/null | wc -l)
BAD=0
for f in "$WORK"/f*.png; do
    C=$(mean_rgb "$f" "$BOX"); is_cover "$C" || BAD=$((BAD + 1))
done
[ "$N" -gt 5 ] && [ "$BAD" -eq 0 ] && pass "stream kept the cover in all $N frames" || fail "stream: $BAD of $N frames show the window"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
