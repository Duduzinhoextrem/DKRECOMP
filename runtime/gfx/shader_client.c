/* Persistent, hidden translator process. Requests use actual bound resource types; caches include compiler identity.
 * Paths/derived shader files stay in the configured local cache, default reports/nvn_shader_cache in this checkout. */
#include "shader_client.h"
#include "perf.h"
#include "../src/hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static struct {
    wchar_t executable[4096], directory[4096];
    char directory_utf8[16384];
    uint8_t identity[32];
    HANDLE process, input, output;
    int initialized;
} client;

typedef struct Json { char *data; size_t size, capacity; } Json;
static void append(Cpu *c, Json *j, const char *format, ...) {
    for (;;) {
        if (j->capacity - j->size < 256) {
            size_t capacity = j->capacity ? j->capacity * 2 : 4096;
            if (capacity > 0x40000) hos_stop(c, "shader: request exceeds protocol limit");
            char *p = realloc(j->data, capacity);
            if (!p) hos_stop(c, "shader: out of memory creating request");
            j->data = p; j->capacity = capacity;
        }
        va_list args; va_start(args, format);
        int n = vsnprintf(j->data + j->size, j->capacity - j->size, format, args);
        va_end(args);
        if (n < 0) hos_stop(c, "shader: cannot format request");
        if ((size_t)n < j->capacity - j->size) { j->size += (size_t)n; return; }
        size_t capacity = j->capacity * 2;
        if (capacity > 0x40000) hos_stop(c, "shader: request exceeds protocol limit");
        char *p = realloc(j->data, capacity);
        if (!p) hos_stop(c, "shader: out of memory growing request");
        j->data = p; j->capacity = capacity;
    }
}
static void string(Cpu *c, Json *j, const char *value) {
    append(c, j, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\\' || *p == '"') append(c, j, "\\%c", *p);
        else if (*p < 32) append(c, j, "\\u%04x", *p);
        else append(c, j, "%c", *p);
    }
    append(c, j, "\"");
}
static void directory(Cpu *c, const wchar_t *path) {
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        hos_stop(c, "shader: cannot create local cache directory (%lu)", GetLastError());
}
/* <cache directory>/<name>, without stopping: callers that only want an optional file use this before any shader. */
int nvn_shader_cache_file(const wchar_t *name, wchar_t *out, unsigned count) {
    wchar_t root[4096], directory_path[4096];
    DWORD n = GetEnvironmentVariableW(L"DKR_SHADER_CACHE", directory_path, 4096);
    if (n >= 4096) return -1;
    if (!n) {
        if (!GetModuleFileNameW(NULL, root, 4096)) return -1;
        for (unsigned i = 0; i < 3; i++) {
            wchar_t *slash = wcsrchr(root, L'\\');
            if (!slash) return -1;
            *slash = 0;
        }
        swprintf(directory_path, 4096, L"%ls/reports", root); CreateDirectoryW(directory_path, NULL);
        swprintf(directory_path, 4096, L"%ls/reports/nvn_shader_cache", root);
    }
    if (!CreateDirectoryW(directory_path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    int written = swprintf(out, count, L"%ls/%ls", directory_path, name);
    return written > 0 && (unsigned)written < count ? 0 : -1;
}
static void initialize(Cpu *c) {
    if (client.initialized) return;
    wchar_t root[4096], path[4096];
    if (!GetModuleFileNameW(NULL, root, 4096)) hos_stop(c, "shader: cannot locate executable");
    for (unsigned i = 0; i < 3; i++) {
        wchar_t *slash = wcsrchr(root, L'\\');
        if (!slash) hos_stop(c, "shader: configure explicit translator/cache paths for this executable location");
        *slash = 0;
    }
    DWORD n = GetEnvironmentVariableW(L"DKR_SHADER_TRANSLATOR", client.executable, 4096);
    if (!n) swprintf(client.executable, 4096, L"%ls/build/shader_translator/bin/dkr_shader_translator.exe", root);
    else if (n >= 4096) hos_stop(c, "shader: translator path is too long");
    n = GetEnvironmentVariableW(L"DKR_SHADER_CACHE", client.directory, 4096);
    if (!n) {
        swprintf(path, 4096, L"%ls/reports", root); directory(c, path);
        swprintf(client.directory, 4096, L"%ls/reports/nvn_shader_cache", root);
    } else if (n >= 4096) hos_stop(c, "shader: cache path is too long");
    if (!(client.directory[1] == L':' && (client.directory[2] == L'/' || client.directory[2] == L'\\')) ||
        !(client.executable[1] == L':' && (client.executable[2] == L'/' || client.executable[2] == L'\\')))
        hos_stop(c, "shader: translator/cache paths must be absolute local paths");
    directory(c, client.directory);
    if (!WideCharToMultiByte(CP_UTF8, 0, client.directory, -1, client.directory_utf8, sizeof client.directory_utf8, NULL, NULL))
        hos_stop(c, "shader: cannot encode cache path");
    wcscpy(path, client.executable);
    wchar_t *extension = wcsrchr(path, L'.');
    if (!extension) hos_stop(c, "shader: translator must have an executable extension");
    wcscpy(extension, L".id");
    FILE *file = _wfopen(path, L"rb");
    char identity[65] = {0};
    if (!file || fread(identity, 1, 64, file) != 64) hos_stop(c, "shader: build the translator first (compiler identity missing)");
    fclose(file);
    for (unsigned i = 0; i < 32; i++) {
        unsigned byte;
        if (sscanf(identity + 2*i, "%2x", &byte) != 1) hos_stop(c, "shader: invalid compiler identity");
        client.identity[i] = (uint8_t)byte;
    }
    client.initialized = 1;
    atexit(nvn_shader_client_shutdown);
}

void nvn_shader_client_shutdown(void) {
    if (client.input) CloseHandle(client.input);
    if (client.output) CloseHandle(client.output);
    if (client.process) {
        if (WaitForSingleObject(client.process, 2000) == WAIT_TIMEOUT) TerminateProcess(client.process, 0);
        CloseHandle(client.process);
    }
    client.process = client.input = client.output = NULL;
}

static void start(Cpu *c) {
    if (client.process) return;
    SECURITY_ATTRIBUTES attributes = {sizeof attributes, NULL, TRUE};
    HANDLE child_in = NULL, child_out = NULL;
    if (!CreatePipe(&child_in, &client.input, &attributes, 0) || !CreatePipe(&client.output, &child_out, &attributes, 0) ||
        !SetHandleInformation(client.input, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(client.output, HANDLE_FLAG_INHERIT, 0))
        hos_stop(c, "shader: cannot create compiler pipes (%lu)", GetLastError());
    wchar_t log_path[4096], command[8192];
    swprintf(log_path, 4096, L"%ls/translator.log", client.directory);
    HANDLE log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) hos_stop(c, "shader: cannot open compiler log");
    swprintf(command, 8192, L"\"%ls\" --serve", client.executable);
    STARTUPINFOW startup = {.cb = sizeof startup, .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = child_in, .hStdOutput = child_out, .hStdError = log};
    PROCESS_INFORMATION process;
    BOOL ok = CreateProcessW(client.executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, client.directory, &startup, &process);
    DWORD error = GetLastError();
    CloseHandle(child_in); CloseHandle(child_out); CloseHandle(log);
    if (!ok) hos_stop(c, "shader: cannot start managed translator (%lu)", error);
    client.process = process.hProcess; CloseHandle(process.hThread);
    hos_log("shader: persistent managed translator started\n");
}

void nvn_shader_variant_key(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, char key[66]) {
    char variant[33];
    nvn_shader_digest(c, inputs, sizeof *inputs, NULL, 0, variant);
    snprintf(key, 66, "%s_%s", shader->key, variant);
}
static int integer_format(uint32_t format) {
    static const uint8_t uints[] = {3,8,0xb,0xf,0x14,0x17,0x1b,0x20,0x23,0x27,0x2c,0x2f,0x3e};
    static const uint8_t sints[] = {4,9,0xc,0x10,0x15,0x18,0x1c,0x21,0x24,0x28,0x2d,0x30,0x6a};
    for (unsigned i = 0; i < sizeof uints; i++) if (format == uints[i]) return 1;
    for (unsigned i = 0; i < sizeof sints; i++) if (format == sints[i]) return 2;
    return 0;
}
static int load(Cpu *c, const wchar_t *path, unsigned stage, DkrShaderCache *out) {
    FILE *file = _wfopen(path, L"rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    if (size < 64 || size > 0x1002000) { fclose(file); return 0; }
    uint8_t *data = malloc((size_t)size);
    if (!data) hos_stop(c, "shader: out of memory reading compiled cache");
    int ok = fread(data, 1, (size_t)size, file) == (size_t)size; fclose(file);
    char error[256];
    ok = ok && dkr_shader_cache_parse(data, (size_t)size, out, error) == 0;
    free(data);
    if (ok && (out->stage != stage || memcmp(out->compiler, client.identity, 32))) { dkr_shader_cache_free(out); ok = 0; }
    return ok;
}
static int spv_has_xfb(const DkrShaderCache *cache) {
    size_t words = cache->spv_size / 4;
    for (size_t p = 5; p < words;) {
        unsigned count = cache->spv[p] >> 16, op = cache->spv[p] & 0xffff;
        if (!count || count > words - p) break;
        if (op == 17 && count == 2 && cache->spv[p + 1] == 53) return 1; /* OpCapability TransformFeedback */
        if (op != 17) break; /* capabilities come first in SPIR-V modules */
        p += count;
    }
    return 0;
}
static int load_compatible_variant(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, DkrShaderCache *out) {
    wchar_t pattern[4096];
    swprintf(pattern, 4096, L"%ls/%hs_*.dkrspv", client.directory, shader->key);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        wchar_t path[4096];
        swprintf(path, 4096, L"%ls/%ls", client.directory, fd.cFileName);
        if (load(c, path, shader->stage, out)) {
            if (!out->notes && spv_has_xfb(out) == (inputs->transform_feedback != 0)) {
                found = 1;
                break;
            }
            dkr_shader_cache_free(out);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void translate(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, DkrShaderCache *cache);
void nvn_shader_translate(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, DkrShaderCache *cache) {
    int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
    translate(c, shader, inputs, cache);
    DKR_PERF_ADD(shader, dkr_perf_now() - t0);
}
static void translate(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, DkrShaderCache *cache) {
    initialize(c);
    char key[66]; nvn_shader_variant_key(c, shader, inputs, key);
    wchar_t binary[4096], code_path[4096];
    swprintf(binary, 4096, L"%ls/%hs.dkrspv", client.directory, key);
    if (load(c, binary, shader->stage, cache)) {
        if (cache->notes) hos_stop(c, "shader: cached variant %s still has unresolved resource assumptions", key);
        return;
    }
    if (load_compatible_variant(c, shader, inputs, cache)) return;
    unsigned skip = shader->stage == 5 ? (ld32((uint64_t)(uintptr_t)shader->code) == 0x12345679 ? 0x100 : 0) : 0x30;
    swprintf(code_path, 4096, L"%ls/%hs.maxwell", client.directory, key);
    FILE *file = _wfopen(code_path, L"wb");
    if (!file || fwrite(shader->code + skip, 1, shader->code_size - skip, file) != shader->code_size - skip)
        hos_stop(c, "shader: cannot write normalized Maxwell input");
    fclose(file);
    static const char *stages[] = {"vs", "ps", "gs", "tcs", "tes", "cs"};
    Json request = {0};
    append(c, &request, "{\"out\":"); string(c, &request, client.directory_utf8);
    append(c, &request, ",\"job\":{\"key\":\"%s\",\"stage\":\"%s\",\"debug\":false,\"tex_base\":32,\"tex_limit\":288,\"code\":", key, stages[shader->stage]);
    char code_utf8[18000];
    if (!WideCharToMultiByte(CP_UTF8, 0, code_path, -1, code_utf8, sizeof code_utf8, NULL, NULL)) hos_stop(c, "shader: cannot encode code path");
    string(c, &request, code_utf8);
    append(c, &request, ",\"c1\":\"");
    if (shader->code_size - shader->c1_offset > 65536) hos_stop(c, "shader: constant segment exceeds 64 KiB");
    for (unsigned i = shader->c1_offset; i < shader->code_size; i++) append(c, &request, "%02x", shader->code[i]);
    append(c, &request, "\",\"early_fragment_tests\":%s,\"tex\":{", shader->stage == 1 && shader->control[0x718] ? "true" : "false");
    static const char *types[] = {"1d", "2d", "3d", "1darray", "2darray", "2dms", "2dmsarray", "2d", "cube", "cubearray", "buffer"};
    unsigned comma = 0;
    for (unsigned i = 0; i < 32; i++) if (inputs->texture_mask & (1u << i)) {
        unsigned target = inputs->texture_targets[i];
        if (target >= sizeof types / sizeof *types) hos_stop(c, "shader: invalid bound texture target");
        append(c, &request, "%s\"%u\":\"%s%s\"", comma++ ? "," : "", i, types[target],
            inputs->texture_formats[i] >= 0x32 && inputs->texture_formats[i] <= 0x36 ? "_depth" : "");
    }
    static const char *kinds[] = {"float", "uint", "sint"};
    append(c, &request, "},\"attr\":{"); comma = 0;
    for (unsigned i = 0; i < 16; i++) if (inputs->attributes[i])
        append(c, &request, "%s\"%u\":\"%s\"", comma++ ? "," : "", i, kinds[integer_format(inputs->attributes[i])]);
    append(c, &request, "},\"fout\":{"); comma = 0;
    for (unsigned i = 0; i < 8; i++) if (inputs->outputs[i])
        append(c, &request, "%s\"%u\":\"%s\"", comma++ ? "," : "", i, kinds[integer_format(inputs->outputs[i])]);
    append(c, &request, "},\"primitive\":%u", inputs->primitive);
    if (inputs->transform_feedback) {
        NvnXfbLayout layout;
        if (nvn_shader_xfb_layout(shader, &layout)) hos_stop(c, "shader: unsupported transform feedback control layout");
        append(c, &request, ",\"xfb\":[");
        for (unsigned b = 0; b < 4; b++) {
            append(c, &request, "%s{\"stride\":%u,\"locations\":[", b ? "," : "", layout.strides[b]);
            for (unsigned i = 0; i < layout.counts[b]; i++) append(c, &request, "%s%u", i ? "," : "", layout.locations[b][i]);
            append(c, &request, "]}");
        }
        append(c, &request, "]");
    }
    if (shader->stage == 5) append(c, &request, ",\"local\":[%u,%u,%u],\"shared\":%u",
        ld32((uint64_t)(uintptr_t)shader->control + 0x748), ld32((uint64_t)(uintptr_t)shader->control + 0x74c),
        ld32((uint64_t)(uintptr_t)shader->control + 0x750), ld32((uint64_t)(uintptr_t)shader->control + 0x754));
    append(c, &request, "}}\n");
    DKR_PERF_ADD(shader_compiles, 1);
    start(c);
    DWORD written;
    if (!WriteFile(client.input, request.data, (DWORD)request.size, &written, NULL) || written != request.size)
        hos_stop(c, "shader: translator request failed (%lu)", GetLastError());
    free(request.data);
    char reply[8192]; unsigned length = 0; ULONGLONG start_tick = GetTickCount64(), deadline = start_tick + 30000;
    unsigned spins = 0;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(client.output, NULL, 0, NULL, &available, NULL)) hos_stop(c, "shader: translator pipe closed");
        if (!available) {
            if (WaitForSingleObject(client.process, 0) == WAIT_OBJECT_0) hos_stop(c, "shader: translator exited; inspect translator.log");
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) hos_stop(c, "shader: translator request timed out; inspect translator.log");
            if (spins++ < 2000) YieldProcessor();
            else if (now - start_tick < 20) SwitchToThread();
            else Sleep(1);
            continue;
        }
        char buf[512];
        DWORD to_read = available < sizeof buf ? available : (DWORD)sizeof buf, got = 0;
        if (!ReadFile(client.output, buf, to_read, &got, NULL) || !got) hos_stop(c, "shader: cannot read compiler reply");
        int done = 0;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n') { done = 1; break; }
            if (length + 1 == sizeof reply) hos_stop(c, "shader: compiler reply exceeds limit");
            if (buf[i] != '\r') reply[length++] = buf[i];
        }
        if (done) break;
    }
    reply[length] = 0;
    if (strncmp(reply, "ok ", 3) || strcmp(reply + 3, key)) hos_stop(c, "shader: %s", reply);
    if (!load(c, binary, shader->stage, cache)) hos_stop(c, "shader: translator produced an invalid/mismatched cache");
    if (cache->notes) hos_stop(c, "shader: variant %s has unresolved resource assumptions; inspect its reflection", key);
    hos_log("shader: compiled %s (%u bytes, %u bindings including support)\n", key, cache->spv_size, cache->binding_count);
}
