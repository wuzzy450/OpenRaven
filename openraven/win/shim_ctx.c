// getcontext / setcontext / makecontext / swapcontext with glibc's x86_64 ucontext_t layout.
// Same register set as glibc: callee-saved regs, rsp, rip, the first 6 argument registers,
// x87 environment and mxcsr. The signal mask is not touched (no signals on this side).
#include <stdint.h>
#include <stdlib.h>
#include "shim.h"

// glibc offsets (ucontext_i.sym)
#define oRBP 120
#define oRSP 160
#define oRBX 128
#define oR8 40
#define oR9 48
#define oR12 72
#define oR13 80
#define oR14 88
#define oR15 96
#define oRDI 104
#define oRSI 112
#define oRDX 136
#define oRCX 152
#define oRIP 168
#define oFPREGS 224
#define oFPREGSMEM 360
#define oMXCSR (oFPREGSMEM + 24)

#define S_(x) #x
#define S(x) S_(x)

SV int lx_getcontext(void *ucp);
SV int lx_setcontext(const void *ucp);
SV int lx_swapcontext(void *oucp, const void *ucp);
void lx_start_context(void);

// Save the caller's state into the context in %rdi (shared by getcontext and swapcontext).
#define SAVE_CTX \
    "movq %rbx, " S(oRBX) "(%rdi)\n movq %rbp, " S(oRBP) "(%rdi)\n" \
    "movq %r12, " S(oR12) "(%rdi)\n movq %r13, " S(oR13) "(%rdi)\n" \
    "movq %r14, " S(oR14) "(%rdi)\n movq %r15, " S(oR15) "(%rdi)\n" \
    "movq %rdi, " S(oRDI) "(%rdi)\n movq %rsi, " S(oRSI) "(%rdi)\n" \
    "movq %rdx, " S(oRDX) "(%rdi)\n movq %rcx, " S(oRCX) "(%rdi)\n" \
    "movq %r8, " S(oR8) "(%rdi)\n movq %r9, " S(oR9) "(%rdi)\n" \
    "movq (%rsp), %rcx\n movq %rcx, " S(oRIP) "(%rdi)\n" \
    "leaq 8(%rsp), %rcx\n movq %rcx, " S(oRSP) "(%rdi)\n" \
    "leaq " S(oFPREGSMEM) "(%rdi), %rcx\n movq %rcx, " S(oFPREGS) "(%rdi)\n" \
    "fnstenv (%rcx)\n fldenv (%rcx)\n stmxcsr " S(oMXCSR) "(%rdi)\n"

// Load the context in %rdx and jump to its rip.
#define LOAD_CTX \
    "movq " S(oFPREGS) "(%rdx), %rcx\n fldenv (%rcx)\n ldmxcsr " S(oMXCSR) "(%rdx)\n" \
    "movq " S(oRSP) "(%rdx), %rsp\n" \
    "movq " S(oRBX) "(%rdx), %rbx\n movq " S(oRBP) "(%rdx), %rbp\n" \
    "movq " S(oR12) "(%rdx), %r12\n movq " S(oR13) "(%rdx), %r13\n" \
    "movq " S(oR14) "(%rdx), %r14\n movq " S(oR15) "(%rdx), %r15\n" \
    "movq " S(oRIP) "(%rdx), %rcx\n pushq %rcx\n" \
    "movq " S(oRDI) "(%rdx), %rdi\n movq " S(oRSI) "(%rdx), %rsi\n" \
    "movq " S(oRCX) "(%rdx), %rcx\n movq " S(oR8) "(%rdx), %r8\n" \
    "movq " S(oR9) "(%rdx), %r9\n movq " S(oRDX) "(%rdx), %rdx\n" \
    "xorl %eax, %eax\n ret\n"

__asm__(
    ".text\n .globl lx_getcontext\n lx_getcontext:\n"
    SAVE_CTX
    "xorl %eax, %eax\n ret\n"

    ".globl lx_setcontext\n lx_setcontext:\n"
    "movq %rdi, %rdx\n"
    LOAD_CTX

    ".globl lx_swapcontext\n lx_swapcontext:\n"
    "movq %rsi, %rdx\n"   // SAVE_CTX only clobbers rcx, so the target stays in rdx
    SAVE_CTX
    LOAD_CTX

    // A makecontext function returned: rbx points at the uc_link slot.
    ".globl lx_start_context\n lx_start_context:\n"
    "movq %rbx, %rsp\n movq (%rsp), %rdi\n testq %rdi, %rdi\n je 1f\n"
    "call lx_setcontext\n"
    "1: andq $-16, %rsp\n call lx_context_done\n hlt\n"
);

// setjmp/longjmp, glibc __jmp_buf order: rbx, rbp, r12, r13, r14, r15, rsp, rip (no pointer
// mangling: only our own longjmp reads it). +64 = mask_was_saved, set to 0.
SV int lx_setjmp(void *env);
SV void lx_longjmp(void *env, int val);
__asm__(
    ".globl lx_setjmp\n lx_setjmp:\n"
    "movq %rbx, 0(%rdi)\n movq %rbp, 8(%rdi)\n movq %r12, 16(%rdi)\n movq %r13, 24(%rdi)\n"
    "movq %r14, 32(%rdi)\n movq %r15, 40(%rdi)\n leaq 8(%rsp), %rdx\n movq %rdx, 48(%rdi)\n"
    "movq (%rsp), %rdx\n movq %rdx, 56(%rdi)\n movl $0, 64(%rdi)\n xorl %eax, %eax\n ret\n"
    ".globl lx_longjmp\n lx_longjmp:\n"
    "movl %esi, %eax\n testl %eax, %eax\n jnz 1f\n incl %eax\n 1:\n"
    "movq 0(%rdi), %rbx\n movq 8(%rdi), %rbp\n movq 16(%rdi), %r12\n movq 24(%rdi), %r13\n"
    "movq 32(%rdi), %r14\n movq 40(%rdi), %r15\n movq 48(%rdi), %rsp\n jmpq *56(%rdi)\n"
);

SV void lx_context_done(void) {
    raven_log("makecontext function returned with no uc_link: exiting\n");
    exit(0);
}

// Non-variadic SysV signature reads the same registers/stack slots as the variadic call.
// Up to 6 integer arguments (all the game uses).
static SV void s_makecontext(uint8_t *ucp, void (*fn)(void), int argc,
                             uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (argc > 6) { raven_log("FATAL: makecontext with %d args\n", argc); exit(7); }
    uint8_t *ss_sp = *(uint8_t **)(ucp + 16);
    uint64_t ss_size = *(uint64_t *)(ucp + 32);
    uint64_t top = ((uint64_t)ss_sp + ss_size) & ~15ULL;
    static int n;
    if (n++ < 8) raven_log("makecontext(%p, fn=%p, argc=%d, stack=%p+%#llx, link=%p)\n", (void *)ucp, (void *)fn, argc,
                           (void *)ss_sp, (unsigned long long)ss_size, *(void **)(ucp + 8));
    uint64_t *sp = (uint64_t *)(top - 24);   // entry rsp = 8 mod 16, as after a call
    sp[0] = (uint64_t)lx_start_context;      // return address of fn
    sp[1] = *(uint64_t *)(ucp + 8);          // uc_link
    *(uint64_t *)(ucp + oRIP) = (uint64_t)fn;
    *(uint64_t *)(ucp + oRSP) = (uint64_t)sp;
    *(uint64_t *)(ucp + oRBX) = (uint64_t)&sp[1];
    uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    static const int off[6] = {oRDI, oRSI, oRDX, oRCX, oR8, oR9};
    for (int i = 0; i < argc; i++) *(uint64_t *)(ucp + off[i]) = args[i];
}

const shim_entry shims_ctx[] = {
    {"getcontext", lx_getcontext}, {"setcontext", lx_setcontext},
    {"swapcontext", lx_swapcontext}, {"makecontext", s_makecontext},
    // __sigsetjmp(env, savemask): same as setjmp, signal mask ignored
    {"setjmp", lx_setjmp}, {"_setjmp", lx_setjmp}, {"__sigsetjmp", lx_setjmp},
    {"longjmp", lx_longjmp}, {"_longjmp", lx_longjmp}, {"siglongjmp", lx_longjmp},
    {"__longjmp_chk", lx_longjmp},
    {NULL, NULL},
};
