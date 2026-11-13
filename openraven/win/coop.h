// Shared by rio2pad_win.c (inputs), coop_win.c (clock, frame tick) and coopnet_win.c (network).
#pragma once
#include <stdint.h>
#define COOP_PLAYERS 4
// One frame of input for every player: bits = CI_* (rio2pad_win.c), aim 0..1.
struct coop_in { uint8_t bits[COOP_PLAYERS]; float aim[COOP_PLAYERS][2]; };
int coopnet_active(void);
void coopnet_init(void);
void coopnet_start(void);
void coopnet_frame(unsigned frame, uint8_t bits, float ax, float ay, struct coop_in *out);
