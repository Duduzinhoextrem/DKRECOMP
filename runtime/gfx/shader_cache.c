#include "shader_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t word(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int fail(char error[256], const char *message) { snprintf(error, 256, "%s", message); return -1; }
void dkr_shader_cache_free(DkrShaderCache *cache) { free(cache->spv); memset(cache, 0, sizeof *cache); }

int dkr_shader_cache_parse(const void *data, size_t size, DkrShaderCache *out, char error[256]) {
    const uint8_t *p = data;
    memset(out, 0, sizeof *out);
    if (!data || size < 64 || memcmp(p, "DKRSHD01", 8) || word(p + 8) != 1) return fail(error, "invalid shader cache magic/version");
    uint32_t stage = word(p + 12), support = word(p + 16), bindings = word(p + 20), spv_size = word(p + 24);
    if (stage > 5 || !bindings || bindings > 128 || support < 16 || support > 65536 || support % 16 ||
        spv_size < 20 || spv_size > 0x1000000 || spv_size % 4 || size != 64u + (uint64_t)bindings * 32 + spv_size)
        return fail(error, "invalid shader cache stage/count/length");
    unsigned support_count = 0;
    for (unsigned i = 0; i < bindings; i++) {
        const uint8_t *record = p + 64 + i * 32;
        DkrShaderBinding *b = &out->bindings[i];
        b->binding = word(record); b->kind = word(record + 4); b->slot = (int32_t)word(record + 8);
        b->cb_slot = (int32_t)word(record + 12); b->handle = (int32_t)word(record + 16);
        b->texture_type = word(record + 20); b->count = word(record + 24); b->flags = word(record + 28);
        if (b->binding > 4095 || b->kind > DKR_SHADER_STORAGE_TEXEL || !b->count || b->count > 1024)
            return fail(error, "invalid shader cache binding");
        for (unsigned j = 0; j < i; j++) if (out->bindings[j].binding == b->binding)
            return fail(error, "duplicate shader cache binding");
        if (b->kind == DKR_SHADER_SUPPORT) {
            if (b->binding || b->slot != -1 || b->count != 1) return fail(error, "invalid support-buffer binding");
            support_count++;
        }
        if (b->kind == DKR_SHADER_CBUF && (b->slot < 0 || b->slot > 31)) return fail(error, "invalid constant-buffer slot");
    }
    if (support_count != 1) return fail(error, "missing support-buffer binding");
    const uint8_t *code = p + 64 + bindings * 32;
    if (word(code) != 0x07230203 || word(code + 16)) return fail(error, "invalid SPIR-V header");
    static const unsigned models[] = {0, 4, 3, 1, 2, 5};
    unsigned entries = 0;
    for (size_t i = 20; i < spv_size;) {
        uint32_t instruction = word(code + i), count = instruction >> 16, op = instruction & 65535;
        if (!count || count > (spv_size - i) / 4) return fail(error, "truncated SPIR-V instruction");
        if (op == 15) {
            if (count < 5 || word(code + i + 4) != models[stage]) return fail(error, "SPIR-V stage does not match cache");
            entries++;
        }
        i += count * 4;
    }
    if (entries != 1) return fail(error, "invalid SPIR-V entry-point count");
    out->spv = malloc(spv_size);
    if (!out->spv) return fail(error, "out of memory reading shader cache");
    for (unsigned i = 0; i < spv_size / 4; i++) out->spv[i] = word(code + i * 4);
    out->stage = stage; out->support_size = support; out->binding_count = bindings;
    out->spv_size = spv_size; out->notes = word(p + 28); memcpy(out->compiler, p + 32, 32);
    error[0] = 0;
    return 0;
}
