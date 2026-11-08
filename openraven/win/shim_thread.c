// pthread + POSIX semaphore shims over Win32. Game-side objects keep their
// Linux sizes; we store our Win32 state inside them.
#include "shim.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef SV void *(*sv_start_fn)(void *);

typedef struct lx_thread {
    HANDLE h;
    sv_start_fn fn;
    void *arg, *ret;
} lx_thread;

static __thread lx_thread *t_self;
extern void raven_thread_setup(void);  // main.c: canary slot + FPU mode

static DWORD WINAPI thread_main(void *p) {
    lx_thread *t = p;
    t_self = t;
    raven_thread_setup();
    t->ret = t->fn(t->arg);
    return 0;
}

// pthread_attr_t (56 bytes): we use [0]=detached, [1]=stack size.
static SV int s_attr_init(uint64_t *a) { memset(a, 0, 56); return 0; }
static SV int s_attr_setdetachstate(uint64_t *a, int d) { a[0] = d; return 0; }
static SV int s_attr_setstacksize(uint64_t *a, size_t s) { a[1] = s; return 0; }

static SV int s_create(uint64_t *tid, uint64_t *attr, sv_start_fn fn, void *arg) {
    lx_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    size_t stack = attr && attr[1] ? attr[1] : 8 << 20;  // Linux default 8 MB
    // Commit the whole stack: Linux code has no stack probes, so a frame > 4 KB would jump
    // over the guard page of a reserve-only stack and crash.
    t->h = CreateThread(NULL, stack, thread_main, t, 0, NULL);
    if (!t->h) { free(t); return LX_EAGAIN; }
    *tid = (uint64_t)t;
    raven_log("pthread_create(%p) -> %p\n", (void *)fn, (void *)t);
    return 0;
}
static SV int s_join(uint64_t tid, void **ret) {
    lx_thread *t = (lx_thread *)tid;
    if (!t || !t->h) return 3;  // ESRCH: the game joins workers it never started
    WaitForSingleObject(t->h, INFINITE);
    if (ret) *ret = t->ret;
    CloseHandle(t->h);
    return 0;
}
static SV uint64_t s_self(void) {
    if (!t_self) t_self = calloc(1, sizeof *t_self);  // main / foreign thread
    return (uint64_t)t_self;
}
static SV int s_equal(uint64_t a, uint64_t b) { return a == b; }
static SV int s_cancel(uint64_t t) { raven_log("pthread_cancel(%p) ignored\n", (void *)t); return 0; }

// pthread_mutex_t (40 bytes): first 8 bytes hold a CRITICAL_SECTION*, created
// lazily so PTHREAD_MUTEX_INITIALIZER (all zero) works.
static CRITICAL_SECTION *mtx(void **m) {
    CRITICAL_SECTION *cs = *m;
    if (cs) return cs;
    cs = malloc(sizeof *cs);
    InitializeCriticalSectionAndSpinCount(cs, 1000);
    CRITICAL_SECTION *prev = InterlockedCompareExchangePointer(m, cs, NULL);
    if (prev) { DeleteCriticalSection(cs); free(cs); return prev; }
    return cs;
}
static SV int s_mutex_init(void **m, void *a) { (void)a; *m = NULL; mtx(m); return 0; }
static SV int s_mutex_destroy(void **m) {
    if (*m) { DeleteCriticalSection(*m); free(*m); *m = NULL; }
    return 0;
}
static SV int s_mutex_lock(void **m) { EnterCriticalSection(mtx(m)); return 0; }
static SV int s_mutex_unlock(void **m) { LeaveCriticalSection(mtx(m)); return 0; }

// pthread_cond_t (48 bytes): a CONDITION_VARIABLE fits at offset 0, zero = ready.
static SV int s_cond_init(CONDITION_VARIABLE *c, void *a) { (void)a; InitializeConditionVariable(c); return 0; }
static SV int s_cond_destroy(CONDITION_VARIABLE *c) { (void)c; return 0; }
static SV int s_cond_signal(CONDITION_VARIABLE *c) { WakeConditionVariable(c); return 0; }
static SV int s_cond_broadcast(CONDITION_VARIABLE *c) { WakeAllConditionVariable(c); return 0; }
static SV int s_cond_wait(CONDITION_VARIABLE *c, void **m) {
    SleepConditionVariableCS(c, mtx(m), INFINITE);
    return 0;
}
typedef struct { long long sec, nsec; } lx_timespec;
static SV int s_cond_timedwait(CONDITION_VARIABLE *c, void **m, const lx_timespec *abs) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    long long now_us = (long long)(((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ULL);
    long long want_us = abs->sec * 1000000 + abs->nsec / 1000;
    long long ms = (want_us - now_us + 999) / 1000;
    if (ms < 0) ms = 0;
    if (!SleepConditionVariableCS(c, mtx(m), (DWORD)ms))
        return GetLastError() == ERROR_TIMEOUT ? LX_ETIMEDOUT : LX_EINVAL;
    return 0;
}

// pthread_rwlock_t (56 bytes): SRWLOCK at offset 0, writer flag at offset 8.
typedef struct { SRWLOCK l; volatile LONG writer; } rw;
static SV int s_rw_init(rw *r, void *a) { (void)a; InitializeSRWLock(&r->l); r->writer = 0; return 0; }
static SV int s_rw_destroy(rw *r) { (void)r; return 0; }
static SV int s_rw_rdlock(rw *r) { AcquireSRWLockShared(&r->l); return 0; }
static SV int s_rw_wrlock(rw *r) { AcquireSRWLockExclusive(&r->l); r->writer = 1; return 0; }
static SV int s_rw_unlock(rw *r) {
    if (r->writer) { r->writer = 0; ReleaseSRWLockExclusive(&r->l); }
    else ReleaseSRWLockShared(&r->l);
    return 0;
}

// sem_t (32 bytes): HANDLE at offset 0.
static SV int s_sem_init(HANDLE *s, int pshared, unsigned v) {
    (void)pshared;
    *s = CreateSemaphoreA(NULL, (LONG)v, 0x7fffffff, NULL);
    return *s ? 0 : -1;
}
static SV int s_sem_wait(HANDLE *s) { return WaitForSingleObject(*s, INFINITE) == WAIT_OBJECT_0 ? 0 : -1; }
static SV int s_sem_post(HANDLE *s) { return ReleaseSemaphore(*s, 1, NULL) ? 0 : -1; }
static SV HANDLE *s_sem_open(const char *name, int oflag, int mode, unsigned v) {
    (void)oflag; (void)mode;
    char n[MAX_PATH];
    snprintf(n, sizeof n, "Local\\raven_%s", name[0] == '/' ? name + 1 : name);
    HANDLE *s = calloc(1, 32);
    *s = CreateSemaphoreA(NULL, (LONG)v, 0x7fffffff, n);
    raven_log("sem_open(%s) -> %p\n", name, *s);
    if (!*s) { free(s); return NULL; }
    return s;
}
static SV int s_sem_close(HANDLE *s) { CloseHandle(*s); free(s); return 0; }
static SV int s_ok(void) { return 0; }

// ---- extras: trylock, keys, once, detach/exit, sem variants ----
static SV int s_mutex_trylock(void **m) { return TryEnterCriticalSection(mtx(m)) ? 0 : 16; /* EBUSY */ }
static SV int s_key_create(DWORD *k, void *dtor) {
    (void)dtor;  // destructors not run
    DWORD i = TlsAlloc();
    if (i == TLS_OUT_OF_INDEXES) return LX_EAGAIN;
    *k = i;
    return 0;
}
static SV int s_key_delete(DWORD k) { TlsFree(k); return 0; }
static SV void *s_getspecific(DWORD k) { return TlsGetValue(k); }
static SV int s_setspecific(DWORD k, void *v) { return TlsSetValue(k, v) ? 0 : LX_EINVAL; }
// pthread_once_t (int): 0 = not run, 1 = running, 2 = done.
static SV int s_once(volatile LONG *o, SV void (*fn)(void)) {
    if (*o == 2) return 0;
    if (InterlockedCompareExchange(o, 1, 0) == 0) { fn(); InterlockedExchange(o, 2); return 0; }
    while (*o != 2) Sleep(0);
    return 0;
}
static SV int s_detach(uint64_t tid) {
    lx_thread *t = (lx_thread *)tid;
    if (t && t->h) { CloseHandle(t->h); t->h = NULL; }
    return 0;
}
static SV void s_exit(void *ret) { raven_log("pthread_exit(%p)\n", ret); if (t_self) t_self->ret = ret; ExitThread(0); }
static SV int s_timedjoin(uint64_t tid, void **ret, const lx_timespec *abs) {
    lx_thread *t = (lx_thread *)tid;
    if (!t || !t->h) return 3;  // ESRCH
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    long long now_us = (long long)(((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ULL);
    long long ms = (abs->sec * 1000000 + abs->nsec / 1000 - now_us + 999) / 1000;
    if (WaitForSingleObject(t->h, ms < 0 ? 0 : (DWORD)ms) != WAIT_OBJECT_0) return LX_ETIMEDOUT;
    if (ret) *ret = t->ret;
    CloseHandle(t->h);
    return 0;
}
static SV int s_sem_destroy(HANDLE *s) { CloseHandle(*s); return 0; }
static SV int s_sem_trywait(HANDLE *s) {
    if (WaitForSingleObject(*s, 0) == WAIT_OBJECT_0) return 0;
    lx_errno = LX_EAGAIN;
    return -1;
}
static SV int s_sem_timedwait(HANDLE *s, const lx_timespec *abs) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    long long now_us = (long long)(((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ULL);
    long long ms = (abs->sec * 1000000 + abs->nsec / 1000 - now_us + 999) / 1000;
    if (WaitForSingleObject(*s, ms < 0 ? 0 : (DWORD)ms) == WAIT_OBJECT_0) return 0;
    lx_errno = LX_ETIMEDOUT;
    return -1;
}
typedef LONG (NTAPI *nqs_fn)(HANDLE, int, void *, ULONG, ULONG *);
static SV int s_sem_getvalue(HANDLE *s, int *v) {
    static nqs_fn nqs;
    if (!nqs) nqs = (nqs_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySemaphore");
    struct { LONG cur, max; } info = {0, 0};
    if (nqs) nqs(*s, 0, &info, sizeof info, NULL);
    *v = info.cur;
    return 0;
}
// sched affinity: report all CPUs (cpu_set_t is 128 bytes).
static SV int s_getaffinity(int pid, size_t sz, unsigned char *set) {
    (void)pid;
    memset(set, 0, sz);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    for (DWORD i = 0; i < si.dwNumberOfProcessors && i / 8 < sz; i++) set[i / 8] |= 1 << (i % 8);
    return 0;
}

const shim_entry shims_thread[] = {
    {"pthread_attr_init", s_attr_init}, {"pthread_attr_destroy", s_ok},
    {"pthread_attr_setdetachstate", s_attr_setdetachstate},
    {"pthread_attr_setstacksize", s_attr_setstacksize},
    {"pthread_create", s_create}, {"pthread_join", s_join}, {"pthread_self", s_self},
    {"pthread_equal", s_equal}, {"pthread_cancel", s_cancel},
    {"pthread_mutex_init", s_mutex_init}, {"pthread_mutex_destroy", s_mutex_destroy},
    {"pthread_mutex_lock", s_mutex_lock}, {"pthread_mutex_unlock", s_mutex_unlock},
    {"pthread_cond_init", s_cond_init}, {"pthread_cond_destroy", s_cond_destroy},
    {"pthread_cond_signal", s_cond_signal}, {"pthread_cond_broadcast", s_cond_broadcast},
    {"pthread_cond_wait", s_cond_wait}, {"pthread_cond_timedwait", s_cond_timedwait},
    {"pthread_rwlock_init", s_rw_init}, {"pthread_rwlock_destroy", s_rw_destroy},
    {"pthread_rwlock_rdlock", s_rw_rdlock}, {"pthread_rwlock_wrlock", s_rw_wrlock},
    {"pthread_rwlock_unlock", s_rw_unlock},
    {"sem_init", s_sem_init}, {"sem_wait", s_sem_wait}, {"sem_post", s_sem_post},
    {"sem_open", s_sem_open}, {"sem_close", s_sem_close}, {"sem_unlink", s_ok},
    {"pthread_mutex_trylock", s_mutex_trylock},
    {"pthread_mutexattr_init", s_ok}, {"pthread_mutexattr_destroy", s_ok},
    {"pthread_mutexattr_settype", s_ok},  // CRITICAL_SECTION is always recursive
    {"pthread_key_create", s_key_create}, {"__pthread_key_create", s_key_create},
    {"pthread_key_delete", s_key_delete},
    {"pthread_getspecific", s_getspecific}, {"pthread_setspecific", s_setspecific},
    {"pthread_once", s_once}, {"pthread_detach", s_detach}, {"pthread_exit", s_exit},
    {"pthread_timedjoin_np", s_timedjoin}, {"pthread_kill", s_ok},
    {"pthread_setcanceltype", s_ok}, {"pthread_attr_setinheritsched", s_ok},
    {"pthread_attr_setschedparam", s_ok}, {"pthread_attr_setschedpolicy", s_ok},
    {"pthread_attr_setstack", s_ok},
    {"sem_destroy", s_sem_destroy}, {"sem_trywait", s_sem_trywait},
    {"sem_timedwait", s_sem_timedwait}, {"sem_getvalue", s_sem_getvalue},
    {"sched_getaffinity", s_getaffinity}, {"sched_setaffinity", s_ok},
    {NULL, NULL},
};
