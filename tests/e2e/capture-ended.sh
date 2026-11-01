#!/usr/bin/env bash
# A video cover keeps the monitor repainting while it's captured, so a stream of a static
# screen doesn't freeze it. Once the stream ends that must stop. A wlr-screencopy session
# (xdg-desktop-portal-hyprland, wf-recorder) is kept by Hyprland until the monitor goes away,
# even after its client has gone, and the plugin used to take that for a capture: after the
# first portal stream it repainted 60 times a second until logout.
#
#   tests/e2e/capture-ended.sh <libnoshare-cover.so> <video>
#
# Requires: Hyprland, foot, wf-recorder, jq; a seat. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
CLIP=$(realpath "${2:?a video file}")
WORK=$(mktemp -d /tmp/nsc-ce.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$CLIP" } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true, disable_watchdog_warning = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
LUA

hctl() { hyprctl -i 0 "$@"; }
pkill -xu "$(id -u)" Hyprland; sleep 0.5
Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
sleep 2
foot --app-id cover-me sh -c 'sleep 1000' >/dev/null 2>&1 &
sleep 3

frames() { grep -ac "frame: monitor" "$WORK/trace.log" 2>/dev/null; }
wf-recorder -y -f "$WORK/stream.mkv" -c libx264 -p preset=ultrafast >/dev/null 2>&1 &
REC=$!
sleep 1
a=$(frames); sleep 2; b=$(frames)
# nothing on screen changes: only the video cover makes Hyprland produce frames
[ $((b - a)) -ge 30 ] && pass "a stream of a static screen keeps getting frames ($((b - a)) in 2 s)" \
    || fail "the stream of a static screen got $((b - a)) frames in 2 s"
kill "$REC"; wait "$REC" 2>/dev/null
ended=$(wc -l < "$WORK/trace.log")
sleep 1.5
tail -n +"$((ended + 1))" "$WORK/trace.log" | grep -aq "pump: idle" \
    && pass "after the stream ends the cover stops repainting the monitor" \
    || fail "the cover keeps repainting the monitor after the stream ended ($WORK/trace.log)"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
