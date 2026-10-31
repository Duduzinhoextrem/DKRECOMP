/* Horizon kernel objects for the guest: threads (one host thread each), synchronisation, waits.
 *
 * One kernel lock serialises every SVC (horizon.c takes it around the SVC handler). A thread that blocks waits on
 * its own condition variable, which releases the lock while it sleeps; nothing sleeps while holding the lock.
 *
 * Semantics follow switchbrew's SVC page and the behaviour libnx relies on:
 *  - Mutex word: owner thread handle, bit 30 (0x40000000) set while threads wait. ArbitrateLock sleeps only if the
 *    word still reads "owner | waiters"; ArbitrateUnlock hands the mutex to the best-priority waiter (writing its
 *    handle, keeping bit 30 if others still wait) or writes 0.
 *  - WaitProcessWideKeyAtomic releases the mutex, sets the key word to 1 and sleeps. SignalProcessWideKey wakes up
 *    to `count` waiters (count <= 0: all) in priority order and re-acquires the mutex for each: a free mutex goes
 *    to the waiter, a taken one gets bit 30 and the waiter moves to its waiters. With no waiters left the key word
 *    is set to 0. A wait that times out returns 0xEA01 without the mutex (libnx then locks it itself).
 *  - Readable events stay signaled until cleared; a thread is signaled once it exited.
 * Guest code changes these words without the kernel (exclusive loads/stores), so the kernel changes them with
 * compare-and-swap only. Scheduling is the host's: no virtual cores or priority preemption yet. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define THREAD_BASE 0xC000u
#define MAX_THREADS 256
#define WAITERS_BIT 0x40000000u
#define TLR_SIZE 0x200

enum { W_NONE, W_MUTEX, W_CONDVAR, W_SYNC, W_SLEEP };

typedef struct KThread {
    int used, started, exited;
    uint32_t handle;
    uint64_t id;
    Cpu cpu;
    uint64_t entry, arg, stack_top, tlr;
    int prio, core;
    HANDLE host;
    CONDITION_VARIABLE cv;
    /* wait state */
    int wait_kind;
    uint64_t wait_addr;           /* mutex or condition variable key */
    uint64_t cv_mutex;            /* condition variable: the mutex to re-acquire */
    uint32_t cv_tag;              /* condition variable: the handle to write into the mutex */
    uint32_t mutex_tag;           /* ArbitrateLock's tag, handed over when the mutex becomes available */
    uint32_t sync_handles[64];
    int nsync;
    int woken;                    /* set by the waker */
    int sync_index;
    uint64_t seq;                 /* FIFO order among equal priorities */
    /* for the watchdog */
    const char *last_svc;
    uint64_t last_pc, svc_count;
} KThread;

static CRITICAL_SECTION g_kern;
static KThread g_threads[MAX_THREADS];
static uint64_t g_seq, g_tlr_base;
static _Thread_local KThread *t_self;
static uint64_t g_exit_hostfn;

void kern_lock(void) { EnterCriticalSection(&g_kern); }
void kern_unlock(void) { LeaveCriticalSection(&g_kern); }

static KThread *thread_of(uint32_t handle) {
    if (handle == 0xFFFF8000u) return t_self; /* current thread pseudo-handle */
    uint32_t i = handle - THREAD_BASE;
    return handle >= THREAD_BASE && i < MAX_THREADS && g_threads[i].used ? &g_threads[i] : NULL;
}

int kern_is_thread(uint32_t handle) { return thread_of(handle) != NULL; }

static KThread *alloc_thread(void) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (!g_threads[i].used) {
            KThread *t = &g_threads[i];
            memset(t, 0, sizeof *t);
            t->used = 1;
            t->handle = THREAD_BASE + (uint32_t)i;
            t->id = 0x100 + (uint64_t)i;
            t->tlr = g_tlr_base + (uint64_t)i * TLR_SIZE;
            memset((void *)(uintptr_t)t->tlr, 0, TLR_SIZE);
            InitializeConditionVariable(&t->cv);
            return t;
        }
    return NULL;
}

/* Called once, on the host thread that will run the guest main thread. */
uint32_t kern_init(Cpu **main_cpu, uint64_t stack_top, uint64_t tlr_base, int prio, int core,
                   uint64_t exit_hostfn) {
    InitializeCriticalSection(&g_kern);
    g_tlr_base = tlr_base;
    g_exit_hostfn = exit_hostfn;
    KThread *t = alloc_thread();
    t->started = 1;
    t->prio = prio;
    t->core = core;
    t->stack_top = stack_top;
    t->cpu.sp = stack_top;
    t->cpu.tpidrro = t->tlr;
    t_self = t;
    *main_cpu = &t->cpu;
    return t->handle;
}

/* ---- waiting and waking ---------------------------------------------------------------------------------- */
/* Sleep until woken or until `timeout_ns` passes (-1: forever). Kernel lock held on entry and on return.
 * Returns 1 if woken, 0 on timeout. */
static int wait_current(int64_t timeout_ns) {
    KThread *t = t_self;
    ULONGLONG duration = timeout_ns < 0 ? 0 : (ULONGLONG)timeout_ns / 1000000 + (timeout_ns % 1000000 != 0);
    ULONGLONG deadline = GetTickCount64() + duration;
    while (!t->woken) {
        DWORD ms = INFINITE;
        /* A signaled CV waiter moved to the mutex queue. Its timeout applies to the CV, not to re-locking. */
        if (timeout_ns >= 0 && t->wait_kind != W_MUTEX) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            ULONGLONG remaining = deadline - now;
            ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
        }
        SleepConditionVariableCS(&t->cv, &g_kern, ms);
    }
    int woken = t->woken;
    t->woken = 0;
    if (!woken) t->wait_kind = W_NONE;
    return woken;
}

static void wake(KThread *t) {
    t->wait_kind = W_NONE;
    t->woken = 1;
    WakeConditionVariable(&t->cv);
}

/* Best-priority (lowest number), then oldest waiter of `kind` on `addr`; counts the others in *rest. */
static KThread *best_waiter(int kind, uint64_t addr, int *rest) {
    KThread *best = NULL;
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = &g_threads[i];
        if (!t->used || t->wait_kind != kind || t->wait_addr != addr) continue;
        n++;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)) best = t;
    }
    if (rest) *rest = n ? n - 1 : 0;
    return best;
}

static uint32_t atomic_load32(uint64_t addr) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)(uintptr_t)addr, 0, 0);
}

/* ---- mutexes and condition variables ----------------------------------------------------------------------- */
uint32_t kern_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t requester) {
    if (addr & 3) return HOS_KERNEL(102);
    uint32_t v = atomic_load32(addr);
    if (v != (owner | WAITERS_BIT)) return HOS_OK; /* changed meanwhile: the guest retries */
    if (!thread_of(owner)) return HOS_INVALID_HANDLE;
    KThread *self = t_self;
    self->wait_kind = W_MUTEX;
    self->wait_addr = addr;
    self->mutex_tag = requester;
    self->seq = ++g_seq;
    wait_current(-1); /* ArbitrateUnlock wrote our handle into the mutex before waking us */
    return HOS_OK;
}

static void unlock_mutex(uint64_t addr) {
    int rest;
    KThread *w = best_waiter(W_MUTEX, addr, &rest);
    if (!w) {
        InterlockedExchange((volatile LONG *)(uintptr_t)addr, 0);
        return;
    }
    InterlockedExchange((volatile LONG *)(uintptr_t)addr, (LONG)(w->mutex_tag | (rest ? WAITERS_BIT : 0)));
    wake(w);
}

uint32_t kern_arbitrate_unlock(uint64_t addr) {
    if (addr & 3) return HOS_KERNEL(102);
    unlock_mutex(addr);
    return HOS_OK;
}

uint32_t kern_wait_cv(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout_ns) {
    if ((mutex | key) & 3) return HOS_KERNEL(102);
    KThread *self = t_self;
    unlock_mutex(mutex);
    st32(key, 1);
    self->wait_kind = W_CONDVAR;
    self->wait_addr = key;
    self->cv_mutex = mutex;
    self->cv_tag = tag;
    self->seq = ++g_seq;
    if (!wait_current(timeout_ns)) return HOS_TIMED_OUT;
    return HOS_OK;
}

uint32_t kern_signal_cv(uint64_t key, int32_t count) {
    if (key & 3) return HOS_KERNEL(102);
    int woken = 0;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *w = best_waiter(W_CONDVAR, key, NULL);
        if (!w) break;
        woken++;
        /* re-acquire the mutex for the waiter */
        uint64_t m = w->cv_mutex;
        for (;;) {
            uint32_t v = atomic_load32(m);
            if (v == 0) {
                if (rt_cas(m, 0, w->cv_tag, 4)) { wake(w); break; }
            } else {
                /* Guest unlocks run outside our kernel lock. If the owner released it meanwhile, retry the
                 * acquisition instead of setting the waiters bit on a free mutex and sleeping forever. */
                if (!rt_cas(m, v, v | WAITERS_BIT, 4)) continue;
                w->wait_kind = W_MUTEX; /* keeps sleeping; ArbitrateUnlock hands the mutex over later */
                w->wait_addr = m;
                w->mutex_tag = w->cv_tag;
                break;
            }
        }
    }
    if (!best_waiter(W_CONDVAR, key, NULL)) st32(key, 0);
    return HOS_OK;
}

/* ---- waitable objects -------------------------------------------------------------------------------------- */
static int object_signaled(uint32_t h) {
    KThread *t = thread_of(h);
    if (t && h != 0xFFFF8000u) return t->exited;
    int e = hos_event_state(h);
    return e > 0;
}

/* Called (lock held) whenever an object may have become signaled. */
void kern_sync_poke(uint32_t h) {
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = &g_threads[i];
        if (!t->used || t->wait_kind != W_SYNC) continue;
        for (int k = 0; k < t->nsync; k++)
            if (t->sync_handles[k] == h) { t->sync_index = k; wake(t); break; }
    }
}

/* Returns the result; *index gets the signaled handle's index. */
uint32_t kern_wait_sync(const uint32_t *handles, int count, int64_t timeout_ns, int32_t *index) {
    if (count < 0 || count > 64) return HOS_KERNEL(119); /* OutOfRange */
    for (int k = 0; k < count; k++) {
        if (handles[k] == 0xFFFF8000u || handles[k] == 0xFFFF8001u) return HOS_INVALID_HANDLE;
        int known = thread_of(handles[k]) || hos_event_state(handles[k]) >= 0;
        if (!known) return HOS_INVALID_HANDLE;
    }
    for (int k = 0; k < count; k++) {
        if (object_signaled(handles[k])) { *index = k; return HOS_OK; }
    }
    if (timeout_ns == 0) return HOS_TIMED_OUT;
    KThread *self = t_self;
    memcpy(self->sync_handles, handles, (size_t)count * 4);
    self->nsync = count;
    self->wait_kind = W_SYNC;
    if (!wait_current(timeout_ns)) return HOS_TIMED_OUT;
    *index = self->sync_index;
    return HOS_OK;
}

void kern_sleep(int64_t ns) {
    if (ns <= 0) {
        kern_unlock();
        SwitchToThread();
        kern_lock();
    } else {
        t_self->wait_kind = W_SLEEP;
        wait_current(ns);
    }
}

/* ---- threads ----------------------------------------------------------------------------------------------- */
static DWORD WINAPI host_thread_main(void *p) {
    KThread *t = p;
    t_self = t;
    rt_call(&t->cpu, t->entry);
    kern_lock();
    kern_exit_current();
    kern_unlock();
    return 0;
}

uint32_t kern_create_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core, uint32_t *out) {
    if (prio < 0 || prio > 63) return HOS_KERNEL(112); /* InvalidPriority */
    KThread *t = alloc_thread();
    if (!t) return HOS_KERNEL(103); /* OutOfResource */
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->prio = prio;
    t->core = core == -2 ? 0 : core;
    t->cpu.x[0] = arg;
    t->cpu.sp = stack_top;
    t->cpu.tpidrro = t->tlr;
    t->cpu.x[30] = g_exit_hostfn;
    *out = t->handle;
    return HOS_OK;
}

uint32_t kern_start_thread(uint32_t h) {
    KThread *t = thread_of(h);
    if (!t) return HOS_INVALID_HANDLE;
    if (t->started) return HOS_KERNEL(125); /* InvalidState */
    t->started = 1;
    /* 8 MiB of host stack: lifted code nests as deep as the guest calls */
    t->host = CreateThread(NULL, 8 << 20, host_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->host) t->started = 0;
    return t->host ? HOS_OK : HOS_KERNEL(103);
}

void kern_exit_current(void) {
    KThread *t = t_self;
    t->exited = 1;
    kern_sync_poke(t->handle);
}

uint32_t kern_thread_prio(uint32_t h, int set, int value, int *out) {
    KThread *t = thread_of(h);
    if (!t) return HOS_INVALID_HANDLE;
    if (set && (value < 0 || value > 63)) return HOS_KERNEL(112);
    if (set) t->prio = value;
    else *out = t->prio;
    return HOS_OK;
}

uint32_t kern_thread_core(uint32_t h, int set, int core, int *out) {
    KThread *t = thread_of(h);
    if (!t) return HOS_INVALID_HANDLE;
    if (set && core != -1 && core != -3) t->core = core == -2 ? 0 : core;
    if (!set) *out = t->core;
    return HOS_OK;
}

uint32_t kern_thread_id(uint32_t h, uint64_t *out) {
    KThread *t = thread_of(h);
    if (!t) return HOS_INVALID_HANDLE;
    *out = t->id;
    return HOS_OK;
}

int kern_current_core(void) { return t_self ? t_self->core : 0; }
uint32_t kern_current_handle(void) { return t_self ? t_self->handle : 0; }

void kern_note_svc(const char *name, uint64_t pc) {
    if (!t_self) return;
    t_self->last_svc = name;
    t_self->last_pc = pc;
    t_self->svc_count++;
}

/* Watchdog report: every thread's state. Takes the lock briefly; safe from any host thread. */
void kern_report(void) {
    static const char *kinds[] = {"running", "mutex", "condvar", "wait-sync", "sleep"};
    kern_lock();
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = &g_threads[i];
        if (!t->used) continue;
        char where[96];
        rt_describe(t->last_pc, where, sizeof where);
        hos_log("thread %#x id %llu prio %d core %d: %s%s, %llu SVCs, last %s at %s", t->handle,
                (unsigned long long)t->id, t->prio, t->core, !t->started ? "created" : t->exited ? "exited" : kinds[t->wait_kind],
                "", (unsigned long long)t->svc_count, t->last_svc ? t->last_svc : "-", where);
        if (t->wait_kind == W_MUTEX || t->wait_kind == W_CONDVAR)
            hos_log(" (on %#llx)", (unsigned long long)t->wait_addr);
        hos_log("\n");
    }
    kern_unlock();
}
