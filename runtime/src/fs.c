/* Read-only RomFS storage for fsp-srv. The input is a raw, already-decrypted RomFS image, never an NCA.
 * The guest's own filesystem code parses the metadata; IStorage exposes byte reads with 64-bit offsets.
 * Commands follow switchbrew's Filesystem services page (firmware 4.x). All calls hold the kernel lock. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_RESULT(desc) (2u | ((desc) << 9))
#define FS_OUT_OF_RANGE FS_RESULT(3005)
#define FS_INVALID_ARGUMENT FS_RESULT(6001)

static char g_romfs_path[4096];
static FILE *g_romfs;
static uint64_t g_romfs_size;

void hos_fs_set_romfs(const char *path) {
    if (g_romfs) fclose(g_romfs);
    g_romfs = NULL;
    g_romfs_size = 0;
    snprintf(g_romfs_path, sizeof g_romfs_path, "%s", path);
}

static void open_romfs(Cpu *c) {
    if (g_romfs) return;
    g_romfs = fopen(g_romfs_path, "rb");
    if (!g_romfs) hos_stop(c, "cannot open RomFS image '%s'; build it with python -m dkrecomp.romfs", g_romfs_path);
    if (_fseeki64(g_romfs, 0, SEEK_END)) hos_stop(c, "RomFS: cannot seek the image");
    int64_t size = _ftelli64(g_romfs);
    if (size < 0x50) hos_stop(c, "RomFS: image is shorter than its header");
    g_romfs_size = (uint64_t)size;
    uint64_t header_size = 0;
    if (_fseeki64(g_romfs, 0, SEEK_SET) || fread(&header_size, 1, 8, g_romfs) != 8 || header_size != 0x50)
        hos_stop(c, "RomFS: expected a raw filesystem with a 0x50-byte header");
    hos_log("     RomFS opened: %llu bytes\n", (unsigned long long)g_romfs_size);
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
        if (_fseeki64(g_romfs, offset, SEEK_SET) ||
            fread((void *)(uintptr_t)rq->b[0].addr, 1, (size_t)length, g_romfs) != (size_t)length)
            hos_stop(c, "RomFS: host read failed at %#llx, size %#llx", (unsigned long long)offset,
                     (unsigned long long)length);
        hos_log("     RomFS read: offset %#llx, size %#llx\n", (unsigned long long)offset, (unsigned long long)length);
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
    case 1003: return;                           /* DisableAutoSaveDataCreation */
    case 1005: RP_PUT(rp, uint32_t, 0); return;  /* GetGlobalAccessLogMode: logging off */
    case 1006: return;                           /* OutputAccessLogToSdCard */
    default: hos_unimplemented(c, s, rq);
    }
}
