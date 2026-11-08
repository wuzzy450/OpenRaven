// Shared between shim_x11.c and shim_glx.c.
#pragma once
#include <windows.h>
#include <stdint.h>
HWND x11_hwnd(uint64_t xwindow);   // NULL for root / unknown
void *x11_new_vinfo(void);         // malloc'd XVisualInfo (caller XFree's it)
int x11_req_size(uint64_t xwindow, int *w, int *h);  // size the game asked for
int x11_screen_w(void);
int x11_screen_h(void);
