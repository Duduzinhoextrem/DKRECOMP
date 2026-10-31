/* Horizon shared memory backed by real Windows section views. All calls hold the kernel lock.
 * MapViewOfFile3 + split placeholders allow a guest view at a 4 KiB-aligned address, independently of the
 * Windows 64 KiB reservation granularity. No copying or polling is needed to propagate service writes.
 * Reference: Microsoft memoryapi documentation; no external implementation is incorporated here. */
#include "hos.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

typedef PVOID (WINAPI *Alloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *Map3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *UnmapExFn)(PVOID, ULONG);
static Alloc2Fn alloc2;
static Map3Fn map3;
static UnmapExFn unmap_ex;

#define SHARED_BASE 0xA000u
#define MAX_SHARED_HANDLES 256
#define MAX_RESERVATIONS 128
#define MAX_SPANS 768

typedef struct SharedMemory {
    HANDLE section;
    void *host;
    uint64_t size;
    uint32_t remote_perm;
    unsigned refs; /* open handles and guest mappings each hold one reference */
} SharedMemory;

typedef struct Reservation {
    uint64_t base, size;
    unsigned views;
} Reservation;

typedef struct Span {
    uint64_t base, size; /* zero size denotes an unused slot */
    int reservation;
    SharedMemory *object; /* NULL denotes an unmapped placeholder */
} Span;

static SharedMemory *g_handles[MAX_SHARED_HANDLES];
static Reservation g_reservations[MAX_RESERVATIONS];
static Span g_spans[MAX_SPANS];

static SharedMemory *object_of(uint32_t handle) {
    uint32_t i = handle - SHARED_BASE;
    return i < MAX_SHARED_HANDLES ? g_handles[i] : NULL;
}

static void release_object(SharedMemory *object) {
    if (--object->refs) return;
    UnmapViewOfFile(object->host);
    CloseHandle(object->section);
    free(object);
}

static uint32_t new_handle(SharedMemory *object) {
    for (uint32_t i = 0; i < MAX_SHARED_HANDLES; i++) {
        if (g_handles[i]) continue;
        g_handles[i] = object;
        object->refs++;
        return SHARED_BASE + i;
    }
    return 0;
}

uint32_t hos_new_shared_memory(uint64_t size, uint32_t remote_perm) {
    if (!size || (size & 0xfff) || size > SIZE_MAX || (remote_perm != 1 && remote_perm != 3)) return 0;
    if (!alloc2 || !map3 || !unmap_ex) {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        alloc2 = (Alloc2Fn)(void *)GetProcAddress(module, "VirtualAlloc2");
        map3 = (Map3Fn)(void *)GetProcAddress(module, "MapViewOfFile3");
        unmap_ex = (UnmapExFn)(void *)GetProcAddress(module, "UnmapViewOfFileEx");
        if (!alloc2 || !map3 || !unmap_ex) return 0;
    }
    SharedMemory *object = calloc(1, sizeof *object);
    if (!object) return 0;
    object->section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!object->section) { free(object); return 0; }
    object->host = MapViewOfFile(object->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)size);
    if (!object->host) { CloseHandle(object->section); free(object); return 0; }
    object->size = size;
    object->remote_perm = remote_perm;
    object->refs = 1; /* temporary creation reference */
    uint32_t handle = new_handle(object);
    release_object(object);
    return handle;
}

uint32_t hos_copy_shared_memory_handle(uint32_t handle) {
    SharedMemory *object = object_of(handle);
    return object ? new_handle(object) : 0;
}

void *hos_shared_memory_data(uint32_t handle, uint64_t *size) {
    SharedMemory *object = object_of(handle);
    if (!object) return NULL;
    if (size) *size = object->size;
    return object->host;
}

int hos_close_shared_memory(uint32_t handle) {
    SharedMemory *object = object_of(handle);
    if (!object) return 0;
    g_handles[handle - SHARED_BASE] = NULL;
    release_object(object);
    return 1;
}

static int free_span(void) {
    for (int i = 0; i < MAX_SPANS; i++) if (!g_spans[i].size) return i;
    return -1;
}

/* Coalesce adjacent placeholders in one reservation. They remain available for a later page-aligned view. */
static void tidy_reservation(int reservation) {
    int changed;
    do {
        changed = 0;
        for (int i = 0; i < MAX_SPANS && !changed; i++) {
            Span *a = &g_spans[i];
            if (!a->size || a->object || a->reservation != reservation) continue;
            for (int j = 0; j < MAX_SPANS; j++) {
                Span *b = &g_spans[j];
                if (!b->size || b->object || b->reservation != reservation || a->base + a->size != b->base) continue;
                if (!VirtualFree((void *)(uintptr_t)a->base, (SIZE_T)(a->size + b->size),
                                 MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) continue;
                a->size += b->size;
                b->size = 0;
                changed = 1;
                break;
            }
        }
    } while (changed);
    if (g_reservations[reservation].views) return;
    for (int i = 0; i < MAX_SPANS; i++) {
        Span *s = &g_spans[i];
        if (!s->size || s->reservation != reservation) continue;
        if (!VirtualFree((void *)(uintptr_t)s->base, 0, MEM_RELEASE)) return;
        s->size = 0;
    }
    g_reservations[reservation].size = 0;
}

static int placeholder_for(uint64_t addr, uint64_t size) {
    for (int i = 0; i < MAX_SPANS; i++) {
        Span *s = &g_spans[i];
        if (s->size && !s->object && addr >= s->base && addr + size <= s->base + s->size) return i;
    }
    uint64_t base = addr & ~0xffffull, end = (addr + size + 0xffff) & ~0xffffull;
    int ri = -1, si = free_span();
    for (int i = 0; i < MAX_RESERVATIONS; i++) {
        Reservation *r = &g_reservations[i];
        if (r->size && base < r->base + r->size && r->base < end) return -1;
        if (!r->size && ri < 0) ri = i;
    }
    if (ri < 0 || si < 0) return -1;
    void *p = alloc2(GetCurrentProcess(), (void *)(uintptr_t)base, (SIZE_T)(end - base),
                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (p != (void *)(uintptr_t)base) return -1;
    g_reservations[ri] = (Reservation){base, end - base, 0};
    g_spans[si] = (Span){base, end - base, ri, NULL};
    return si;
}

static int split_placeholder(int index, uint64_t cut) {
    Span *s = &g_spans[index];
    if (cut <= s->base || cut >= s->base + s->size) return 1;
    int other = free_span();
    if (other < 0) return 0;
    if (!VirtualFree((void *)(uintptr_t)s->base, (SIZE_T)(cut - s->base),
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return 0;
    g_spans[other] = (Span){cut, s->base + s->size - cut, s->reservation, NULL};
    s->size = cut - s->base;
    return 1;
}

uint32_t hos_map_shared_memory(uint32_t handle, uint64_t addr, uint64_t size, uint32_t perm) {
    SharedMemory *object = object_of(handle);
    if (addr & 0xfff) return HOS_KERNEL(102);
    if (!size || (size & 0xfff) || addr + size < addr) return HOS_KERNEL(101);
    if (perm != 1 && perm != 3) return HOS_KERNEL(108);
    if (!object) return HOS_INVALID_HANDLE;
    if (size != object->size) return HOS_KERNEL(101);
    if (perm != object->remote_perm) return HOS_KERNEL(108);
    if (!addr || addr + size > (1ull << 39)) return HOS_KERNEL(106);
    int index = placeholder_for(addr, size);
    if (index < 0) return HOS_KERNEL(106);
    int reservation = g_spans[index].reservation;
    if (!split_placeholder(index, addr)) { tidy_reservation(reservation); return HOS_KERNEL(104); }
    index = placeholder_for(addr, size);
    if (index < 0 || !split_placeholder(index, addr + size)) {
        tidy_reservation(reservation);
        return HOS_KERNEL(104);
    }
    void *view = map3(object->section, GetCurrentProcess(), (void *)(uintptr_t)addr, 0, (SIZE_T)size,
                      MEM_REPLACE_PLACEHOLDER, perm == 1 ? PAGE_READONLY : PAGE_READWRITE, NULL, 0);
    if (view != (void *)(uintptr_t)addr) {
        hos_log("shared: MapViewOfFile3 failed at %#llx: Windows error %lu\n", (unsigned long long)addr, GetLastError());
        tidy_reservation(reservation);
        return HOS_KERNEL(104);
    }
    g_spans[index].object = object;
    object->refs++;
    g_reservations[reservation].views++;
    return HOS_OK;
}

uint32_t hos_unmap_shared_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    SharedMemory *object = object_of(handle);
    if (addr & 0xfff) return HOS_KERNEL(102);
    if (!size || (size & 0xfff) || addr + size < addr) return HOS_KERNEL(101);
    if (!object) return HOS_INVALID_HANDLE;
    for (int i = 0; i < MAX_SPANS; i++) {
        Span *s = &g_spans[i];
        if (!s->size || s->object != object || s->base != addr || s->size != size) continue;
        if (!unmap_ex((void *)(uintptr_t)addr, MEM_PRESERVE_PLACEHOLDER)) return HOS_KERNEL(106);
        s->object = NULL;
        release_object(object);
        g_reservations[s->reservation].views--;
        tidy_reservation(s->reservation);
        return HOS_OK;
    }
    return HOS_KERNEL(106);
}
