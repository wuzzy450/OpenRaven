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
for v in RAVENGFX_SHOT RAVENGFX_SHOTDIR RAVENGFX_FPSLOG RAVENGFX_OVERLAY RAVENGFX_UIKEYS RAVENGFX_TANLOG RAVENGFX_HORPLUS_SITE RAVENGFX_HORPLUS_K RAVENGFX_GLTRACE RAVENGFX_PROGSRC RAVENGFX_SWAPINTERVAL RAVENGFX_FPSCAP RAVENFSR_FP16 RAVENFSR_GLDEBUG RAVENFSR_FLAGS RAVENFSR_JSX RAVENFSR_JSY RAVENFSR_MSX RAVENFSR_MSY RAVENGFX_TAADUMP RAVENNN RAVENNN_DUMP RAVENNN_BACKEND RAVENNN_VKTEST RAVENVK_DEVICE RAVENFG RAVENFG_EVAL RAVENGFX_BLOOMFIX RAVENGFX_TEXSTAT RAVENFG_FORCE RAVENGFX_NOGLHOOK; do
  [ -n "${!v}" ] && GFX_BWRAP+=(--setenv "$v" "${!v}")
done
# Launcher/cabinet/input options (env vars win over graphics.ini).
export HALO_VOLUME="${HALO_VOLUME:-$(gfx volume 100%)}"
[ -z "$RIO2PAD_AIM" ] && [ "$(gfx aim cursor)" = abs ] && export RIO2PAD_AIM=abs
[ -z "$RIO2PAD_STICKSPEED" ] && export RIO2PAD_STICKSPEED="$(gfx stick_speed 0.25)"
[ -z "$RIO2PAD_FREEPLAY" ] && [ "$(gfx free_play 1)" = 0 ] && export RIO2PAD_FREEPLAY=0
[ -z "$RIO2PAD_SKIPBOOT" ] && export RIO2PAD_SKIPBOOT="$(gfx skip_boot 1)"
for v in RAVENGFX_UISCRIPT RAVENGFX_UIKEY RAVENGFX_NOUI; do
  [ -n "${!v}" ] && GFX_BWRAP+=(--setenv "$v" "${!v}")
done
# VSync: NVIDIA's driver-level switch too, so the game's own swap-interval calls can't turn it off.
case "$(gfx VSYNC 0)" in 1) GFX_BWRAP+=(--setenv __GL_SYNC_TO_VBLANK 1);; 0) GFX_BWRAP+=(--setenv __GL_SYNC_TO_VBLANK 0);; esac
