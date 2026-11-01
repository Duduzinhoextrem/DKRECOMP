/* NVN compiler sections are self-sized (+0x20); the base SDK 4.4 uses 0x840, not SDK 12's 0x880.
 * Container metadata follows the measured format documented by nxnative (MIT), see shaderjobs.py/notices.
 * This file copies the actual API inputs and hashes them with Windows CNG; it does not translate shaders. */
#include "nvn_shaders.h"
#include "../src/hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <wchar.h>

static uint32_t word(const uint8_t *p, unsigned offset) { uint32_t v; memcpy(&v, p + offset, 4); return v; }
static void read_guest(Cpu *c, void *out, uint64_t address, size_t bytes) {
    SIZE_T read = 0;
    if (!address || !ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, out, bytes, &read) || read != bytes)
        hos_stop(c, "nvn: unreadable shader section at %#llx (%zu bytes)", (unsigned long long)address, bytes);
}

static INIT_ONCE g_hash_once = INIT_ONCE_STATIC_INIT;
static BCRYPT_ALG_HANDLE g_sha256;
static DWORD g_hash_bytes;
static BOOL CALLBACK hash_initialize(PINIT_ONCE once, void *parameter, void **context) {
    (void)once; (void)parameter; (void)context;
    DWORD got = 0;
    if (BCryptOpenAlgorithmProvider(&g_sha256, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return FALSE;
    if (BCryptGetProperty(g_sha256, BCRYPT_OBJECT_LENGTH, (PUCHAR)&g_hash_bytes, sizeof g_hash_bytes, &got, 0) < 0) {
        BCryptCloseAlgorithmProvider(g_sha256, 0); g_sha256 = NULL; return FALSE;
    }
    return TRUE;
}

void nvn_shader_digest(Cpu *c, const void *a, uint32_t na, const void *b, uint32_t nb, char key[33]) {
    if (!InitOnceExecuteOnce(&g_hash_once, hash_initialize, NULL, NULL)) hos_stop(c, "nvn: cannot initialize SHA-256");
    uint8_t *object = malloc(g_hash_bytes), sum[32];
    BCRYPT_HASH_HANDLE hash = NULL;
    if (!object) hos_stop(c, "nvn: out of memory hashing a shader");
    NTSTATUS status = BCryptCreateHash(g_sha256, &hash, object, g_hash_bytes, NULL, 0, 0);
    if (status >= 0 && na) status = BCryptHashData(hash, (PUCHAR)a, na, 0);
    if (status >= 0 && nb) status = BCryptHashData(hash, (PUCHAR)b, nb, 0);
    if (status >= 0) status = BCryptFinishHash(hash, sum, sizeof sum, 0);
    if (hash) BCryptDestroyHash(hash);
    free(object);
    if (status < 0) hos_stop(c, "nvn: SHA-256 failed (%#lx)", (unsigned long)status);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++) { key[2*i] = hex[sum[i] >> 4]; key[2*i+1] = hex[sum[i] & 15]; }
    key[32] = 0;
}

void nvn_shader_release(NvnShader *s) {
    free(s->control); free(s->code); memset(s, 0, sizeof *s);
}

void nvn_shader_snapshot(Cpu *c, NvnShader *out, uint64_t control, uint64_t code) {
    uint8_t prefix[0x24];
    read_guest(c, prefix, control, sizeof prefix);
    uint32_t size = word(prefix, 0x20);
    if (word(prefix, 0) != 0x98761234 || size < 0x760 || size > 0x100000)
        hos_stop(c, "nvn: invalid shader control magic/length (%#x)", size);
    NvnShader s = {0};
    s.control = malloc(size);
    if (!s.control) hos_stop(c, "nvn: out of memory copying shader control");
    read_guest(c, s.control, control, size);
    /* Later compilers may append transform-feedback records after their own declared section. */
    if (size >= 0x7f0 && word(s.control, 0x7ec)) {
        uint32_t offset = word(s.control, 0x7e8), extra = word(s.control, 0x7ec);
        if (offset < size || offset > 0x100000 || extra > 0x100000 - offset)
            hos_stop(c, "nvn: shader extra control data is outside the capture limit");
        size = offset + extra;
        uint8_t *extended = realloc(s.control, size);
        if (!extended) hos_stop(c, "nvn: out of memory copying extra shader control");
        s.control = extended;
        read_guest(c, s.control, control, size);
    }
    s.control_size = size;
    s.code_size = word(s.control, 0x704); s.c1_offset = word(s.control, 0x700);
    if (s.code_size < 16 || s.code_size > 0x1000000 || s.c1_offset > s.code_size || (s.code_size & 7))
        hos_stop(c, "nvn: invalid shader code/constant lengths (%#x/%#x)", s.code_size, s.c1_offset);
    s.code = malloc(s.code_size);
    if (!s.code) hos_stop(c, "nvn: out of memory copying shader code");
    read_guest(c, s.code, code, s.code_size);
    uint32_t magic = word(s.code, 0);
    if (magic == 0x12345678) {
        if (s.code_size < 0x90) hos_stop(c, "nvn: truncated graphics shader header");
        unsigned kind = (word(s.code, 0x30) >> 10) & 15;
        static const uint8_t stages[] = {255, 0, 3, 4, 2, 1}; /* SPH -> NVNstage */
        if (kind >= sizeof stages || stages[kind] == 255)
            hos_stop(c, "nvn: graphics shader type %u needs explicit translation support", kind);
        s.stage = stages[kind];
    } else {
        uint32_t x = word(s.control, 0x748), y = word(s.control, 0x74c), z = word(s.control, 0x750);
        if (!x || x > 1024 || !y || y > 1024 || !z || z > 1024 || (uint64_t)x*y*z > 1024)
            hos_stop(c, "nvn: invalid compute shader group size");
        if (magic == 0x12345679) {
            if (s.code_size < 0x110) hos_stop(c, "nvn: truncated compute code header");
            for (unsigned i = 0x30; i < 0x100; i++)
                if (s.code[i]) hos_stop(c, "nvn: unsupported compute code padding");
        }
        s.stage = 5;
    }
    nvn_shader_digest(c, s.control, s.control_size, s.code, s.code_size, s.key);
    *out = s;
}

static int write_bytes(HANDLE file, const void *data, DWORD size) {
    DWORD written;
    return WriteFile(file, data, size, &written, NULL) && written == size;
}

void nvn_program_capture(Cpu *c, const NvnProgramSnapshot *p) {
    const char *directory = getenv("DKR_SHADER_CAPTURE");
    if (!directory || !*directory || !p->count) return;
    wchar_t folder[4096], path[4096], temporary[4096];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, folder, 4096) ||
        !(folder[0] && folder[1] == L':' && (folder[2] == L'\\' || folder[2] == L'/')))
        hos_stop(c, "nvn: DKR_SHADER_CAPTURE requires an absolute local directory");
    if (!CreateDirectoryW(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        hos_stop(c, "nvn: cannot create shader capture directory (%lu)", GetLastError());
    char keys[6 * 32], key[33];
    for (unsigned i = 0; i < p->count; i++) memcpy(keys + 32*i, p->shaders[i].key, 32);
    nvn_shader_digest(c, keys, p->count * 32, NULL, 0, key);
    if (swprintf(path, 4096, L"%ls/p_%hs.nvnprog", folder, key) < 0)
        hos_stop(c, "nvn: shader capture path is too long");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    if (swprintf(temporary, 4096, L"%ls/p_%hs.%lu.%lu.%llu.tmp", folder, key, GetCurrentProcessId(),
        GetCurrentThreadId(), (unsigned long long)GetTickCount64()) < 0)
        hos_stop(c, "nvn: shader capture temporary path is too long");
    /* One immutable file per program, without an append-only manifest or transient guest pointers. */
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) hos_stop(c, "nvn: cannot create shader capture (%lu)", GetLastError());
    uint32_t header[4] = {0x4e524b44, 0x31504e56, p->count, 0}; /* DKRNVNP1 */
    int ok = write_bytes(file, header, sizeof header);
    for (unsigned i = 0; ok && i < p->count; i++) {
        const NvnShader *s = &p->shaders[i];
        uint32_t lengths[] = {s->control_size, s->code_size};
        ok = write_bytes(file, lengths, sizeof lengths) && write_bytes(file, s->control, s->control_size) &&
             write_bytes(file, s->code, s->code_size);
    }
    CloseHandle(file);
    if (!ok) { DeleteFileW(temporary); hos_stop(c, "nvn: failed writing shader capture"); }
    if (MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH))
        hos_log("nvn: captured shader program %s (%u stages)\n", key, p->count);
    else {
        DWORD error = GetLastError();
        DeleteFileW(temporary); /* Only the new file owned by this call is ever removed. */
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            hos_stop(c, "nvn: cannot publish shader capture (%lu)", error);
    }
}
