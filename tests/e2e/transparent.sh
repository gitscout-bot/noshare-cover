#!/usr/bin/env bash
# no_screen_share_cover = "transparent": a hidden window or layer is left out of the
# capture as if it wasn't there. What is behind it shows, a hidden window behind it keeps
# its whole cover, and the rest of the screen is untouched: the capture must equal a
# capture of the same screen without the transparent surface, also for a region, under
# cursor zoom and while it closes.
#
#   tests/e2e/transparent.sh <libnoshare-cover.so>
#
# Requires: Hyprland 0.57+, grim, foot, wf-recorder, jq, imagemagick; a seat. The layer case
# needs quickshell.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-tr.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 pattern:verticalbricks -fill red -opaque black -fill '#2040ff' -opaque white "$WORK/cover.png"
GONE='#ff00ff' # the transparent surfaces' color, must never reach a capture

write_config() { # $1: path_cover, $2: zoom factor, $3: blur (true/false)
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$1" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = ${3:-false}, ignore_opacity = true } },
    cursor = { zoom_factor = $2 },
    animations = { enabled = true },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
-- only closing is animated, slowly enough to take shots during it
hl.curve("linear", { type = "bezier", points = { {0, 0}, {1, 1} } })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 10, bezier = "linear", style = "popin 87%" })
hl.animation({ leaf = "fadeOut", enabled = true, speed = 10, bezier = "linear" })
hl.animation({ leaf = "windowsIn", enabled = false })
hl.animation({ leaf = "fadeIn", enabled = false })
hl.animation({ leaf = "windowsMove", enabled = false })
hl.animation({ leaf = "layers", enabled = false })
hl.animation({ leaf = "fadeLayers", enabled = false })
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
hl.window_rule({ match = { class = "gone-me" }, no_screen_share = true, no_screen_share_cover = "transparent",
                 float = true, size = { 700, 500 }, move = { 600, 300 } })
hl.window_rule({ match = { class = "glass" }, float = true, size = { 500, 400 }, move = { 1000, 450 } })
hl.layer_rule({ match = { namespace = "nsc-gone" }, no_screen_share = true, no_screen_share_cover = "transparent" })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
win_pid() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | .pid' | head -1; }
rmse() { magick compare -metric RMSE "$WORK/$1.png" "$WORK/$2.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/'; }
gone_px() { magick "$WORK/$1.png" -fuzz 8% -fill black +opaque "$GONE" -fill white -opaque "$GONE" -format '%[fx:int(mean*w*h+0.5)]' info:; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
same() { awk "BEGIN{exit !($1 < 0.01)}"; }

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
}

open_win() { # class [foot args...]: a static window, no cursor
    local class=$1; shift
    foot --app-id "$class" "$@" sh -c 'printf "\033[?25l"; for i in $(seq 40); do printf "\033[3%dm line %d of some text to look at\n" $((i % 7 + 1)) $i; done; sleep 1000' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$class")" ] && break; done
    sleep 1
}

# A visible window on the left, a covered one on the right, and a transparent window
# floating across both: over the visible one it must show that one, over the covered
# one the cover, never itself.
setup() { # $1: path_cover, $2: zoom factor, $3: name of the shot without the transparent window, $4: blur
    write_config "$1" "$2" "${4:-false}"
    start_hyprland
    open_win plain
    open_win cover-me
    shot "$3"
    open_win gone-me -o colors-dark.background=ff00ff
}

# A screencast keeps the output shared and asks for frames continuously, as the portal
# does; grim alone is a one-frame share.
STREAM=
stream_on() { wf-recorder -y -f "$WORK/stream.mkv" -c libx264 -p preset=ultrafast >/dev/null 2>&1 & STREAM=$!; sleep 1.5; }
stream_off() { [ -n "$STREAM" ] && kill "$STREAM" 2>/dev/null; wait "$STREAM" 2>/dev/null; STREAM=; sleep 1; }

check_frame() { # $1 label, $2 shot, $3 shot without the transparent surfaces
    local d n
    d=$(rmse "$3" "$2"); n=$(gone_px "$2")
    same "$d" && pass "$1: same as the screen without it (RMSE $d)" || fail "$1: differs from the screen without it (RMSE $d, $WORK/$2.png vs $3.png)"
    [ "$n" -eq 0 ] && pass "$1: none of it in the capture" || fail "$1: $n of its pixels in the capture ($WORK/$2.png)"
}

echo "== transparent window"
setup "$WORK/cover.png" 1 ref
[ "$(win_box gone-me)" = "700x500+600+300" ] || fail "gone-me is at $(win_box gone-me), not 700x500+600+300"
shot gone
check_frame "window" gone ref
grep -aq "scene: .* drawn without" "$WORK/trace.log" || fail "the frame wasn't drawn without it (trace.log)"

grim -g "500,250 900x600" "$WORK/region.png"
magick "$WORK/ref.png" -crop 900x600+500+250 +repage "$WORK/ref-region.png"
d=$(rmse ref-region region)
same "$d" && pass "region capture: same as the screen without it (RMSE $d)" || fail "region capture differs (RMSE $d, $WORK/region.png)"

if command -v quickshell >/dev/null; then
    mkdir -p "$WORK/qs"
    cat > "$WORK/qs/shell.qml" <<QML
import Quickshell
import Quickshell.Wayland
import QtQuick
PanelWindow {
    WlrLayershell.namespace: "nsc-gone"
    WlrLayershell.layer: WlrLayer.Overlay
    exclusionMode: ExclusionMode.Ignore
    anchors { top: true; right: true }
    margins { top: 120; right: 160 }
    implicitWidth: 420
    implicitHeight: 220
    color: "$GONE"
}
QML
    quickshell -p "$WORK/qs/shell.qml" > "$WORK/qs.log" 2>&1 &
    QS=$!
    for _ in $(seq 40); do sleep 0.25; hctl layers -j | grep -q nsc-gone && break; done
    sleep 1
    if hctl layers -j | grep -q nsc-gone; then
        shot layer
        check_frame "window and layer" layer ref
    else
        fail "the quickshell layer didn't map ($WORK/qs.log)"
    fi
    kill "$QS" 2>/dev/null; sleep 1
else
    echo "  no quickshell, layer case skipped"
fi

hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "grim" } } })' >/dev/null
shot shown
n=$(gone_px shown)
[ "$n" -gt 300000 ] && pass "show_to: grim sees it as it is ($n px)" || fail "show_to: grim doesn't see it ($n px, $WORK/shown.png)"
hctl eval 'hl.config({ plugin = { no_screen_share_cover = { show_to = "" } } })' >/dev/null

# closing, during a stream: the fade-out is left out too (1 s at speed 10)
stream_on
kill "$(win_pid gone-me)"
for t in 1 2 3; do
    sleep 0.2
    shot "closing-$t"
    check_frame "closing, shot $t" "closing-$t" ref
done
stream_off

echo "== path_cover = \"transparent\": every hidden surface without its own cover"
setup transparent 1 ref-global
shot global
IFS='x+' read -r w h x y <<<"$(win_box cover-me)"
c=$(mean_rgb global "200x200+$((x + w - 250))+$((y + 50))") # away from the floating window
read -r r g b <<<"$c"
[ "$r" -lt 40 ] && [ "$g" -lt 40 ] && [ "$b" -lt 40 ] && [ "$r" = "$g" ] && [ "$g" = "$b" ] \
    && pass "the covered window is gone too, the background shows ($c)" || fail "the covered window still shows something ($c, $WORK/global.png)"
n=$(gone_px global)
[ "$n" -eq 0 ] && pass "none of the transparent window in the capture" || fail "$n pixels of the transparent window in the capture"

echo "== cursor zoom"
setup "$WORK/cover.png" 2 ref-zoomed
shot zoomed
check_frame "zoomed" zoomed ref-zoomed

echo "== blur: a translucent window over the transparent one"
setup "$WORK/cover.png" 1 ref-blur true
open_win glass -o colors-dark.alpha=0.8
shot blur
# the same screen once the transparent window has closed (1 s close animation)
kill "$(win_pid gone-me)"
sleep 2
shot blur-after
check_frame "under a blurred translucent window" blur blur-after

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "ALL PASSED ($WORK)" || echo "$FAILS FAILED ($WORK)"
exit "$FAILS"
