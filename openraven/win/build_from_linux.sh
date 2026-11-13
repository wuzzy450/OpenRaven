#!/usr/bin/env bash
# Builds ravenloader.exe (the Windows version) on Linux, with llvm-mingw: the same clang + lld + mingw-w64
# headers that MSYS2 clang64 uses on Windows, so the Makefile works unchanged.
#
# One-time setup (no root needed):
#   cd ~ && curl -LO https://github.com/mstorsjo/llvm-mingw/releases/download/20241119/llvm-mingw-20241119-ucrt-ubuntu-20.04-x86_64.tar.xz
#   tar xf llvm-mingw-20241119-ucrt-ubuntu-20.04-x86_64.tar.xz
#   (any newer llvm-mingw release works too; set LLVM_MINGW to its folder if it is not the one below)
#
# Then:  openraven/win/build_from_linux.sh          (clean build)
# Test the exe on Windows (or under Wine for a quick "does it start" check) before a release.
set -e
cd "$(dirname "$0")"
LLVM_MINGW=${LLVM_MINGW:-$(ls -d ~/llvm-mingw-*-ucrt-*/ 2>/dev/null | sort | tail -1)}
if [ -z "$LLVM_MINGW" ] || [ ! -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ]; then
    echo "llvm-mingw not found: see the setup lines at the top of $0"; exit 1
fi
export PATH="$LLVM_MINGW/bin:$PATH"
# gl_thunks.c is committed; it is only regenerated on Windows (needs the MSYS2 GL headers).
touch gl_thunks.c
make clean
# MSYS2 clang64 targets Windows 10 by default; llvm-mingw does not (DPI and Media Foundation calls)
W10="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000"
# Vulkan headers (MSYS2 has them in its include dir): use the system's copy, only those two folders
VKH=$(mktemp -d); ln -s /usr/include/vulkan "$VKH/vulkan"; [ -d /usr/include/vk_video ] && ln -s /usr/include/vk_video "$VKH/vk_video"
[ -f /usr/include/vulkan/vulkan.h ] || { echo "needs the Vulkan headers (package libvulkan-dev / vulkan-headers)"; exit 1; }
W10="$W10 -isystem $VKH"
make -j"$(nproc)" CC="x86_64-w64-mingw32-clang $W10" CXX="x86_64-w64-mingw32-clang++ $W10"
ls -la ravenloader.exe
