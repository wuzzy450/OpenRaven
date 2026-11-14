# Sourced by play_halo.sh. Turns graphics.ini into launcher options.
# Lower-case keys are handled here; UPPER-CASE keys are engine settings applied by ravengfx.so.
GFX_INI="${HALO_GFX_INI:-$OUT/graphics.ini}"
GFX_BWRAP=() GFX_PREFIX=() GFX_ARGS=()
gfx() { # gfx key default -> value from graphics.ini (env HALO_GFX_<key> wins)
  local env="HALO_GFX_$1" v
  v="${!env}"
  [ -z "$v" ] && [ -f "$GFX_INI" ] && v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^#]*\).*/\1/p" "$GFX_INI" | tail -1 | sed 's/[[:space:]]*$//')
  echo "${v:-$2}"
}
[ -f "$GFX_INI" ] || : > "$GFX_INI"   # the F1 overlay saves here
[ -f "$GFX_INI" ] && GFX_BWRAP+=(--bind "$GFX_INI" /pm/g7/graphics.ini --setenv RAVENGFX_CONF /pm/g7/graphics.ini)
[ -n "$RAVENGFX_KEYLOG" ] && GFX_BWRAP+=(--setenv RAVENGFX_KEYLOG "$RAVENGFX_KEYLOG")
[ -n "$RAVENGFX_RENDER" ] && GFX_BWRAP+=(--setenv RAVENGFX_RENDER "$RAVENGFX_RENDER")
for v in RAVENGFX_SHOT RAVENGFX_SHOTDIR RAVENGFX_FPSLOG RAVENGFX_OVERLAY RAVENGFX_UIKEYS RAVENGFX_TANLOG RAVENGFX_HORPLUS_SITE RAVENGFX_HORPLUS_K RAVENGFX_GLTRACE RAVENGFX_PROGSRC RAVENGFX_SWAPINTERVAL RAVENGFX_FPSCAP RAVENFSR_FP16 RAVENFSR_GLDEBUG RAVENFSR_FLAGS RAVENFSR_JSX RAVENFSR_JSY RAVENFSR_MSX RAVENFSR_MSY RAVENGFX_TAADUMP RAVENNN RAVENNN_DUMP RAVENNN_BACKEND RAVENNN_VKTEST RAVENVK_DEVICE RAVENFG RAVENFG_EVAL RAVENGFX_BLOOMFIX RAVENGFX_TEXSTAT RAVENFG_FORCE RAVENGFX_NOGLHOOK RAVENNN_PAIR_EVERY RAVENNN_PAIR_KEY; do
  [ -n "${!v}" ] && GFX_BWRAP+=(--setenv "$v" "${!v}")
done
# Launcher/cabinet/input options (env vars win over graphics.ini).
export HALO_VOLUME="${HALO_VOLUME:-$(gfx volume 100%)}"
[ -z "$RIO2PAD_AIM" ] && [ "$(gfx aim cursor)" = abs ] && export RIO2PAD_AIM=abs
[ -z "$RIO2PAD_STICKSPEED" ] && export RIO2PAD_STICKSPEED="$(gfx stick_speed 0.25)"
[ -z "$RIO2PAD_MOUSE" ] && export RIO2PAD_MOUSE="$(gfx mouse screen)"
[ -z "$RIO2PAD_PADSTART" ] && export RIO2PAD_PADSTART="$(gfx pad_start 1)"
[ -z "$RIO2PAD_MOUSESENS" ] && export RIO2PAD_MOUSESENS="$(gfx mouse_sens 1.0)"
[ -z "$RIO2PAD_FREEPLAY" ] && [ "$(gfx free_play 1)" = 0 ] && export RIO2PAD_FREEPLAY=0
[ -z "$RIO2PAD_SKIPBOOT" ] && export RIO2PAD_SKIPBOOT="$(gfx skip_boot 1)"
# Cabinet: players = 4 (default) is the 4-player cabinet, shown on one screen in the hybrid picture
# (menus once, missions full screen, 16:9 cutscenes); players = 2 is the original 2-player cabinet.
# wide4 = hybrid (default) | 1 (both cabinet screens side by side, 32:9) | 0 (squeezed).
[ -z "$RIO2PAD_PLAYERS" ] && export RIO2PAD_PLAYERS="$(gfx players 4)"
[ -z "$RAVEN_WIDE4" ] && export RAVEN_WIDE4="$(gfx wide4 hybrid)"
# Online coop (F1 Coop tab): coop = off|host|join, coop_address = the host to join. Env vars win.
if [ -z "$RAVEN_COOP" ]; then
  case "$(gfx coop off)" in
    host) export RAVEN_COOP=host;;
    join) A="$(gfx coop_address '')"; if [ -n "$A" ]; then export RAVEN_COOP="join:$A"; else echo "[coop] Coop tab says join, but no host address is set: coop off"; fi;;
  esac
  if [ -n "$RAVEN_COOP" ]; then
    [ -z "$RAVEN_COOP_PLAYERS" ] && export RAVEN_COOP_PLAYERS="$(gfx coop_players 2)"
    [ -z "$RAVEN_COOP_PORT" ] && export RAVEN_COOP_PORT="$(gfx coop_port 27500)"
    [ -z "$RAVEN_COOP_DELAY" ] && export RAVEN_COOP_DELAY="$(gfx coop_delay 3)"
    [ -z "$RAVEN_COOP_UPNP" ] && export RAVEN_COOP_UPNP="$(gfx coop_upnp 1)"
  fi
fi
# Browser stream (F1 Coop tab): friends watch and play in a web browser.
[ -z "$RAVEN_STREAM" ] && export RAVEN_STREAM="$(gfx stream 0)"
[ -z "$RAVEN_STREAM_PORT" ] && export RAVEN_STREAM_PORT="$(gfx stream_port 27600)"
[ -z "$RAVEN_STREAM_KBPS" ] && export RAVEN_STREAM_KBPS="$(gfx stream_kbps 8000)"
[ -z "$RAVEN_STREAM_ENCODER" ] && export RAVEN_STREAM_ENCODER="$(gfx stream_encoder auto)"
for v in RAVENGFX_UISCRIPT RAVENGFX_UIKEY RAVENGFX_NOUI; do
  [ -n "${!v}" ] && GFX_BWRAP+=(--setenv "$v" "${!v}")
done
# VSync: NVIDIA's driver-level switch too, so the game's own swap-interval calls can't turn it off.
case "$(gfx VSYNC 0)" in 1) GFX_BWRAP+=(--setenv __GL_SYNC_TO_VBLANK 1);; 0) GFX_BWRAP+=(--setenv __GL_SYNC_TO_VBLANK 0);; esac

# First launch (autodetect = 1): is HDR on for the desktop, and how bright is the screen? Saves HDR,
# HDR_PEAK (when the EDID says) and autodetect = 0 to graphics.ini, like display_detect_win.c on Windows.
gfx_set() { # gfx_set key value: replace the key in graphics.ini, or add it
  if grep -q "^[[:space:]]*$1[[:space:]]*=" "$GFX_INI"; then sed -i "s|^[[:space:]]*$1[[:space:]]*=.*|$1 = $2|" "$GFX_INI"; else echo "$1 = $2" >> "$GFX_INI"; fi
}
if [ "$(gfx autodetect 0)" = 1 ]; then
  DET_HDR=0 DET_PEAK=
  # KDE Plasma 6: kscreen-doctor lists each output's HDR state
  if command -v kscreen-doctor >/dev/null && command -v python3 >/dev/null; then
    DET_HDR=$(kscreen-doctor -j 2>/dev/null | python3 -I -c 'import json,sys
try: o=[o for o in json.load(sys.stdin)["outputs"] if o.get("enabled")]
except Exception: o=[]
o.sort(key=lambda o: o.get("priority", 99)); print(1 if o and o[0].get("hdr") else 0)' 2>/dev/null || echo 0)
  fi
  # peak from the EDID (CTA-861 HDR static metadata: max luminance code L, nits = 50 * 2^(L/32))
  if command -v python3 >/dev/null; then
    DET_PEAK=$(python3 -I - <<'PY' 2>/dev/null
import glob
def peak(e):
    for b in range(128, len(e) - 127, 128):
        if e[b] != 2: continue
        i = b + 4
        while i < b + e[b + 2]:
            tag, ln = e[i] >> 5, e[i] & 31
            if tag == 7 and ln >= 4 and e[i + 1] == 6 and e[i + 4]: return round(50 * 2 ** (e[i + 4] / 32))
            i += 1 + ln
    return 0
for f in sorted(glob.glob('/sys/class/drm/card*-*/edid')):
    try:
        if open(f[:-4] + 'status').read().strip() != 'connected': continue
        p = peak(open(f, 'rb').read())
        if p: print(p); break
    except Exception: pass
PY
)
  fi
  MSG="[detect] desktop HDR $([ "$DET_HDR" = 1 ] && echo on || echo off)"
  gfx_set HDR "$DET_HDR"
  if [ -n "$DET_PEAK" ] && [ "$DET_PEAK" -gt 100 ] 2>/dev/null; then
    # round to the nearest F1 menu choice
    best=1000; bd=999999; for c in 400 600 800 1000 1500 2000 4000; do d=$(( c > DET_PEAK ? c - DET_PEAK : DET_PEAK - c )); [ $d -lt $bd ] && bd=$d best=$c; done
    gfx_set HDR_PEAK "$best"; MSG="$MSG, peak $DET_PEAK nits -> HDR_PEAK = $best"
  else MSG="$MSG, peak not in the EDID (HDR_PEAK stays $(gfx HDR_PEAK 1000))"; fi
  gfx_set autodetect 0
  echo "$MSG -> HDR = $DET_HDR (saved to graphics.ini)"
fi

# Native HDR on Linux: an X11 window only gets HDR10 output inside gamescope, so HDR = 1 runs the game
# in a fullscreen HDR gamescope (ravenhdr.c presents HDR10 through Vulkan there). Without gamescope the
# game shows in SDR. HALO_NO_GAMESCOPE=1 skips it.
HDR_WRAP=()
if [ "$(gfx HDR 0)" = 1 ] && [ -z "$HALO_NO_GAMESCOPE" ] && [ -z "$GAMESCOPE_WAYLAND_DISPLAY" ] && [ -n "$WAYLAND_DISPLAY" ]; then
  if command -v gamescope >/dev/null; then
    HDR_WRAP=(gamescope --hdr-enabled -f)
    HZ=$(xrandr 2>/dev/null | awk '/\*/{for(i=2;i<=NF;i++) if ($i ~ /\*/) {gsub(/[*+]/,"",$i); print int($i+0.5); exit}}')
    [ -n "$HZ" ] && HDR_WRAP+=(-r "$HZ")
    HDR_WRAP+=(--)
  else
    echo "HDR = 1 but gamescope is not installed: the game shows in SDR (install gamescope for HDR10)." >&2
  fi
fi
