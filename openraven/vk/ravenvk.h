// ravenvk - the neural detail network (train5.py U-Net) in Vulkan, shared with the game's OpenGL frame.
// Matrix-core kernels (VK_KHR_cooperative_matrix) where the GPU has them, plain fp32 kernels otherwise.
#pragma once
#include <stdint.h>

typedef struct {
    int nl;                                   // layers
    const int *src0, *mode0, *src1, *cin, *cout;
    const float *const *w, *const *b;         // w[l] = [cout][cin][3][3], b[l] = [cout]
} rvk_net;

typedef struct {
    int ok, coopmat, acc16;                   // after rvk_init
    char gpu[256], why[256];                  // device name / reason it is off
    int in_fd, out_fd, sem_in_fd, sem_out_fd; // exported to GL (-1 when not exported) - Linux
    void *in_h, *out_h, *sem_in_h, *sem_out_h; // exported to GL as Win32 NT handles - Windows
    uint64_t in_size, out_size;               // bytes: W*H*8 (RGBA16F) each, allocation sizes
    double ms;                                // GPU time of one run of the whole net (after autotune)
    int n_conv, n_acc16, n_acc32;             // autotune result: conv layers, how many picked fp16 / fp32 accumulate
} rvk_info;

// W x H = full frame (even sizes). uuid = GL_DEVICE_UUID_EXT (16 bytes) to pick the same GPU, or NULL.
// mode: 0 auto, 1 force plain kernels, 2 force matrix cores. export_gl: create fd-exportable memory/semaphores.
int rvk_init(const rvk_net *net, int W, int H, const uint8_t *uuid, int mode, int export_gl, rvk_info *info);
// Runs the net on the input buffer (RGBA16F frame, GL row order) into the output buffer (RGBA16F,
// top-down rows: rgb = residual, a = the input's 3x3 contrast). With export_gl it waits for the "in"
// semaphore and signals the "out" one (the GL side signals / waits them); otherwise it blocks until done.
int rvk_run(void);
// Host access for tests (non-exported mode only).
int rvk_upload(const void *rgba16f);
int rvk_download(void *rgba16f);
void rvk_destroy(void);
// Matrix-core accumulate (set before rvk_init): 0 auto = time both, keep the faster per layer; 1 fp16; 2 fp32.
extern int rvk_acc_pref;
