// Shared definitions for the Linux->Windows shim layer.
// Every function the game calls is SysV ABI, so shims are declared SV.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define SV __attribute__((sysv_abi))

typedef struct { const char *name; void *fn; } shim_entry;

// Each shim_*.c file exports one table, terminated by {NULL, NULL}.
extern const shim_entry shims_core[], shims_fmt[], shims_thread[], shims_dl[], shims_libc2[], shims_locale[], shims_x11[], shims_glx[], shims_gl[], shims_devices[], shims_ctx[], shims_fmod[], shims_glhook[];
extern const shim_entry shims_fmodlx[], shims_alsa[];   // shim_fmodlx.c (Linux FMOD wrappers), shim_alsa.c (fake libasound -> WASAPI)

void *shim_lookup(const char *name);
void *shim_missing_stub(const char *name);  // thunk that logs + aborts when called
void raven_log(const char *fmt, ...);
// Loader setting from the Win32 environment (the game can change the CRT one). NULL if unset.
const char *raven_env(const char *name);

// Linux errno lives in our own thread-local int.
int *lx_errno_ptr(void);
#define lx_errno (*lx_errno_ptr())
int lx_errno_from_crt(int e);
void lx_set_errno_from_crt(void);

// Linux absolute path -> Windows path under the game root. Returns buf.
const char *lx_path(const char *p, char *buf, size_t cap);
extern char g_root[];  // folder that contains pm/
void raven_set_root(const char *root);

// Linux constants we need on the Windows side.
#define LX_ENOENT 2
#define LX_EBADF 9
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EINVAL 22
#define LX_ENOTTY 25
#define LX_ENOSYS 38
#define LX_ETIMEDOUT 110
