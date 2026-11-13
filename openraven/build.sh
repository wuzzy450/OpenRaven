#!/bin/bash
# Build ravengfx.so (graphics shim + F1 overlay + GL hooks + FSR 2). FSR 2 library: fsr2/build_fsr2.sh
set -e; cd "$(dirname "$0")"; O=fsr2/obj; mkdir -p $O
# rio2pad.so: pads/cabinet input + online coop (clock, lockstep network, router port)
gcc -O2 -Wall -shared -fPIC -Wno-nonnull-compare -o ${OUT_PAD:-rio2pad.so} rio2pad.c coop.c coopnet.c coopupnp.c streamweb.c -lpthread -ldl -lm
echo built rio2pad.so
# Vulkan neural network kernels -> vk/ravenvk_spv.h (needs glslc; the committed header is used otherwise)
if command -v glslc >/dev/null; then (cd vk && for s in conv_cm.comp conv_plain.comp misc2.comp; do [ $s -nt ravenvk_spv.h ] && { ./build_vk.sh >/dev/null; break; }; done; true); fi
for f in ravengfx ravenui ravengltrace ravenfg ravenhdr streamenc; do gcc -O2 -Wall -fPIC -c $f.c -o $O/$f.o; done
gcc -O2 -Wall -fPIC -c vk/ravenvk.c -o $O/ravenvk.o
g++ -std=c++17 -O2 -fPIC -Wall -Wno-misleading-indentation -include fsr2/src/ffx_linux.h -I fsr2/src -I fsr2/src/gl/external/glad/include -c ravenfsr.cpp -o $O/ravenfsr.o
g++ -shared -o ${OUT:-ravengfx.so} $O/ravengfx.o $O/ravenui.o $O/ravengltrace.o $O/ravenfg.o $O/ravenhdr.o $O/streamenc.o $O/ravenvk.o $O/ravenfsr.o fsr2/libfsr2gl.a \
    -static-libstdc++ -static-libgcc -ldl -lpthread -lm -Wl,--no-undefined
echo built ravengfx.so
