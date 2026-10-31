/* Synthetic guest threads exercise the Horizon mutex protocol, condition variables and waitable objects.
 * No game code or game data is needed. Host events coordinate the test so the intended waits really happen. */
#include "../../runtime/src/hos.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WAITERS 0x40000000u
#define NS_PER_MS 1000000ll
static volatile LONG failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
    InterlockedIncrement(&failures); \
} } while (0)

void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(2);
}
Service *hos_get_service(const char *name) { (void)name; return NULL; }

static uint64_t addr(const void *p) { return (uint64_t)(uintptr_t)p; }
static uint32_t load_word(uint32_t *p) { return (uint32_t)InterlockedCompareExchange((volatile LONG *)p, 0, 0); }

/* The guest's fast path uses CAS; only contention enters the Horizon kernel. */
static void mutex_lock(uint32_t *word, HANDLE queued) {
    uint32_t self = kern_current_handle();
    for (;;) {
        uint32_t v = load_word(word);
        if (!v) {
            if (rt_cas(addr(word), 0, self, 4)) return;
            continue;
        }
        if (!(v & WAITERS) && !rt_cas(addr(word), v, v | WAITERS, 4)) continue;
        kern_lock();
        if (queued) SetEvent(queued);
        CHECK(kern_arbitrate_lock(v & ~WAITERS, addr(word), self) == HOS_OK);
        kern_unlock();
        if ((load_word(word) & ~WAITERS) == self) return;
    }
}

static void mutex_unlock(uint32_t *word) {
    uint32_t self = kern_current_handle();
    CHECK((load_word(word) & ~WAITERS) == self);
    if (rt_cas(addr(word), self, 0, 4)) return;
    kern_lock();
    CHECK(kern_arbitrate_unlock(addr(word)) == HOS_OK);
    kern_unlock();
}

static uint32_t start(GuestFn fn, void *arg, int priority) {
    uint64_t entry = rt_hostfn_register("test worker", fn);
    uint32_t handle = 0;
    kern_lock();
    CHECK(kern_create_thread(entry, addr(arg), 0, priority, 0, &handle) == HOS_OK);
    CHECK(kern_start_thread(handle) == HOS_OK);
    CHECK(kern_start_thread(handle) == HOS_KERNEL(125));
    kern_unlock();
    return handle;
}

static void join(uint32_t handle) {
    int32_t index = -1;
    kern_lock();
    uint32_t result = kern_wait_sync(&handle, 1, 10000 * NS_PER_MS, &index);
    kern_unlock();
    CHECK(result == HOS_OK && index == 0);
    if (result) exit(2); /* do not leave a stuck worker accessing a caller's stack */
}

typedef struct Contender {
    uint32_t *mutex;
    int *counter;
    HANDLE queued;
    uint64_t tlr;
    uint32_t handle;
} Contender;

static void increment(Cpu *c) {
    Contender *a = (Contender *)(uintptr_t)c->x[0];
    a->tlr = c->tpidrro;
    a->handle = kern_current_handle();
    mutex_lock(a->mutex, a->queued);
    for (int i = 0; i < 2000; i++) {
        (*a->counter)++;
        mutex_unlock(a->mutex);
        if (i != 1999) mutex_lock(a->mutex, NULL);
    }
}

static void test_contention(void) {
    enum { N = 6 };
    uint32_t mutex = 0, handles[N];
    int counter = 0;
    Contender args[N];
    mutex_lock(&mutex, NULL);
    for (int i = 0; i < N; i++) {
        args[i] = (Contender){&mutex, &counter, CreateEventW(NULL, TRUE, FALSE, NULL), 0, 0};
        handles[i] = start(increment, &args[i], 44);
        CHECK(WaitForSingleObject(args[i].queued, 2000) == WAIT_OBJECT_0);
        /* queued is set under the kernel lock, immediately before ArbitrateLock sleeps. */
        kern_lock();
        kern_unlock();
    }
    mutex_unlock(&mutex);
    for (int i = 0; i < N; i++) {
        join(handles[i]);
        CHECK(args[i].handle == handles[i]);
        CHECK(args[i].tlr && (args[i].tlr & 0x1ff) == 0);
        for (int j = 0; j < i; j++) CHECK(args[i].tlr != args[j].tlr);
        CloseHandle(args[i].queued);
    }
    CHECK(counter == N * 2000 && mutex == 0);
}

typedef struct CvWaiter {
    uint32_t *mutex, *key;
    HANDLE ready, done;
    int64_t timeout;
    uint32_t result;
    int label, *order, *norder;
} CvWaiter;

static void cv_worker(Cpu *c) {
    CvWaiter *a = (CvWaiter *)(uintptr_t)c->x[0];
    mutex_lock(a->mutex, NULL);
    kern_lock();
    SetEvent(a->ready);
    a->result = kern_wait_cv(addr(a->mutex), addr(a->key), kern_current_handle(), a->timeout);
    kern_unlock();
    if (a->result == HOS_OK) {
        CHECK((load_word(a->mutex) & ~WAITERS) == kern_current_handle());
        if (a->order) a->order[(*a->norder)++] = a->label;
        mutex_unlock(a->mutex);
    }
    SetEvent(a->done);
}

static uint32_t start_cv(CvWaiter *a, int priority) {
    a->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint32_t h = start(cv_worker, a, priority);
    CHECK(WaitForSingleObject(a->ready, 2000) == WAIT_OBJECT_0);
    kern_lock();
    kern_unlock(); /* ensures the worker released the mutex and entered the condition-variable wait */
    return h;
}

static void close_cv(CvWaiter *a) { CloseHandle(a->ready); CloseHandle(a->done); }

static void test_cv(void) {
    uint32_t mutex = 0, key = 0;
    int order[3] = {0}, norder = 0;
    CvWaiter waiters[3];
    uint32_t handles[3];
    const int priorities[] = {45, 30, 40};
    for (int i = 0; i < 3; i++) {
        waiters[i] = (CvWaiter){.mutex = &mutex, .key = &key, .timeout = -1,
                              .label = i, .order = order, .norder = &norder};
        handles[i] = start_cv(&waiters[i], priorities[i]);
    }
    CHECK(key == 1 && mutex == 0);
    mutex_lock(&mutex, NULL);
    kern_lock();
    CHECK(kern_signal_cv(addr(&key), 1) == HOS_OK);
    CHECK(key == 1 && (load_word(&mutex) & WAITERS));
    kern_unlock();
    mutex_unlock(&mutex);
    join(handles[1]);
    CHECK(norder == 1 && order[0] == 1);
    mutex_lock(&mutex, NULL);
    kern_lock();
    CHECK(kern_signal_cv(addr(&key), -1) == HOS_OK);
    CHECK(key == 0);
    kern_unlock();
    mutex_unlock(&mutex);
    join(handles[0]);
    join(handles[2]);
    CHECK(norder == 3 && order[1] == 2 && order[2] == 0 && mutex == 0);
    for (int i = 0; i < 3; i++) close_cv(&waiters[i]);

    /* A signal stops the CV timeout even if re-acquiring its mutex takes longer than that timeout. */
    CvWaiter timed = {.mutex = &mutex, .key = &key, .timeout = 200 * NS_PER_MS};
    uint32_t h = start_cv(&timed, 44);
    mutex_lock(&mutex, NULL);
    kern_lock();
    CHECK(kern_signal_cv(addr(&key), 1) == HOS_OK);
    kern_unlock();
    CHECK(WaitForSingleObject(timed.done, 400) == WAIT_TIMEOUT);
    mutex_unlock(&mutex);
    join(h);
    CHECK(timed.result == HOS_OK && mutex == 0 && key == 0);
    close_cv(&timed);

    /* An actual timeout returns without owning the mutex; the guest is responsible for re-locking it. */
    mutex_lock(&mutex, NULL);
    kern_lock();
    CHECK(kern_wait_cv(addr(&mutex), addr(&key), kern_current_handle(), 0) == HOS_TIMED_OUT);
    kern_unlock();
    CHECK(mutex == 0);
}

typedef struct SyncWaiter {
    uint32_t objects[2];
    HANDLE ready;
    uint32_t result;
    int32_t index;
} SyncWaiter;

static void sync_worker(Cpu *c) {
    SyncWaiter *a = (SyncWaiter *)(uintptr_t)c->x[0];
    kern_lock();
    SetEvent(a->ready);
    a->result = kern_wait_sync(a->objects, 2, 5000 * NS_PER_MS, &a->index);
    kern_unlock();
}

static void test_events(void) {
    SyncWaiter a = {.ready = CreateEventW(NULL, TRUE, FALSE, NULL), .index = -1};
    kern_lock();
    a.objects[0] = hos_new_event(0);
    a.objects[1] = hos_new_event(0);
    kern_unlock();
    uint32_t h = start(sync_worker, &a, 44);
    CHECK(WaitForSingleObject(a.ready, 2000) == WAIT_OBJECT_0);
    kern_lock();
    hos_event_set(a.objects[1], 1);
    kern_unlock();
    join(h);
    CHECK(a.result == HOS_OK && a.index == 1);
    kern_lock();
    int32_t index = -1;
    CHECK(kern_wait_sync(a.objects, 2, 0, &index) == HOS_OK && index == 1);
    hos_event_set(a.objects[1], 0);
    CHECK(kern_wait_sync(a.objects, 2, 0, &index) == HOS_TIMED_OUT);
    CHECK(kern_wait_sync(NULL, 0, 0, &index) == HOS_TIMED_OUT);
    uint32_t invalid = 0x1234;
    CHECK(kern_wait_sync(&invalid, 1, 0, &index) == HOS_INVALID_HANDLE);
    uint32_t pseudo = 0xffff8000u;
    CHECK(kern_wait_sync(&pseudo, 1, 0, &index) == HOS_INVALID_HANDLE);
    hos_close_handle(a.objects[0]);
    hos_close_handle(a.objects[1]);
    kern_unlock();
    CloseHandle(a.ready);
}

int main(void) {
    void *tlr = VirtualAlloc(NULL, TLR_REGION_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!tlr) return 2;
    Cpu *cpu;
    uint32_t main_handle = kern_init(&cpu, 0, addr(tlr), 44, 0, 0);
    CHECK(main_handle && cpu->tpidrro == addr(tlr));
    test_contention();
    test_cv();
    test_events();
    printf("%s (%ld failures; 12000 mutex-protected increments)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
