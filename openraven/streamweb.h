// Browser stream (streamweb.c in rio2pad.so; the video encoder streamenc.c in ravengfx.so finds these
// with dlsym). Same interface as win/streamweb_win.c, with the browser's buttons as plain bits.
#pragma once
#include <stddef.h>
struct streampad { unsigned char buttons; };   // 1 fire, 2 reload, 4 grenade, 8 start, 16 back
int streamweb_active(void);
int streamweb_viewers(void);
int streamweb_take_key_request(void);
int streamweb_players(void);
int streamweb_pad(int p, struct streampad *st, float *ax, float *ay);
void streamweb_start(void);
void streamweb_video(const unsigned char *init, size_t ninit, const unsigned char *frag, size_t n, int key);
void streamweb_audio(const float *lr, unsigned frames, unsigned rate);
void streamweb_hook_alsa(void);
