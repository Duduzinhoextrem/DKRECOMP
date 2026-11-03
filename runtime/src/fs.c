/* Read-only RomFS storage for fsp-srv. The input is a raw, already-decrypted RomFS image, never an NCA.
 * The guest's own filesystem code parses the metadata; IStorage exposes byte reads with 64-bit offsets.
 * Commands follow switchbrew's Filesystem services page (firmware 4.x). All calls hold the kernel lock. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define FS_RESULT(desc) (2u | ((desc) << 9))
#define FS_OUT_OF_RANGE FS_RESULT(3005)
#define FS_INVALID_ARGUMENT FS_RESULT(6001)

static char g_romfs_path[4096];
static FILE *g_romfs;
static HANDLE g_romfs_file = INVALID_HANDLE_VALUE;
static HANDLE g_romfs_mapping;
static const uint8_t *g_romfs_map;
static uint64_t g_romfs_size;
static SRWLOCK g_romfs_lock = SRWLOCK_INIT;

void hos_fs_set_romfs(const char *path) {
    AcquireSRWLockExclusive(&g_romfs_lock);
    if (g_romfs_map) { UnmapViewOfFile(g_romfs_map); g_romfs_map = NULL; }
    if (g_romfs_mapping) { CloseHandle(g_romfs_mapping); g_romfs_mapping = NULL; }
    if (g_romfs_file != INVALID_HANDLE_VALUE) { CloseHandle(g_romfs_file); g_romfs_file = INVALID_HANDLE_VALUE; }
    if (g_romfs) { fclose(g_romfs); g_romfs = NULL; }
    g_romfs_size = 0;
    snprintf(g_romfs_path, sizeof g_romfs_path, "%s", path ? path : "");
    ReleaseSRWLockExclusive(&g_romfs_lock);
}

static void open_romfs(Cpu *c) {
    AcquireSRWLockExclusive(&g_romfs_lock);
    if (g_romfs || g_romfs_map) { ReleaseSRWLockExclusive(&g_romfs_lock); return; }
    g_romfs = fopen(g_romfs_path, "rb");
    if (!g_romfs) {
        ReleaseSRWLockExclusive(&g_romfs_lock);
        hos_stop(c, "cannot open RomFS image '%s'; build it with python -m dkrecomp.romfs", g_romfs_path);
    }
    if (_fseeki64(g_romfs, 0, SEEK_END)) {
        ReleaseSRWLockExclusive(&g_romfs_lock);
        hos_stop(c, "RomFS: cannot seek the image");
    }
    int64_t size = _ftelli64(g_romfs);
    if (size < 0x50) {
        ReleaseSRWLockExclusive(&g_romfs_lock);
        hos_stop(c, "RomFS: image is shorter than its header");
    }
    g_romfs_size = (uint64_t)size;
    uint64_t header_size = 0;
    if (_fseeki64(g_romfs, 0, SEEK_SET) || fread(&header_size, 1, 8, g_romfs) != 8 || header_size != 0x50) {
        ReleaseSRWLockExclusive(&g_romfs_lock);
        hos_stop(c, "RomFS: expected a raw filesystem with a 0x50-byte header");
    }
    g_romfs_file = CreateFileA(g_romfs_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
    if (g_romfs_file != INVALID_HANDLE_VALUE) {
        g_romfs_mapping = CreateFileMappingA(g_romfs_file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (g_romfs_mapping)
            g_romfs_map = (const uint8_t *)MapViewOfFile(g_romfs_mapping, FILE_MAP_READ, 0, 0, 0);
    }
    ReleaseSRWLockExclusive(&g_romfs_lock);
    hos_log("     RomFS opened: %llu bytes (%s)\n", (unsigned long long)g_romfs_size, g_romfs_map ? "mapped" : "stdio");
}

static void storage_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: { /* Read(s64 offset, s64 size, output map-alias buffer B) */
        if (rq->args_size < 16) { rp->result = FS_INVALID_ARGUMENT; return; }
        int64_t offset = RQ_GET(rq, int64_t, 0), length = RQ_GET(rq, int64_t, 8);
        if (offset < 0 || length < 0 || (uint64_t)offset > g_romfs_size ||
            (uint64_t)length > g_romfs_size - (uint64_t)offset) {
            rp->result = FS_OUT_OF_RANGE;
            return;
        }
        if (!length) return;
        if (!rq->nb || !rq->b[0].addr || (uint64_t)length > rq->b[0].size) {
            rp->result = FS_INVALID_ARGUMENT;
            return;
        }
        void *dst = (void *)(uintptr_t)rq->b[0].addr;
        kern_unlock();
        int read_err = 0;
        AcquireSRWLockShared(&g_romfs_lock);
        if (g_romfs_map) {
            memcpy(dst, g_romfs_map + (size_t)offset, (size_t)length);
            ReleaseSRWLockShared(&g_romfs_lock);
        } else {
            ReleaseSRWLockShared(&g_romfs_lock);
            AcquireSRWLockExclusive(&g_romfs_lock);
            if (!g_romfs || _fseeki64(g_romfs, offset, SEEK_SET) ||
                fread(dst, 1, (size_t)length, g_romfs) != (size_t)length)
                read_err = 1;
            ReleaseSRWLockExclusive(&g_romfs_lock);
        }
        kern_lock();
        if (read_err)
            hos_stop(c, "RomFS: host read failed at %#llx, size %#llx", (unsigned long long)offset,
                     (unsigned long long)length);
        static int log_fs = -1;
        if (log_fs < 0) log_fs = getenv("DKR_LOG_FS") != NULL;
        if (log_fs) hos_log("     RomFS read: offset %#llx, size %#llx\n", (unsigned long long)offset, (unsigned long long)length);
        return;
    }
    case 2: return; /* Flush: immutable storage */
    case 4: RP_PUT(rp, uint64_t, g_romfs_size); return; /* GetSize */
    default: hos_unimplemented(c, s, rq);
    }
}

void hos_fsp_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service storage = {"fsp:IStorage", storage_handle, NULL};
    switch (rq->cmd) {
    case 1: return;                              /* SetCurrentProcess(u64 pid placeholder, PID) */
    case 200:                                   /* OpenDataStorageByCurrentProcess -> IStorage */
        open_romfs(c);
        hos_reply_interface(rp, &storage);
        return;
    case 51: hos_save_open(c, rq, rp); return;     /* OpenSaveDataFileSystem */
    case 1003: return;                           /* DisableAutoSaveDataCreation */
    case 1005: RP_PUT(rp, uint32_t, 0); return;  /* GetGlobalAccessLogMode: logging off */
    case 1006: return;                           /* OutputAccessLogToSdCard */
    default: hos_unimplemented(c, s, rq);
    }
}
