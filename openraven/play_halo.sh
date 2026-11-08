#!/bin/bash
# Run Halo: Fireteam Raven (v3.11) on Linux / WSL2 with OpenRaven gamepad & graphics shims.
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

IS_WSL=0
grep -qi microsoft /proc/version 2>/dev/null && IS_WSL=1
if [ "$IS_WSL" = 1 ]; then
  export GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}"
  export mesa_glthread="${mesa_glthread:-true}"
  export MESA_GLTHREAD="${MESA_GLTHREAD:-true}"
  if [ -z "$MESA_D3D12_DEFAULT_ADAPTER_NAME" ]; then
    # Auto-detect RTX / GeForce GPU if multiple GPUs (e.g. Tesla + RTX 3080) are present
    DETECTED_GPU=$(/usr/lib/wsl/lib/nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | grep -iE 'geforce|rtx' | head -n 1)
    [ -n "$DETECTED_GPU" ] && export MESA_D3D12_DEFAULT_ADAPTER_NAME="$DETECTED_GPU"
  fi
  if [ -z "$RAVENVK_WIN_SERVER" ]; then
    HOST_IP=$(ip route show default 2>/dev/null | awk '{print $3; exit}')
    export RAVENVK_WIN_SERVER="${HOST_IP:-127.0.0.1}:28765"
  fi
fi

# Graphics settings (resolution, AA, anisotropy, FPS, ...): see graphics.ini.
. "$OUT/graphics.sh"
# Resolution: command-line WxH wins, then graphics.ini `resolution` (WxH or "native"), else 1920x1080.
RES="${1:-${HALO_RES:-$(gfx resolution 1920x1080)}}"
if [ "$RES" = native ]; then
  # In WSLg, xrandr reports active monitor resolution while xdpyinfo often reports unscaled 640x480
  RES=$(xrandr 2>/dev/null | grep '\*' | awk '{print $1; exit}')
  [ -z "$RES" ] && RES=$(xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2; exit}')
  [ "$RES" = 640x480 ] && RES=1920x1080
  RES="${RES:-1920x1080}"
fi
CMD=(env LD_PRELOAD="$PRELOAD" ./game)
# HALO_GDB=1: run under gdb and print a backtrace on crash (debugging).
[ -n "$HALO_GDB" ] && CMD=(gdb -q -batch -ex "set startup-with-shell off" -ex "set environment LD_PRELOAD=$PRELOAD" -ex "handle SIGPIPE nostop noprint" -ex "set breakpoint pending on" -ex "${HALO_GDB_BREAK:-echo}" -ex run -ex "info breakpoints" -ex bt -ex "info registers rip" -ex 'x/8i $pc' -ex 'p/x $rbx' -ex 'p/x $rdi' -ex 'x/gx 0x16914d0' -ex 'x/4gx 0x1c1caa0' -ex 'x/40gx $rsp' --args ./game)

# The game's own output level is calibrated for arcade cabinets with hardware amps.
# Under WSLg/PulseAudio, sink volume is already unmuted and set to 100%. Keeping stream at 100% avoids digital clipping.
boost_volume() {
  # WSLg's PulseAudio sink often starts low/muted: unmute and set it to 100% first.
  if [ "$IS_WSL" = 1 ]; then
    pactl set-sink-mute @DEFAULT_SINK@ 0 2>/dev/null || true
    pactl set-sink-volume @DEFAULT_SINK@ 100% 2>/dev/null || true
  fi
  # Keep re-applying for a while: the game may reopen its audio stream (new sink-input id).
  done_ids=" "
  for _ in $(seq 1 120); do
    sleep 2
    for id in $(pactl list sink-inputs 2>/dev/null | awk '/^Sink Input #/{id=substr($3,2)} /PipeWire ALSA \[game\]|ALSA plug-in \[game\]|application.process.binary = "game"/{print id}' | sort -u); do
      case "$done_ids" in *" $id "*) continue;; esac
      pactl set-sink-input-volume "$id" "${HALO_VOLUME:-100%}" 2>/dev/null && done_ids="$done_ids$id "
    done
  done
}
boost_volume &

# Fullscreen: on native Linux, apply EWMH fullscreen via wmctrl.
# On WSLg, windows map natively to the Windows desktop via FreeRDP RAIL.
make_fullscreen() {
  if [ "$IS_WSL" = 1 ]; then
    return 0
  fi
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

# Choose ALSA config: lookahead soft-limiter if LADSPA is available, otherwise Pulse/PipeWire fallback
ASOUND_CONF="$OUT/halo_asound.conf"
if [ -f /usr/lib/ladspa/fast_lookahead_limiter_1913.so ] || [ -f /usr/lib64/ladspa/fast_lookahead_limiter_1913.so ]; then
  ASOUND_CONF="$OUT/halo_asound_limiter.conf"
elif [ -f "$OUT/halo_asound_pulse.conf" ] && ! pactl info 2>/dev/null | grep -qi pipewire; then
  ASOUND_CONF="$OUT/halo_asound_pulse.conf"
fi

# Boot straight to attract: skip the file test and gun calibration. RIO2PAD_SKIPBOOT=0 shows them.
# Everything the game and our hooks print goes to run_logs/last_play.log (screen changes included).
export RIO2PAD_INPUTLOG="${RIO2PAD_INPUTLOG:-$OUT/run_logs/input.log}"
REAL_PM="$(readlink -f "$WS/pm" 2>/dev/null || true)"
bwrap --tmpfs / \
  --ro-bind /usr /usr --ro-bind /etc /etc \
  --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin \
  --proc /proc --dev-bind /dev /dev --ro-bind /sys /sys \
  --tmpfs /tmp --tmpfs /run --tmpfs /dev/shm \
  --bind-try /tmp/.X11-unix /tmp/.X11-unix \
  --ro-bind-try /mnt/wslg /mnt/wslg \
  --ro-bind-try /usr/lib/wsl /usr/lib/wsl \
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
  --setenv IS_WSL "$IS_WSL" \
  ${GALLIUM_DRIVER:+--setenv GALLIUM_DRIVER "$GALLIUM_DRIVER"} \
  --setenv mesa_glthread "${mesa_glthread:-true}" \
  --setenv MESA_GLTHREAD "${MESA_GLTHREAD:-true}" \
  ${MESA_D3D12_DEFAULT_ADAPTER_NAME:+--setenv MESA_D3D12_DEFAULT_ADAPTER_NAME "$MESA_D3D12_DEFAULT_ADAPTER_NAME"} \
  ${HALO_FULLSCREEN:+--setenv HALO_FULLSCREEN "$HALO_FULLSCREEN"} \
  --setenv DISPLAY "${DISPLAY:-:0}" \
  --setenv WAYLAND_DISPLAY "${WAYLAND_DISPLAY:-}" \
  --setenv PULSE_SERVER "${PULSE_SERVER:-}" \
  --setenv XAUTHORITY "${XAUTHORITY:-}" \
  --setenv XDG_RUNTIME_DIR "/run/user/$(id -u)" \
  --setenv LD_BIND_NOW 1 \
  --setenv LD_LIBRARY_PATH "/pm/g7/extralibs:/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu:/usr/lib64:/usr/lib" \
  --chdir /pm/g7/halo \
  "${GFX_PREFIX[@]}" "${CMD[@]}" -f"$RES" -n "${GFX_ARGS[@]}" 2>&1 | tee "$OUT/run_logs/last_play.log"
