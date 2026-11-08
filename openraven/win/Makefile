# Build with MSYS2 clang64:  make
CC = clang
CFLAGS = -O2 -Wall -Wextra -std=c11
CXX = clang++
# FSR 2 (ravenfsr.cpp is shared with the Linux build; library: ../fsr2/src, shaders ../fsr2/gen)
FSR = ../fsr2
FSRFLAGS = -std=c++17 -O2 -w -I $(FSR)/gen -I $(FSR)/src -I $(FSR)/src/gl/external/glad/include
FSROBJS = fsr_ffx_fsr2.o fsr_ffx_assert.o fsr_ffx_fsr2_gl.o fsr_ffx_fsr2_shaders_gl.o
# Keep our own image far from the game's fixed 0x400000 range.
LDFLAGS = -fuse-ld=lld -Wl,--image-base=0x7ff600000000 -Wl,--disable-dynamicbase -lopengl32 -lglu32 -lgdi32 -luser32 -lwinmm -ld3d11 -ldxgi -ldxguid -lole32
OBJS = main.o elfload.o shim_core.o shim_fmt.o shim_thread.o shim_dl.o shim_libc2.o shim_locale.o shim_x11.o shim_glx.o gl_thunks.o shim_devices.o rio2pad_win.o shim_ctx.o shim_fmod.o shim_fmodlx.o shim_alsa.o ravenrec_win.o ravengfx_win.o ravenui_win.o gl_hooks.o ravenhdr_win.o ravenfg.o ravenfsr.o $(FSROBJS)
ravenloader.exe: $(OBJS)
	$(CXX) -static -o $@ $(OBJS) $(LDFLAGS)
ravenfg.o: ../ravenfg.c
	$(CC) -O2 -w -c $< -o $@
ravenfsr.o: ../ravenfsr.cpp
	$(CXX) $(FSRFLAGS) -c $< -o $@
fsr_%.o: $(FSR)/src/%.cpp
	$(CXX) $(FSRFLAGS) -c $< -o $@
fsr_%.o: $(FSR)/src/gl/%.cpp
	$(CXX) $(FSRFLAGS) -c $< -o $@
fsr_%.o: $(FSR)/src/gl/shaders/%.cpp
	$(CXX) $(FSRFLAGS) -c $< -o $@
%.o: %.c elfload.h elf.h shim.h x11_shim.h
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f ravenloader.exe *.o
gl_thunks.c: gen_gl.py
	python gen_gl.py /c/msys64/clang64/include/GL > $@
