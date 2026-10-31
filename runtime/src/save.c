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
#define FS_PATH_NOT_FOUND FS_RESULT(1)
#define FS_PATH_EXISTS FS_RESULT(2)

typedef struct SaveFile SaveFile;
typedef struct SaveFs {
    wchar_t root[SAVE_PATH_MAX];
    SaveFile *files;
    unsigned refs;
} SaveFs;
struct SaveFile { HANDLE handle; uint32_t mode; SaveFs *fs; SaveFile *next; };

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

static uint32_t host_error(void) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return FS_PATH_NOT_FOUND;
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return FS_PATH_EXISTS;
    hos_log("     save: host error %lu\n", error);
    return FS_INVALID_ARGUMENT;
}

/* Refuse Win32 device names, alternate streams, dot components and reparse points at every level. */
static uint32_t save_path(SaveFs *fs, const IpcRequest *rq, wchar_t *out) {
    const IpcBuf *buffer = rq->nx && rq->x[0].size ? &rq->x[0] : rq->na ? &rq->a[0] : NULL;
    if (!buffer || !buffer->addr || !buffer->size || buffer->size > 0x301) return FS_INVALID_PATH;
    const char *input = (const char *)(uintptr_t)buffer->addr;
    const char *end = memchr(input, 0, (size_t)buffer->size);
    if (!end || input[0] != '/') return FS_INVALID_PATH;
    wchar_t relative[0x302];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int)(end - input), relative, 0x301);
    if (!n) return FS_INVALID_PATH;
    relative[n] = 0;
    size_t used = wcslen(fs->root);
    wcscpy(out, fs->root);
    for (int begin = 1; begin < n;) {
        int finish = begin;
        while (finish < n && relative[finish] != L'/') finish++;
        int length = finish - begin;
        if (!length || (length == 1 && relative[begin] == L'.') ||
            (length == 2 && relative[begin] == L'.' && relative[begin + 1] == L'.') ||
            relative[finish - 1] == L'.' || relative[finish - 1] == L' ') return FS_INVALID_PATH;
        wchar_t device[16] = {0};
        unsigned d = 0;
        for (int i = begin; i < finish; i++) {
            wchar_t ch = relative[i];
            if (ch < 32 || wcschr(L"\\:*?\"<>|", ch)) return FS_INVALID_PATH;
            if (d < 15 && ch != L'.') device[d++] = ch;
            else if (ch == L'.') break;
        }
        /* Check all characters, including extensions, independently of the device-name prefix. */
        for (int i = begin; i < finish; i++)
            if (relative[i] < 32 || wcschr(L"\\:*?\"<>|", relative[i])) return FS_INVALID_PATH;
        if (!_wcsicmp(device, L"CON") || !_wcsicmp(device, L"PRN") || !_wcsicmp(device, L"AUX") ||
            !_wcsicmp(device, L"NUL") || !_wcsicmp(device, L"CONIN$") || !_wcsicmp(device, L"CONOUT$") ||
            (d == 4 && device[3] >= L'1' && device[3] <= L'9' &&
             (!_wcsnicmp(device, L"COM", 3) || !_wcsnicmp(device, L"LPT", 3)))) return FS_INVALID_PATH;
        if (used + length + 2 >= SAVE_PATH_MAX) return FS_INVALID_PATH;
        out[used++] = L'\\';
        memcpy(out + used, relative + begin, (size_t)length * sizeof *out);
        out[used += length] = 0;
        DWORD attributes = GetFileAttributesW(out);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FS_INVALID_PATH;
        if (finish < n && (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)))
            return FS_PATH_NOT_FOUND;
        begin = finish + 1;
    }
    DWORD root = GetFileAttributesW(fs->root);
    if (root == INVALID_FILE_ATTRIBUTES || !(root & FILE_ATTRIBUTE_DIRECTORY) || (root & FILE_ATTRIBUTE_REPARSE_POINT))
        return FS_INVALID_PATH;
    return HOS_OK;
}

static void fs_release(SaveFs *fs) { if (!--fs->refs) free(fs); }
static void save_fs_destroy(Service *s) { fs_release(s->state); free(s); }

static void save_file_destroy(Service *s) {
    SaveFile *file = s->state;
    SaveFile **p = &file->fs->files;
    while (*p && *p != file) p = &(*p)->next;
    if (*p) *p = file->next;
    if (file->mode & 2) FlushFileBuffers(file->handle);
    CloseHandle(file->handle);
    fs_release(file->fs);
    free(file); free(s);
}

static void save_file_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    SaveFile *file = s->state;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file->handle, &size)) { rp->result = host_error(); return; }
    switch (rq->cmd) {
    case 0: case 1: { /* Read/Write: option:u32, padding, offset:s64, size:s64; B output or A input. */
        int writing = rq->cmd == 1;
        const IpcBuf *buffer = writing ? (rq->na ? &rq->a[0] : NULL) : (rq->nb ? &rq->b[0] : NULL);
        if (rq->args_size < 24) { rp->result = FS_INVALID_ARGUMENT; return; }
        uint32_t option = RQ_GET(rq, uint32_t, 0);
        int64_t offset = RQ_GET(rq, int64_t, 8), count = RQ_GET(rq, int64_t, 16);
        if (offset < 0 || count < 0 || count > INT64_MAX - offset || option > (writing ? 1u : 0u) ||
            !(file->mode & (writing ? 2u : 1u)) || !buffer || (uint64_t)count > buffer->size ||
            (count && !buffer->addr)) { rp->result = FS_INVALID_ARGUMENT; return; }
        if (writing && !(file->mode & 4) && offset + count > size.QuadPart) { rp->result = 0x307202; return; }
        LARGE_INTEGER position; position.QuadPart = offset;
        if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)) { rp->result = host_error(); return; }
        uint64_t done = 0;
        while (done < (uint64_t)count) {
            DWORD step = (DWORD)(((uint64_t)count - done) > 0x100000 ? 0x100000 : (uint64_t)count - done), transferred;
            void *data = (void *)(uintptr_t)(buffer->addr + done);
            BOOL ok = writing ? WriteFile(file->handle, data, step, &transferred, NULL) :
                                ReadFile(file->handle, data, step, &transferred, NULL);
            if (!ok) { rp->result = host_error(); return; }
            done += transferred;
            if (transferred != step) {
                if (writing) rp->result = FS_INVALID_ARGUMENT;
                break;
            }
        }
        if (writing && option && !FlushFileBuffers(file->handle)) rp->result = host_error();
        if (!writing) RP_PUT(rp, uint64_t, done);
        return;
    }
    case 2: /* Flush */
        if ((file->mode & 2) && !FlushFileBuffers(file->handle)) rp->result = host_error();
        return;
    case 3: { /* SetSize */
        if (rq->args_size < 8 || !(file->mode & 2) || RQ_GET(rq, int64_t, 0) < 0) {
            rp->result = FS_INVALID_ARGUMENT; return;
        }
        size.QuadPart = RQ_GET(rq, int64_t, 0);
        if (!SetFilePointerEx(file->handle, size, NULL, FILE_BEGIN) || !SetEndOfFile(file->handle)) rp->result = host_error();
        return;
    }
    case 4: RP_PUT(rp, int64_t, size.QuadPart); return;
    default: hos_unimplemented(c, s, rq);
    }
}

static void save_fs_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    SaveFs *fs = s->state;
    if (rq->cmd == 10) { /* Commit: flush each open writable file before replying. */
        for (SaveFile *file = fs->files; file; file = file->next)
            if ((file->mode & 2) && !FlushFileBuffers(file->handle)) { rp->result = host_error(); return; }
        return;
    }
    if (rq->cmd != 0 && rq->cmd != 1 && rq->cmd != 2 && rq->cmd != 3 && rq->cmd != 7 && rq->cmd != 8 &&
        rq->cmd != 11 && rq->cmd != 12) hos_unimplemented(c, s, rq);
    wchar_t path[SAVE_PATH_MAX];
    rp->result = save_path(fs, rq, path);
    if (rp->result) return;
    if (rq->cmd <= 3 && !wcscmp(path, fs->root)) { rp->result = FS_INVALID_PATH; return; }
    switch (rq->cmd) {
    case 0: { /* CreateFile */
        if (rq->args_size < 16 || RQ_GET(rq, uint32_t, 0) > 1 || RQ_GET(rq, int64_t, 8) < 0) {
            rp->result = FS_INVALID_ARGUMENT; return;
        }
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (file == INVALID_HANDLE_VALUE) { rp->result = host_error(); return; }
        LARGE_INTEGER size; size.QuadPart = RQ_GET(rq, int64_t, 8);
        if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file) || !FlushFileBuffers(file))
            rp->result = host_error();
        CloseHandle(file);
        if (rp->result) DeleteFileW(path); /* only the new file from this command */
        return;
    }
    case 1: if (!DeleteFileW(path)) rp->result = host_error(); return;
    case 2: if (!CreateDirectoryW(path, NULL)) rp->result = host_error(); return;
    case 3: if (!RemoveDirectoryW(path)) rp->result = host_error(); return;
    case 7: { /* GetEntryType */
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) { rp->result = host_error(); return; }
        RP_PUT(rp, uint32_t, (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1);
        return;
    }
    case 8: { /* OpenFile, preserving existing contents. */
        if (rq->args_size < 4) { rp->result = FS_INVALID_ARGUMENT; return; }
        uint32_t mode = RQ_GET(rq, uint32_t, 0);
        if (!(mode & 3) || (mode & ~7u) || ((mode & 4) && !(mode & 2))) { rp->result = FS_INVALID_ARGUMENT; return; }
        HANDLE handle = CreateFileW(path, ((mode & 1) ? GENERIC_READ : 0) | ((mode & 2) ? GENERIC_WRITE : 0),
                                    FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE) { rp->result = host_error(); return; }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(handle); rp->result = FS_INVALID_PATH; return;
        }
        SaveFile *file = calloc(1, sizeof *file);
        Service *service = calloc(1, sizeof *service);
        if (!file || !service) { CloseHandle(handle); free(file); free(service); hos_stop(c, "out of memory opening save file"); }
        file->handle = handle; file->mode = mode; file->fs = fs; file->next = fs->files;
        fs->files = file; fs->refs++;
        snprintf(service->name, sizeof service->name, "fsp:IFile(save)");
        service->state = file; service->destroy = save_file_destroy; service->handle = save_file_handle;
        hos_reply_interface(rp, service);
        return;
    }
    case 11: case 12: {
        ULARGE_INTEGER available, total, free_bytes;
        if (!GetDiskFreeSpaceExW(fs->root, &available, &total, &free_bytes)) { rp->result = host_error(); return; }
        RP_PUT(rp, uint64_t, rq->cmd == 11 ? available.QuadPart : total.QuadPart);
        return;
    }
    }
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
    fs->refs = 1;
    service->state = fs;
    service->destroy = save_fs_destroy;
    hos_log("     opened host account save for program %#llx\n", (unsigned long long)g_program_id);
    hos_reply_interface(rp, service);
}
