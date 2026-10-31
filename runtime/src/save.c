/* Host-backed account save filesystems. All paths stay below the configured save root.
 * Interface command ids follow switchbrew's Filesystem services page for firmware 4.x. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define SAVE_PATH_MAX 4096
#define FS_RESULT(desc) (2u | ((desc) << 9))
#define FS_INVALID_ARGUMENT FS_RESULT(6001)
#define FS_INVALID_PATH FS_RESULT(6004)

typedef struct SaveFs {
    wchar_t root[SAVE_PATH_MAX];
} SaveFs;

static char g_save_root[SAVE_PATH_MAX];
static uint64_t g_program_id;

void hos_save_configure(const char *root, uint64_t program_id) {
    snprintf(g_save_root, sizeof g_save_root, "%s", root);
    g_program_id = program_id;
}

/* Create the absolute root a component at a time and refuse reparse points, including existing ancestors.
 * This prototype supports local drive paths; UNC paths need their own root validation before being enabled. */
static int make_save_root(wchar_t *path) {
    if (path[1] != L':' || path[2] != L'\\') return 0;
    size_t length = wcslen(path);
    for (size_t i = 3; i <= length; i++) {
        if (path[i] != L'\\' && path[i]) continue;
        wchar_t saved = path[i];
        path[i] = 0;
        DWORD attrs = GetFileAttributesW(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { path[i] = saved; return 0; }
            attrs = GetFileAttributesW(path);
        }
        path[i] = saved;
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return 0;
    }
    return 1;
}

static void save_fs_destroy(Service *s) { free(s->state); free(s); }

static void save_fs_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    (void)rp;
    hos_unimplemented(c, s, rq);
}

void hos_save_open(Cpu *c, const IpcRequest *rq, IpcReply *rp) {
    if (rq->args_size < 72) { rp->result = FS_INVALID_ARGUMENT; return; }
    uint64_t program_id = RQ_GET(rq, uint64_t, 8);
    uint64_t uid0 = RQ_GET(rq, uint64_t, 16), uid1 = RQ_GET(rq, uint64_t, 24);
    /* User space, account save, current program. Other save types remain explicit work items. */
    if (rq->args[0] != 1 || rq->args[40] != 1 || (program_id && program_id != g_program_id)) {
        hos_stop(c, "unsupported save space/type/program: %u/%u/%#llx", rq->args[0], rq->args[40],
                 (unsigned long long)program_id);
    }
    wchar_t configured[SAVE_PATH_MAX], absolute[SAVE_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, g_save_root, -1, configured, SAVE_PATH_MAX)) {
        rp->result = FS_INVALID_PATH;
        return;
    }
    DWORD n = GetFullPathNameW(configured, SAVE_PATH_MAX, absolute, NULL);
    if (!n || n >= SAVE_PATH_MAX) { rp->result = FS_INVALID_PATH; return; }
    SaveFs *fs = calloc(1, sizeof *fs);
    Service *service = calloc(1, sizeof *service);
    if (!fs || !service) { free(fs); free(service); hos_stop(c, "out of host memory opening save"); }
    int written = swprintf(fs->root, SAVE_PATH_MAX, L"%ls\\%016llx\\%016llx%016llx", absolute,
                           (unsigned long long)g_program_id, (unsigned long long)uid0, (unsigned long long)uid1);
    if (written < 0 || written >= SAVE_PATH_MAX || !make_save_root(fs->root)) {
        free(fs); free(service);
        hos_stop(c, "cannot create save root (host error %lu)", GetLastError());
    }
    snprintf(service->name, sizeof service->name, "fsp:IFileSystem(save)");
    service->handle = save_fs_handle;
    service->state = fs;
    service->destroy = save_fs_destroy;
    hos_log("     opened host account save for program %#llx\n", (unsigned long long)g_program_id);
    hos_reply_interface(rp, service);
}
