#!/bin/bash
# Run Halo: Fireteam Raven on the real display/GPU using the 3.11-dec halo.so dongle emulator.
# Usage: ./play_halo.sh [WxH]   (default 1920x1080)
# The 3.11-dec "game" runs on current glibc; the older patched pm/g7/halo/game segfaults at startup.
HALO_GAME="${HALO_GAME:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/3.11-dec/pm/g7/halo/game}"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS="$(dirname "$OUT")"
cp "$OUT/3.11-dec/pm/g7/halo/halo.so" "$OUT/.haloso_run.so"
mkdir -p "$OUT/run_logs"
PRELOAD="/pm/g7/halo/halo.so /pm/g7/rio2pad.so /pm/g7/ravengfx.so"
# Graphics settings (resolution, AA, anisotropy, FPS, ...): see graphics.ini / GRAPHICS.md.
. "$OUT/graphics.sh"
# Resolution: command-line WxH wins, then graphics.ini `resolution` (WxH or "native"), else 1920x1080.
RES="${1:-$(gfx resolution 1920x1080)}"
if [ "$RES" = native ]; then
  RES=$(xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2; exit}')
  [ -z "$RES" ] && RES=$(xrandr 2>/dev/null | awk '/\*/{print $1; exit}')
  RES="${RES:-1920x1080}"
fi
CMD=(env LD_PRELOAD="$PRELOAD" ./game)
# HALO_GDB=1: run under gdb and print a backtrace on crash (debugging).
[ -n "$HALO_GDB" ] && CMD=(gdb -q -batch -ex "set startup-with-shell off" -ex "set environment LD_PRELOAD=$PRELOAD" -ex "handle SIGPIPE nostop noprint" -ex "set breakpoint pending on" -ex "${HALO_GDB_BREAK:-echo}" -ex run -ex "info breakpoints" -ex bt -ex "info registers rip" -ex 'x/8i $pc' -ex 'p/x $rbx' -ex 'p/x $rdi' -ex 'x/gx 0x16914d0' -ex 'x/4gx 0x1c1caa0' -ex 'x/40gx $rsp' --args ./game)
# The game's own output level is low (cabinets use an external amp), so boost its PipeWire
# stream once it appears. HALO_VOLUME=100% turns this off.
boost_volume() {
  for _ in $(seq 1 120); do
    sleep 2
    id=$(pactl list sink-inputs 2>/dev/null | awk '/^Sink Input #/{id=substr($3,2)} /PipeWire ALSA \[game\]/{print id; exit}')
    [ -n "$id" ] && { pactl set-sink-input-volume "$id" "${HALO_VOLUME:-300%}"; return; }
  done
}
boost_volume &
# Boot straight to attract: skip the file test and gun calibration. RIO2PAD_SKIPBOOT=0 shows them.
# Everything the game and our hooks print goes to run_logs/last_play.log (screen changes included).
export RIO2PAD_INPUTLOG="${RIO2PAD_INPUTLOG:-$OUT/run_logs/input.log}"
bwrap --tmpfs / \
  --ro-bind /usr /usr --ro-bind /etc /etc \
  --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
  --proc /proc --dev-bind /dev /dev --ro-bind /sys /sys \
  --tmpfs /tmp --tmpfs /run --tmpfs /dev/shm \
  --bind /tmp/.X11-unix /tmp/.X11-unix \
  --ro-bind-try "$XAUTHORITY" "$XAUTHORITY" \
  --bind-try "/run/user/$(id -u)" "/run/user/$(id -u)" \
  --bind "$WS" "$WS" \
  --tmpfs /pm \
  --bind "$WS/pm/g7/halo" /pm/g7/halo \
  --ro-bind "$OUT/extralibs" /pm/g7/extralibs \
  --ro-bind "$WS/pm/g7/halo/eFileMaps" /eFileMaps \
  --bind "$WS/pm/g7/dump" /pm/g7/dump \
  --bind "$OUT/.haloso_run.so" /pm/g7/halo/halo.so \
  --ro-bind "$OUT/rio2pad.so" /pm/g7/rio2pad.so \
  --ro-bind "$OUT/halo_asound.conf" /pm/g7/home/.asoundrc \
  --ro-bind "$OUT/ravengfx.so" /pm/g7/ravengfx.so \
  "${GFX_BWRAP[@]}" \
  --setenv HOME /pm/g7/home \
  ${RIO2PAD_DEBUG:+--setenv RIO2PAD_DEBUG 1} \
  ${RIO2PAD_AUTOSTART:+--setenv RIO2PAD_AUTOSTART 1} \
  --setenv RIO2PAD_PROCLOG 1 \
  --setenv RIO2PAD_SKIPBOOT "${RIO2PAD_SKIPBOOT:-1}" \
  ${HALO_GAME:+--ro-bind "$HALO_GAME" /pm/g7/halo/game} \
  --setenv LIBGL_ALWAYS_SOFTWARE "${HALO_SW:-0}" \
  --setenv DISPLAY "${DISPLAY:-:0}" \
  --setenv XAUTHORITY "${XAUTHORITY:-}" \
  --setenv XDG_RUNTIME_DIR "/run/user/$(id -u)" \
  --setenv LD_BIND_NOW 1 \
  --setenv LD_LIBRARY_PATH "/pm/g7/extralibs:/usr/lib/x86_64-linux-gnu" \
  --chdir /pm/g7/halo \
  "${GFX_PREFIX[@]}" "${CMD[@]}" -f"$RES" -n "${GFX_ARGS[@]}" 2>&1 | tee "$OUT/run_logs/last_play.log"
