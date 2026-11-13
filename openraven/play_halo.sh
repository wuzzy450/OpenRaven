#!/bin/bash
# Run Halo: Fireteam Raven (v3.11) on Linux with OpenRaven gamepad & graphics shims.
# Usage: ./play_halo.sh [WxH]   (default: graphics.ini resolution or native desktop size)
set -e
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS="$(dirname "$OUT")"

# The launcher puts the v3.11 game + halo.so it found in your archives into openraven/3.11-dec: prefer those.
DEC="$OUT/3.11-dec/pm/g7/halo"
if [ -z "$HALO_GAME" ] && [ -f "$DEC/game" ]; then HALO_GAME="$DEC/game"; fi
HALO_GAME="${HALO_GAME:-$WS/pm/g7/halo/game}"
HALOSO_BIND=()
[ -f "$DEC/halo.so" ] && HALOSO_BIND=(--ro-bind "$DEC/halo.so" /pm/g7/halo/halo.so)
if [ ! -f "$HALO_GAME" ]; then
  echo "Error: game files not found in $WS/pm/g7/halo/. Run ./start_linux.sh from the root folder first." >&2
  exit 1
fi

# Ensure required runtime directory exists (NetworkInit fails if messages is missing or is a file)
[ -f "$WS/pm/g7/halo/pmuser/messages" ] && rm -f "$WS/pm/g7/halo/pmuser/messages"
mkdir -p "$WS/pm/g7/halo/pmuser/messages" "$OUT/run_logs" "$WS/pm/g7/dump"

PRELOAD="/pm/g7/halo/halo.so /pm/g7/rio2pad.so /pm/g7/ravengfx.so"

# Graphics settings (resolution, AA, anisotropy, FPS, ...): see graphics.ini.
. "$OUT/graphics.sh"
# Resolution: command-line WxH wins, then graphics.ini `resolution` (WxH or "native"), else 1920x1080.
RES="${1:-${HALO_RES:-$(gfx resolution 1920x1080)}}"
if [ "$RES" = native ]; then
  RES=$(xrandr 2>/dev/null | grep '\*' | awk '{print $1; exit}')
  [ -z "$RES" ] && RES=$(xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2; exit}')
  [ "$RES" = 640x480 ] && RES=1920x1080
  RES="${RES:-1920x1080}"
fi
CMD=(env LD_PRELOAD="$PRELOAD" ./game)
# HALO_GDB=1: run under gdb and print a backtrace on crash (debugging).
[ -n "$HALO_GDB" ] && CMD=(gdb -q -batch -ex "set startup-with-shell off" -ex "set environment LD_PRELOAD=$PRELOAD" -ex "handle SIGPIPE nostop noprint" -ex "set breakpoint pending on" -ex "${HALO_GDB_BREAK:-echo}" -ex run -ex "info breakpoints" -ex bt -ex "info registers rip" -ex 'x/8i $pc' -ex 'p/x $rbx' -ex 'p/x $rdi' -ex 'x/gx 0x16914d0' -ex 'x/4gx 0x1c1caa0' -ex 'x/40gx $rsp' --args ./game)

# Background helpers below are stopped when the game exits.
trap 'kill $(jobs -p) 2>/dev/null || true' EXIT

# Set the game's audio stream to graphics.ini `volume` (default 100%; test runs use 0%) once per stream,
# for the first minute. Always done, because the sound server remembers the last volume per app (a muted
# test run would leave the next game silent). After that it isn't touched, so volume keys keep control.
set_game_volume() {
  local done_ids=" "
  for _ in $(seq 1 30); do
    sleep 2
    for id in $(pactl list sink-inputs 2>/dev/null | awk '/^Sink Input #/{id=substr($3,2)} /PipeWire ALSA \[game\]|ALSA plug-in \[game\]|application.process.binary = "game"/{print id}' | sort -u); do
      case "$done_ids" in *" $id "*) continue;; esac
      pactl set-sink-input-volume "$id" "${HALO_VOLUME:-100%}" 2>/dev/null && done_ids="$done_ids$id "
    done
  done
}
set_game_volume &

# Fullscreen: apply EWMH fullscreen via wmctrl.
make_fullscreen() {
  command -v wmctrl >/dev/null 2>&1 || return 0
  for _ in $(seq 1 60); do
    sleep 1
    # Standard EWMH window managers
    for w in $(wmctrl -l 2>/dev/null | awk '{print $1}'); do
      wmctrl -i -r "$w" -b add,fullscreen 2>/dev/null && return 0
    done
  done
}
[ "${HALO_FULLSCREEN:-0}" = 1 ] && make_fullscreen &

# ALSA config. The game talks ALSA; send it to the desktop's sound server as a normal app stream so the
# system volume (and the Steam Deck's volume buttons) control it: through the Pulse interface (also what
# PipeWire offers), with the lookahead limiter when LADSPA is installed, else PipeWire's own ALSA plugin.
alsa_plugin() { local d; for d in /usr/lib/alsa-lib /usr/lib64/alsa-lib /usr/lib/x86_64-linux-gnu/alsa-lib; do
  [ -f "$d/libasound_module_pcm_$1.so" ] && return 0; done; return 1; }
[ -z "$PULSE_SERVER" ] && [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native" ] && \
  export PULSE_SERVER="unix:${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native"
ASOUND_CONF="$OUT/halo_asound.conf"
if alsa_plugin pulse && [ -n "$PULSE_SERVER" ]; then
  ASOUND_CONF="$OUT/halo_asound_pulse.conf"
  if [ -f /usr/lib/ladspa/fast_lookahead_limiter_1913.so ]; then   # the limiter config looks in /usr/lib/ladspa
    ASOUND_CONF="$OUT/halo_asound_limiter.conf"
  fi
fi
echo "[OpenRaven] audio: $(basename "$ASOUND_CONF")"
# Browser stream: friends hear the game's own sound. The surround mixes get a "tee" (ALSA file plugin)
# that copies the game's sound, as 48 kHz 16-bit stereo, into a pipe that rio2pad.so (streamweb.c) reads.
if [ "${RAVEN_STREAM:-0}" = 1 ]; then
  TAP_PIPE="$WS/pm/g7/dump/stream_audio.pcm"
  rm -f "$TAP_PIPE"; mkfifo "$TAP_PIPE"
  SLAVE=$(sed -n '/surround51/,/}/s/.*slave[ .{]*pcm "\([a-z_]*\)".*/\1/p' "$ASOUND_CONF" | head -1)
  STREAM_CONF="$OUT/run_logs/stream_asound.conf"
  sed "s/\"$SLAVE\"/\"halo_streamtap\"/" "$ASOUND_CONF" > "$STREAM_CONF"
  cat >> "$STREAM_CONF" <<EOT

# added by play_halo.sh for the browser stream
pcm.halo_streamtap {
    type plug
    slave { pcm "halo_streamfile" format S16_LE rate 48000 channels 2 }
}
pcm.halo_streamfile {
    type file
    slave.pcm "$SLAVE"
    file "/pm/g7/dump/stream_audio.pcm"
    format "raw"
}
EOT
  ASOUND_CONF="$STREAM_CONF"
  export RAVEN_STREAM_AUDIO=/pm/g7/dump/stream_audio.pcm
  echo "[OpenRaven] browser stream: game sound copied through $(basename "$STREAM_CONF") (slave $SLAVE)"
fi

# Boot straight to attract: skip the file test and gun calibration. RIO2PAD_SKIPBOOT=0 shows them.
# Everything the game and our hooks print goes to run_logs/last_play.log (screen changes included).
export RIO2PAD_INPUTLOG="${RIO2PAD_INPUTLOG:-$OUT/run_logs/input.log}"
REAL_PM="$(readlink -f "$WS/pm" 2>/dev/null || true)"
# The X display: the desktop's, or (HDR in gamescope) gamescope's own, which only exists inside it,
# so then bwrap keeps the DISPLAY that gamescope gives it.
DISPLAY_ENV=(--setenv DISPLAY "${DISPLAY:-:0}" --setenv WAYLAND_DISPLAY "${WAYLAND_DISPLAY:-}")
[ ${#HDR_WRAP[@]} -gt 0 ] && DISPLAY_ENV=()
# HDR = 1 (graphics.sh): run inside gamescope at the game's resolution
if [ ${#HDR_WRAP[@]} -gt 0 ]; then
  HDR_WRAP=("${HDR_WRAP[@]:0:${#HDR_WRAP[@]}-1}" -w "${RES%x*}" -h "${RES#*x}" -- env ENABLE_GAMESCOPE_WSI=1)
  echo "HDR: starting the game in gamescope (${HDR_WRAP[*]})"
fi
"${HDR_WRAP[@]}" bwrap --tmpfs / \
  --ro-bind /usr /usr --ro-bind /etc /etc \
  --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
  --proc /proc --dev-bind /dev /dev --ro-bind /sys /sys \
  --tmpfs /tmp --tmpfs /run --tmpfs /dev/shm \
  --bind-try /tmp/.X11-unix /tmp/.X11-unix \
  --ro-bind-try "${XAUTHORITY:-/nonexistent}" "${XAUTHORITY:-/nonexistent}" \
  --bind-try "/run/user/$(id -u)" "/run/user/$(id -u)" \
  --bind "$WS" "$WS" \
  ${REAL_PM:+--bind-try "$REAL_PM" "$REAL_PM"} \
  --tmpfs /pm \
  --bind "$WS/pm/g7/halo" /pm/g7/halo \
  --ro-bind "$OUT/extralibs" /pm/g7/extralibs \
  --ro-bind-try "$WS/pm/g7/halo/eFileMaps" /eFileMaps \
  --bind "$WS/pm/g7/dump" /pm/g7/dump \
  "${HALOSO_BIND[@]}" \
  --ro-bind "$OUT/rio2pad.so" /pm/g7/rio2pad.so \
  --ro-bind "$ASOUND_CONF" /pm/g7/home/.asoundrc \
  --ro-bind "$OUT/ravengfx.so" /pm/g7/ravengfx.so \
  "${GFX_BWRAP[@]}" \
  --setenv HOME /pm/g7/home \
  ${RIO2PAD_DEBUG:+--setenv RIO2PAD_DEBUG 1} \
  ${RIO2PAD_AUTOSTART:+--setenv RIO2PAD_AUTOSTART 1} \
  ${RIO2PAD_STATE_FILE:+--setenv RIO2PAD_STATE_FILE "$RIO2PAD_STATE_FILE"} \
  ${RIO2PAD_JSDIR:+--setenv RIO2PAD_JSDIR "$RIO2PAD_JSDIR"} \
  --setenv RIO2PAD_PROCLOG 1 \
  --setenv RIO2PAD_SKIPBOOT "${RIO2PAD_SKIPBOOT:-1}" \
  ${HALO_GAME:+--ro-bind "$HALO_GAME" /pm/g7/halo/game} \
  --setenv LIBGL_ALWAYS_SOFTWARE "${HALO_SW:-0}" \
  ${GALLIUM_DRIVER:+--setenv GALLIUM_DRIVER "$GALLIUM_DRIVER"} \
  --setenv mesa_glthread "${mesa_glthread:-true}" \
  --setenv MESA_GLTHREAD "${MESA_GLTHREAD:-true}" \
  ${HALO_FULLSCREEN:+--setenv HALO_FULLSCREEN "$HALO_FULLSCREEN"} \
  "${DISPLAY_ENV[@]}" \
  ${PULSE_SERVER:+--setenv PULSE_SERVER "$PULSE_SERVER"} \
  --setenv XAUTHORITY "${XAUTHORITY:-}" \
  --setenv XDG_RUNTIME_DIR "/run/user/$(id -u)" \
  --setenv LD_BIND_NOW 1 \
  --setenv LD_LIBRARY_PATH "/pm/g7/extralibs:/usr/lib/x86_64-linux-gnu:/usr/lib64:/usr/lib" \
  --chdir /pm/g7/halo \
  "${GFX_PREFIX[@]}" "${CMD[@]}" -f"$RES" -n "${GFX_ARGS[@]}" 2>&1 | tee "$OUT/run_logs/last_play.log"
