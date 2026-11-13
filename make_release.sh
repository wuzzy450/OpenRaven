#!/usr/bin/env bash
# Builds OpenRaven.zip for a GitHub release, the same way v1.0.x was made: every committed file inside one
# "OpenRaven/" folder. Works on Linux and on Windows (Git Bash / MSYS2).
#
#   ./make_release.sh            check + make OpenRaven.zip here (nothing is uploaded)
#
# It stops (instead of making a broken zip) when:
#   - something is not committed: git archive only packs commits, so new files would be left out
#   - openraven/win/ravenloader.exe is older than the Windows sources (rebuild it first: on Windows
#     `make` in openraven/win with MSYS2 clang64, on Linux openraven/win/build_from_linux.sh)
#   - openraven/rio2pad.so / ravengfx.so are older than their sources (Linux: openraven/build.sh)
# Upload afterwards with:  gh release create vX.Y.Z OpenRaven.zip --title "OpenRaven vX.Y.Z" --notes-file notes.md
set -e
cd "$(dirname "$0")"

fail=0
dirty=$(git status --porcelain --untracked-files=all -- . ':!ravenloader.log' ':!ravenloader_proc' ':!OpenRaven.zip' ':!openraven/graphics.ini' ':!stream.mp4' ':!openraven/stream.mp4' ':!openraven/run_logs' | grep -v '^?? .*\.\(log\|png\|o\)$' || true)
if [ -n "$dirty" ]; then
    echo "Not committed yet (the zip would not have these):"
    echo "$dirty" | sed -n 1,40p   # (sed reads everything: head would cut the pipe and stop the script)
    [ "$(echo "$dirty" | wc -l)" -gt 40 ] && echo "  ... and $(( $(echo "$dirty" | wc -l) - 40 )) more"
    fail=1
fi

newer() {   # newer TARGET SOURCES... : prints sources changed after TARGET
    local t=$1; shift
    for s in "$@"; do [ -e "$s" ] && [ "$s" -nt "$t" ] && echo "  $s"; done
    return 0
}
w=$(newer openraven/win/ravenloader.exe openraven/win/*.c openraven/win/*.h openraven/ravenfg.c openraven/ravenfsr.cpp openraven/vk/*.c)
if [ -n "$w" ]; then echo "openraven/win/ravenloader.exe is older than:"; echo "$w"; fail=1; fi
l=$(newer openraven/rio2pad.so openraven/rio2pad.c openraven/coop.c openraven/coopnet.c openraven/coopupnp.c openraven/streamweb.c openraven/streamweb.h; newer openraven/ravengfx.so openraven/ravengfx.c openraven/ravenhdr.c openraven/ravenui.c openraven/ravenfg.c openraven/ravengltrace.c openraven/streamenc.c)
if [ -n "$l" ]; then echo "Linux .so files are older than:"; echo "$l"; fail=1; fi
# (dual boot: if the clock was off, a fresh build can still look older - rebuild, or touch the binary)

if [ $fail = 1 ] && [ "$1" != "--force" ]; then
    echo; echo "No zip made. Fix the above (or ./make_release.sh --force to make it anyway)."
    exit 1
fi
rm -f OpenRaven.zip
git archive --format=zip --prefix=OpenRaven/ -o OpenRaven.zip HEAD
echo "OpenRaven.zip made from commit $(git rev-parse --short HEAD): $(git ls-files | wc -l) files, $(du -h OpenRaven.zip | cut -f1)"
