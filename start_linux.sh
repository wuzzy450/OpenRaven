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
    APT_PKGS=(bubblewrap unzip p7zip-full build-essential libgl1 libgl1-mesa-dri mesa-vulkan-drivers libvulkan1 libx11-6 libx11-dev libxi6 libxrandr2 liblapack3 libudev1 pulseaudio-utils alsa-utils libasound2-plugins swh-plugins ladspa-sdk libvulkan-dev wmctrl)
    $SUDO apt-get update && $SUDO apt-get install -y "${APT_PKGS[@]}"
  elif command -v dnf >/dev/null 2>&1; then
    $SUDO dnf install -y bubblewrap unzip p7zip p7zip-plugins gcc gcc-c++ make libGL mesa-dri-drivers mesa-vulkan-drivers vulkan-loader libX11 libX11-devel libXi libXrandr lapack systemd-libs pulseaudio-utils alsa-utils ladspa-swh-plugins vulkan-headers wmctrl
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -Sy --needed --noconfirm bubblewrap unzip 7zip base-devel libglvnd mesa vulkan-radeon vulkan-intel vulkan-icd-loader libx11 libxi libxrandr lapack systemd-libs libpulse alsa-utils alsa-plugins swh-plugins vulkan-headers
  elif command -v zypper >/dev/null 2>&1; then
    $SUDO zypper install -y bubblewrap unzip p7zip gcc gcc-c++ make libGL1 Mesa-dri Mesa-vulkan-device-select libvulkan1 libX11-6 libX11-devel libXi6 libXrandr2 liblapack3 systemd-libs pulseaudio-utils alsa-utils ladspa-swh-plugins vulkan-headers wmctrl
  else
    echo "Please install the following packages using your distro's package manager: ${need_pkgs[*]}"
    exit 1
  fi
fi

# 2. Game files. Needs (from any of the player's archives, any layout, also archives inside archives;
#    program files recognised by size + SHA-256, so renamed files are found too):
#    ./pm/...                                  game data (folder containing pm/g7/halo/data)
#    ./openraven/3.11-dec/pm/g7/halo/game      decrypted 3.11 game
#    ./openraven/3.11-dec/pm/g7/halo/halo.so   its halo.so
DEC="$ROOT/openraven/3.11-dec/pm/g7/halo"
TMP="$ROOT/openraven/_setup_tmp"
DEC_GAME=e88ba3543d5613c828d14aba26329a3b35b362d5dd7f817db7178c86a3b2fe98
DEC_HALOSO=f000a002e74ba378af9363029259cb5d848fa1e0573ca854c83e4e12e13e551f
SIZE_GAME=33554432
SIZE_HALOSO=880528
DATA_GB=11          # game data, extracted (used when the archive does not list sizes)
DATA_ONLY_PACK=""   # an archive that had the game data but no program files
SETUP_RAN=0

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
have_data() { [ -d "$ROOT/pm/g7/halo/data" ]; }
have_dec() { [ "$(sha "$DEC/game")" = "$DEC_GAME" ] && [ "$(sha "$DEC/halo.so")" = "$DEC_HALOSO" ]; }
ready() { have_data && have_dec; }

SEVENZ=$(command -v 7z || command -v 7zz || command -v 7za || true)
list_archive() {   # one "size<TAB>path" per line (size -1 = unknown), / separators, files only
  if [ -n "$SEVENZ" ]; then
    "$SEVENZ" l -slt -ba "$1" | awk '
      /^Path = /     { if (p != "" && !d) print s "\t" p; p = substr($0, 8); s = -1; d = 0 }
      /^Size = /     { s = substr($0, 8) + 0 }
      /^Folder = \+/ { d = 1 }
      END            { if (p != "" && !d) print s "\t" p }'
  elif command -v bsdtar >/dev/null; then   # "perm links owner group size month day time name"
    bsdtar -tvf "$1" | awk '$1 !~ /^d/ { n = $0; for (i = 1; i <= 8; i++) sub(/^[^ ]+ +/, "", n); print $5 "\t" n }'
  elif [[ "$1" == *.zip ]]; then            # "length date time name"
    unzip -l "$1" | awk 'NR > 3 && $1 ~ /^[0-9]+$/ && NF >= 4 { n = $0; for (i = 1; i <= 3; i++) sub(/^ *[^ ]+ +/, "", n); if (n !~ /\/$/) print $1 "\t" n }'
  else echo "[OpenRaven] Need 7z or bsdtar to read $1" >&2; fi
}
extract() {   # archive dest [members...]
  local a="$1" d="$2"; shift 2; mkdir -p "$d"
  if [ -n "$SEVENZ" ]; then "$SEVENZ" x -y -bso0 -bsp1 "-o$d" "$a" "$@"
  elif command -v bsdtar >/dev/null; then bsdtar -xf "$a" -C "$d" "$@"
  else unzip -q -o "$a" "$@" -d "$d"; fi
}

need_space() {   # bytes what
  local free need
  free=$(( $(df -Pk "$ROOT" | awk 'NR==2 {print $4}') * 1024 ))
  need=$(( $1 + 512 * 1024 * 1024 ))
  if [ "$free" -lt "$need" ]; then
    echo "[OpenRaven] Not enough free disk space to unpack $2." >&2
    echo "[OpenRaven] Needed: about $(( need / 1073741824 + 1 )) GB, free: $(( free / 1073741824 )) GB on the drive with $ROOT." >&2
    echo "[OpenRaven] Free up some space (or move the OpenRaven folder to a bigger drive) and run ./start_linux.sh again." >&2
    exit 1
  fi
}

# Copy any of the given files that is the game program or halo.so (by size, then hash).
take_program() {
  mkdir -p "$DEC"
  local f len
  for f in "$@"; do
    [ -f "$f" ] || continue
    len=$(stat -c %s "$f")
    [ "$len" = "$SIZE_GAME" ] || [ "$len" = "$SIZE_HALOSO" ] || continue
    case "$(sha "$f")" in
      "$DEC_GAME")   cp -f "$f" "$DEC/game";    echo "[OpenRaven] Found the game program ($(basename "$f"))" ;;
      "$DEC_HALOSO") cp -f "$f" "$DEC/halo.so"; echo "[OpenRaven] Found halo.so ($(basename "$f"))" ;;
    esac
  done
}

use_archive() {   # archive [depth]  (depth > 0 = an archive found inside another one)
  local a="$1" depth="${2:-0}"
  local tmpa="$TMP/a$depth" extracted=0
  SETUP_RAN=1
  echo "[OpenRaven] Checking $(basename "$a")"
  local entries; entries=$(list_archive "$a")
  local total known
  total=$(printf '%s\n' "$entries" | awk -F'\t' '$1 >= 0 {t += $1} END {printf "%.0f", t}')
  known=$(printf '%s\n' "$entries" | awk -F'\t' 'NF && $1 < 0 {u = 1} END {print u ? 0 : 1}')
  rm -rf "$tmpa"
  local data; data=$(printf '%s\n' "$entries" | cut -f2- | grep -m1 -E '(^|/)pm/g7/halo/data/' || true)
  if [ -n "$data" ] && ! have_data; then
    local prefix="${data%%pm/g7/halo/data/*}" bytes="$total"
    [ "$known" = 1 ] || bytes=$(( DATA_GB * 1073741824 ))
    need_space "$bytes" "the game data"
    echo "[OpenRaven] Extracting game data (about $(( bytes / 1073741824 + 1 )) GB, takes a few minutes)..."
    extract "$a" "$tmpa"
    extracted=1
  fi
  if ! have_dec; then
    local cands; cands=$(printf '%s\n' "$entries" | awk -F'\t' -v g="$SIZE_GAME" -v h="$SIZE_HALOSO" '
      NF && $2 !~ /(^|\/)pm\/g7\/halo\/data\// && ($2 ~ /(^|\/)(game|halo\.so)$/ || $1 == g || $1 == h) {print $2}')
    if [ -n "$cands" ]; then
      local m; mapfile -t m <<<"$cands"
      [ "$extracted" = 1 ] || extract "$a" "$tmpa" "${m[@]}"
      take_program "${m[@]/#/$tmpa/}"
    fi
  fi
  # move the data into place only now: game / halo.so sit in pm/g7/halo and were taken from there first
  if [ "$extracted" = 1 ] && [ -n "$data" ]; then
    if [ -d "$ROOT/pm" ]; then cp -a "$tmpa/${prefix}pm/." "$ROOT/pm/"; else mv "$tmpa/${prefix}pm" "$ROOT/pm"; fi
  fi
  if [ -n "$data" ] && ! have_dec; then DATA_ONLY_PACK="$(basename "$a")"; fi
  # archives inside this archive (for example a .zip holding the .7z)
  if ! ready && [ "$depth" -lt 2 ]; then
    local sz p
    while IFS=$'\t' read -r sz p; do
      [ -n "$p" ] || continue
      ready && break
      if [ ! -f "$tmpa/$p" ]; then
        if [ "$sz" -gt 0 ] 2>/dev/null; then need_space "$sz" "$p (an archive inside $(basename "$a"))"; fi
        extract "$a" "$tmpa" "$p"
      fi
      use_archive "$tmpa/$p" $(( depth + 1 ))
    done < <(printf '%s\n' "$entries" | awk -F'\t' 'tolower($2) ~ /\.(zip|7z|rar)$/')
  fi
  rm -rf "$tmpa"
  if [ "$depth" = 0 ]; then rm -rf "$TMP"; fi
  return 0
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
    have_data || echo "[OpenRaven] Still needed: your game archive (the one with the pm/g7/halo/data folder)."
    if ! have_dec; then
      if [ -n "$DATA_ONLY_PACK" ]; then
        echo "[OpenRaven] Got the game data from '$DATA_ONLY_PACK'. Still needed: the v3.11 game program (game + halo.so); that archive doesn't have it (the v3.11 .7z pack never has halo.so)."
        echo "[OpenRaven] Pick an archive that has them: a v3.11 copy that includes both files (see README)."
      else
        echo "[OpenRaven] Still needed: the v3.11 game program (game + halo.so). None of the archives checked had it: pick a v3.11 copy that includes both files (see README)."
      fi
    fi
    picked=$(pick_archive)
    [ -n "$picked" ] || { echo "Setup not finished." >&2; exit 1; }
    while IFS= read -r a; do [ -f "$a" ] && use_archive "$a"; done <<<"$picked"
  done
  echo "[OpenRaven] Game files ready."
fi

# Optional: an app-menu and desktop entry, offered once after the first setup.
offer_shortcut() {
  local flag="$ROOT/openraven/.shortcut_asked"
  [ -f "$flag" ] && return 0
  [ -t 0 ] || return 0
  touch "$flag" 2>/dev/null || return 0
  local ans; read -r -p "[OpenRaven] Add a \"Halo Fireteam Raven\" entry to your app menu and desktop? (Y/n) " ans || return 0
  case "${ans,,}" in n*) return 0 ;; esac
  local entry="[Desktop Entry]
Type=Application
Name=Halo Fireteam Raven (OpenRaven)
Exec=\"$ROOT/start_linux.sh\"
Path=$ROOT
Terminal=true
Categories=Game;"
  mkdir -p "$HOME/.local/share/applications"
  printf '%s\n' "$entry" > "$HOME/.local/share/applications/openraven.desktop"
  local desk; desk=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
  if [ -d "$desk" ]; then
    printf '%s\n' "$entry" > "$desk/openraven.desktop"
    chmod +x "$desk/openraven.desktop"
    if command -v gio >/dev/null; then gio set "$desk/openraven.desktop" metadata::trusted true 2>/dev/null || true; fi
  fi
  echo "[OpenRaven] Shortcut added."
}
if [ "$SETUP_RAN" = 1 ]; then offer_shortcut; fi

# 3. Verify and prepare game directory permissions & required folders
mkdir -p "$ROOT/pm/g7/dump" "$ROOT/pm/g7/halo/pmuser/messages"
chmod +x "$ROOT/pm/g7/halo/game" 2>/dev/null || true
chmod 755 "$ROOT/pm/g7/halo/game" "$ROOT/pm/g7/halo/halo.so" "$DEC/game" "$DEC/halo.so" "$ROOT/openraven/"*.sh 2>/dev/null || true

# 4. Build OpenRaven shims if needed (or if source is newer)
if [ ! -f "$ROOT/openraven/rio2pad.so" ] || [ ! -f "$ROOT/openraven/ravengfx.so" ] || \
   [ "$ROOT/openraven/ravengfx.c" -nt "$ROOT/openraven/ravengfx.so" ] || \
   [ "$ROOT/openraven/rio2pad.c" -nt "$ROOT/openraven/rio2pad.so" ]; then
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
