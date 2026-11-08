#!/bin/bash
# OpenRaven — Halo: Fireteam Raven (v3.11) Automatic Setup & Launcher for Linux
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "================================================================"
echo "  OpenRaven — Halo: Fireteam Raven (v3.11) PC Loader"
echo "================================================================"

# 1. Check and install required system packages if missing
SUDO="sudo"
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
elif ! command -v sudo >/dev/null 2>&1; then
  if command -v doas >/dev/null 2>&1; then
    SUDO="doas"
  else
    SUDO=""
  fi
fi

need_pkgs=()
command -v bwrap >/dev/null 2>&1 || need_pkgs+=(bubblewrap)
command -v unzip >/dev/null 2>&1 || need_pkgs+=(unzip)
{ command -v 7z || command -v 7zz || command -v 7za || command -v bsdtar; } >/dev/null 2>&1 || need_pkgs+=(7zip)
command -v gcc   >/dev/null 2>&1 || need_pkgs+=(gcc)
command -v g++   >/dev/null 2>&1 || need_pkgs+=(g++)
[ -f /usr/include/X11/Xlib.h ] || need_pkgs+=(libx11-dev)
[ -f /usr/include/vulkan/vulkan.h ] || need_pkgs+=(libvulkan-dev)

# Check for LAPACK runtime (required by game binary)
if ! ldconfig -p 2>/dev/null | grep -q "liblapack\.so\.3" && \
   [ ! -f /usr/lib/x86_64-linux-gnu/liblapack.so.3 ] && \
   [ ! -f /usr/lib64/liblapack.so.3 ] && \
   [ ! -f /usr/lib/liblapack.so.3 ]; then
  need_pkgs+=(liblapack3)
fi

if [ "${#need_pkgs[@]}" -gt 0 ]; then
  echo "[OpenRaven] Installing missing dependencies (${need_pkgs[*]})..."
  if command -v apt-get >/dev/null 2>&1; then
    APT_PKGS=(bubblewrap unzip p7zip-full build-essential libgl1 libgl1-mesa-dri mesa-vulkan-drivers libvulkan1 libx11-6 libx11-dev libxi6 libxrandr2 liblapack3 libudev1 pulseaudio-utils alsa-utils libasound2-plugins libvulkan-dev wmctrl)
    $SUDO apt-get update && $SUDO apt-get install -y "${APT_PKGS[@]}"
  elif command -v dnf >/dev/null 2>&1; then
    $SUDO dnf install -y bubblewrap unzip p7zip p7zip-plugins gcc gcc-c++ make libGL mesa-dri-drivers mesa-vulkan-drivers vulkan-loader libX11 libX11-devel libXi libXrandr lapack systemd-libs pulseaudio-utils alsa-utils vulkan-headers wmctrl
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -Sy --needed --noconfirm bubblewrap unzip 7zip base-devel libglvnd mesa vulkan-radeon vulkan-intel vulkan-icd-loader libx11 libxi libxrandr lapack systemd-libs libpulse alsa-utils alsa-plugins vulkan-headers
  elif command -v zypper >/dev/null 2>&1; then
    $SUDO zypper install -y bubblewrap unzip p7zip gcc gcc-c++ make libGL1 Mesa-dri Mesa-vulkan-device-select libvulkan1 libX11-6 libX11-devel libXi6 libXrandr2 liblapack3 systemd-libs pulseaudio-utils alsa-utils vulkan-headers wmctrl
  else
    echo "Please install the following packages using your distro's package manager: ${need_pkgs[*]}"
    exit 1
  fi
fi

# 2. Game files. Needs (from any of the player's archives, any layout; recognised by SHA-256):
#    ./pm/...                                  game data (folder containing pm/g7/halo/data)
#    ./openraven/3.11-dec/pm/g7/halo/game      decrypted 3.11 game
#    ./openraven/3.11-dec/pm/g7/halo/halo.so   its halo.so
DEC="$ROOT/openraven/3.11-dec/pm/g7/halo"
TMP="$ROOT/openraven/_setup_tmp"
DEC_GAME=e88ba3543d5613c828d14aba26329a3b35b362d5dd7f817db7178c86a3b2fe98
DEC_HALOSO=f000a002e74ba378af9363029259cb5d848fa1e0573ca854c83e4e12e13e551f

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
have_data() { [ -d "$ROOT/pm/g7/halo/data" ]; }
have_dec() { [ "$(sha "$DEC/game")" = "$DEC_GAME" ] && [ "$(sha "$DEC/halo.so")" = "$DEC_HALOSO" ]; }
ready() { have_data && have_dec; }

SEVENZ=$(command -v 7z || command -v 7zz || command -v 7za || true)
list_archive() {   # one path per line, / separators
  if [ -n "$SEVENZ" ]; then "$SEVENZ" l -slt -ba "$1" | sed -n 's/^Path = //p'
  elif command -v bsdtar >/dev/null; then bsdtar -tf "$1"
  elif [[ "$1" == *.zip ]]; then unzip -Z1 "$1"
  else echo "[OpenRaven] Need 7z or bsdtar to read $1" >&2; fi
}
extract() {   # archive dest [members...]
  local a="$1" d="$2"; shift 2; mkdir -p "$d"
  if [ -n "$SEVENZ" ]; then "$SEVENZ" x -y -bso0 -bsp1 "-o$d" "$a" "$@"
  elif command -v bsdtar >/dev/null; then bsdtar -xf "$a" -C "$d" "$@"
  else unzip -q -o "$a" "$@" -d "$d"; fi
}

use_archive() {
  local a="$1"
  echo "[OpenRaven] Checking $a"
  local entries; entries=$(list_archive "$a")
  local data; data=$(printf '%s\n' "$entries" | grep -m1 -E '(^|/)pm/g7/halo/data/' || true)
  rm -rf "$TMP"
  if [ -n "$data" ] && ! have_data; then
    local prefix="${data%%pm/g7/halo/data/*}"
    echo "[OpenRaven] Extracting game data (about 10 GB, takes a few minutes)..."
    extract "$a" "$TMP"
    if [ -d "$ROOT/pm" ]; then cp -a "$TMP/${prefix}pm/." "$ROOT/pm/"; else mv "$TMP/${prefix}pm" "$ROOT/pm"; fi
  fi
  local cands; cands=$(printf '%s\n' "$entries" | grep -E '(^|/)(game|halo\.so)$' || true)
  if [ -n "$cands" ] && ! have_dec; then
    local base="$TMP"
    if [ ! -d "$TMP" ]; then base="$TMP/cand"; mapfile -t m <<<"$cands"; extract "$a" "$base" "${m[@]}"; fi
    mkdir -p "$DEC"
    while IFS= read -r c; do
      [ -f "$base/$c" ] || continue
      case "$(sha "$base/$c")" in
        "$DEC_GAME")   cp -f "$base/$c" "$DEC/game";    echo "[OpenRaven] Found the game program" ;;
        "$DEC_HALOSO") cp -f "$base/$c" "$DEC/halo.so"; echo "[OpenRaven] Found halo.so" ;;
      esac
    done <<<"$cands"
  fi
  rm -rf "$TMP"
}

pick_archive() {   # prints chosen paths, one per line
  if { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; } && command -v zenity >/dev/null; then
    zenity --file-selection --multiple --separator=$'\n' --title="Select your Halo: Fireteam Raven game archive" \
      --file-filter="Archives | *.zip *.7z *.rar" 2>/dev/null || true
  elif { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; } && command -v kdialog >/dev/null; then
    kdialog --multiple --separate-output --getopenfilename "$HOME" "*.zip *.7z *.rar|Archives" 2>/dev/null || true
  elif [ -t 0 ]; then
    read -r -p "Path to an archive (drag and drop, empty = stop): " p || true
    p="${p%\'}"; p="${p#\'}"; p="${p%\"}"; p="${p#\"}"; p="${p%"${p##*[![:space:]]}"}"
    [ -n "$p" ] && printf '%s\n' "$p"
  fi
}

if ! ready; then
  for a in "$@" "$ROOT"/* "$ROOT"/../* "$HOME/Downloads"/* "$HOME/Desktop"/*; do
    [ -f "$a" ] || continue
    case "${a,,}" in *.zip|*.7z|*.rar) ;; *) continue ;; esac
    [[ "$(basename "${a,,}")" =~ raven|halo|3\.11 ]] || continue
    ready && break
    use_archive "$a"
  done
  while ! ready; do
    have_data || echo "[OpenRaven] Please pick your game archive."
    have_dec  || echo "[OpenRaven] Your archive did not have the v3.11 game program files (game + halo.so). Please pick an archive that has them."
    picked=$(pick_archive)
    [ -n "$picked" ] || { echo "Setup not finished." >&2; exit 1; }
    while IFS= read -r a; do [ -f "$a" ] && use_archive "$a"; done <<<"$picked"
  done
  echo "[OpenRaven] Game files ready."
fi

# 3. Verify and prepare game directory permissions & required folders
mkdir -p "$ROOT/pm/g7/dump" "$ROOT/pm/g7/halo/pmuser/messages"
chmod +x "$ROOT/pm/g7/halo/game" 2>/dev/null || true
chmod 755 "$ROOT/pm/g7/halo/game" "$ROOT/pm/g7/halo/halo.so" "$DEC/game" "$DEC/halo.so" "$ROOT/openraven/"*.sh 2>/dev/null || true

# 4. Build OpenRaven shims if needed (or if source is newer)
if [ ! -f "$ROOT/openraven/rio2pad.so" ] || [ ! -f "$ROOT/openraven/ravengfx.so" ]; then
  echo "[OpenRaven] Building shims (rio2pad.so, ravengfx.so)..."
  "$ROOT/openraven/build.sh"
fi
if grep -qi microsoft /proc/version 2>/dev/null; then
  echo "[OpenRaven] Note: on Windows, use start_windows.bat (native, much faster than WSL)."
fi

echo ""
echo "[OpenRaven] Launching Halo: Fireteam Raven..."
echo "  - Press F1 in-game to open the OpenRaven Settings Overlay"
echo "  - Run ./openraven/stop_halo.sh to force-close the game"
echo ""

if [ $# -gt 0 ]; then
  if [ -f "$1" ] || [ -d "$1" ]; then
    shift
  fi
fi

exec "$ROOT/openraven/play_halo.sh" "$@"
