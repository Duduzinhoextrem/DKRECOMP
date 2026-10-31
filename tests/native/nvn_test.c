/* NVN's register ABI, proc lookup and the exact 16-byte device builder contract measured in the target SDK. */
#include "dkr/nvn.h"
#include "../../runtime/src/hos.h"
#include "../../runtime/gfx/nvn_commands.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); exit(3);
}

static int builder_probe(uint64_t defaults, uint64_t flags) {
    /* Binary test protocol: records {u64 operation, u64 flags, 64 bytes including guards}; return 64 bytes.
     * This allows a local original-SDK oracle to compare the actual host bridge without embedding game code. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    for (;;) {
        uint64_t header[2];
        uint8_t memory[64];
        size_t n = fread(header, 1, sizeof header, stdin);
        if (!n && feof(stdin)) return 0;
        if (n != sizeof header || fread(memory, 1, sizeof memory, stdin) != sizeof memory || header[0] > 1) return 2;
        Cpu cpu = {0};
        cpu.x[0] = (uint64_t)(uintptr_t)(memory + 16);
        cpu.x[1] = header[1];
        rt_call(&cpu, header[0] ? flags : defaults);
        if (fwrite(memory, 1, sizeof memory, stdout) != sizeof memory) return 2;
    }
}

static void invoke(Cpu *c, const char *name) { rt_call(c, dkr_nvn_proc_address(name)); }

static void check_record(const NvnRecord *r, void *user) {
    unsigned *position = user;
    CHECK(r->copy_row_stride == 0x100000010ll && r->copy_image_stride == 0x200000040ll);
    switch ((*position)++ % 7) {
    case 0:
        CHECK(!strcmp(r->name, "nvnCommandBufferBindBlendState"));
        CHECK(ld64(r->x[1]) == 0x12345678abcdef00ull);
        break;
    case 1: {
        CHECK(!strcmp(r->name, "nvnCommandBufferSetRenderTargets"));
        CHECK(r->x[1] == 2 && ld64(r->x[2]) == 0x1234 && ld64(r->x[2] + 8) == 0x5678);
        CHECK(ld64(r->x[3] + 8) == 0);
        uint64_t view = ld64(r->x[3]);
        for (unsigned i = 0; i < 0x24; i++) CHECK(ld8(view + i) == i + 1);
        CHECK(r->x[4] == 0 && r->x[5] == 0);
        break;
    }
    case 2:
        CHECK(!strcmp(r->name, "nvnCommandBufferUpdateUniformBuffer"));
        CHECK(r->x[1] == 0x90000000 && r->x[3] == 16 && r->x[4] == 16);
        for (unsigned i = 0; i < 16; i++) CHECK(ld8(r->x[5] + i) == 0x40 + i);
        break;
    case 3:
        CHECK(!strcmp(r->name, "nvnCommandBufferBindVertexBuffers"));
        CHECK(r->x[2] == 2 && ld64(r->x[3]) == 0x1122 && ld64(r->x[3] + 24) == 0x88);
        break;
    case 4:
        CHECK(!strcmp(r->name, "nvnCommandBufferSetPolygonOffsetClamp"));
        CHECK(r->v[0] == 0x1234567800000000ull && r->v[1] == 0xabcdef0100000000ull);
        break;
    case 5:
        CHECK(!strcmp(r->name, "nvnCommandBufferBindTextures"));
        CHECK(r->x[3] == 0 && r->x[4] == 0);
        break;
    case 6:
        CHECK(!strcmp(r->name, "nvnCommandBufferSetViewports"));
        CHECK(r->x[1] == 3 && r->x[2] == 2 && ld32(r->x[3]) == 7 && ld32(r->x[3] + 28) == 14);
        break;
    }
}

static void test_command_lifetime(void) {
    Cpu c = {0};
    uint8_t command_a[0x90] = {0}, command_b[0x90] = {0}, view[0x24], uniform[16];
    uint64_t a = (uint64_t)(uintptr_t)command_a, b = (uint64_t)(uintptr_t)command_b;
    uint64_t blend = 0x12345678abcdef00ull, textures[] = {0x1234, 0x5678};
    uint64_t views[] = {(uint64_t)(uintptr_t)view, 0}, ranges[] = {0x1122, 0x44, 0x5566, 0x88};
    uint32_t viewports[] = {7, 8, 9, 10, 11, 12, 13, 14};
    for (unsigned i = 0; i < sizeof view; i++) view[i] = (uint8_t)(i + 1);
    for (unsigned i = 0; i < sizeof uniform; i++) uniform[i] = (uint8_t)(0x40 + i);
    c.x[0] = a; invoke(&c, "nvnCommandBufferInitialize");
    c.x[0] = a; c.x[1] = 0x100000010ull; invoke(&c, "nvnCommandBufferSetCopyRowStride");
    c.x[1] = 0x200000040ull; invoke(&c, "nvnCommandBufferSetCopyImageStride");
    c.x[0] = a; c.x[1] = 0x3000000000ull; c.x[2] = 0x1000; invoke(&c, "nvnCommandBufferAddControlMemory");
    c.x[0] = a; invoke(&c, "nvnCommandBufferBeginRecording");
    c.x[1] = (uint64_t)(uintptr_t)&blend; invoke(&c, "nvnCommandBufferBindBlendState");
    c.x[1] = 2; c.x[2] = (uint64_t)(uintptr_t)textures; c.x[3] = (uint64_t)(uintptr_t)views;
    c.x[4] = c.x[5] = 0; invoke(&c, "nvnCommandBufferSetRenderTargets");
    c.x[1] = 0x90000000; c.x[2] = 64; c.x[3] = 16; c.x[4] = 16; c.x[5] = (uint64_t)(uintptr_t)uniform;
    invoke(&c, "nvnCommandBufferUpdateUniformBuffer");
    c.x[1] = 0; c.x[2] = 2; c.x[3] = (uint64_t)(uintptr_t)ranges;
    invoke(&c, "nvnCommandBufferBindVertexBuffers");
    c.v[0].d[0] = 0x1234567800000000ull; c.v[1].d[0] = 0xabcdef0100000000ull;
    invoke(&c, "nvnCommandBufferSetPolygonOffsetClamp");
    c.x[1] = c.x[2] = c.x[3] = 0; c.x[4] = 1; /* zero count must not read a poison pointer */
    invoke(&c, "nvnCommandBufferBindTextures");
    c.x[1] = 3; c.x[2] = 2; c.x[3] = (uint64_t)(uintptr_t)viewports;
    invoke(&c, "nvnCommandBufferSetViewports");
    invoke(&c, "nvnCommandBufferEndRecording");
    uint64_t child = c.x[0];
    blend = 0; memset(textures, 0, sizeof textures); memset(views, 0, sizeof views);
    memset(view, 0, sizeof view); memset(uniform, 0, sizeof uniform); memset(ranges, 0, sizeof ranges);
    memset(viewports, 0, sizeof viewports); memset(c.v, 0, sizeof c.v);
    c.x[0] = b; invoke(&c, "nvnCommandBufferInitialize");
    c.x[0] = b; invoke(&c, "nvnCommandBufferBeginRecording");
    c.x[1] = 1; c.x[2] = (uint64_t)(uintptr_t)&child; invoke(&c, "nvnCommandBufferCallCommands");
    invoke(&c, "nvnCommandBufferCopyCommands");
    invoke(&c, "nvnCommandBufferEndRecording");
    uint64_t parent = c.x[0];
    c.x[0] = a; c.x[1] = 0x3000000000ull; c.x[2] = 0x1000; invoke(&c, "nvnCommandBufferAddControlMemory");
    invoke(&c, "nvnCommandBufferFinalize"); /* invalidate child handle and its command buffer */
    unsigned visited = 0;
    dkr_nvn_visit_commands(&c, &parent, 1, check_record, &visited);
    CHECK(visited == 14);
    c.x[0] = b; invoke(&c, "nvnCommandBufferFinalize");
}

int main(int argc, char **argv) {
    dkr_nvn_register(NULL, 0);
    uint64_t defaults = dkr_nvn_proc_address("nvnDeviceBuilderSetDefaults");
    uint64_t flags = dkr_nvn_proc_address("nvnDeviceBuilderSetFlags");
    uint64_t lookup = dkr_nvn_proc_address("nvnDeviceGetProcAddress");
    if (argc == 2 && !strcmp(argv[1], "--builder-probe")) return builder_probe(defaults, flags);
    CHECK(defaults && flags && lookup);
    CHECK(dkr_nvn_proc_address("nvnDeviceApplyDeferredFinalizes"));
    CHECK(dkr_nvn_proc_address("nvnQueueAcquireTexture"));
    CHECK(!dkr_nvn_proc_address("nvnDKRecompUnknownFunction"));
    uint8_t storage[48];
    Cpu cpu = {0};
    for (unsigned i = 0; i < 31; i++) cpu.x[i] = 0x12340000ull + i;
    cpu.nzcv = 0xa0000000u;
    cpu.x[0] = (uint64_t)(uintptr_t)(storage + 16);
    cpu.pc = defaults;
    Cpu saved = cpu;
    memset(storage, 0xa5, sizeof storage);
    rt_call(&cpu, defaults);
    CHECK(!memcmp(&cpu, &saved, sizeof cpu));
    for (unsigned i = 0; i < sizeof storage; i++) CHECK(storage[i] == (i >= 16 && i < 32 ? 0 : 0xa5));
    const uint64_t values[] = {0, 1, 0x80000000ull, 0xffffffffull, 0xabcdef0112345678ull};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        memset(storage + 16, 0x5a, 16);
        cpu.x[1] = values[i]; cpu.pc = flags; saved = cpu;
        rt_call(&cpu, flags);
        CHECK(!memcmp(&cpu, &saved, sizeof cpu));
        uint32_t result;
        memcpy(&result, storage + 16, 4);
        CHECK(result == (uint32_t)values[i]);
        for (unsigned j = 20; j < 32; j++) CHECK(storage[j] == 0x5a);
        CHECK(storage[15] == 0xa5 && storage[32] == 0xa5);
    }
    cpu.x[1] = (uint64_t)(uintptr_t)"nvnDeviceBuilderSetDefaults";
    rt_call(&cpu, lookup);
    CHECK(cpu.x[0] == defaults);
    cpu.x[0] = (uint64_t)(uintptr_t)"nvnDeviceBuilderSetFlags";
    dkr_nvn_bootstrap(&cpu);
    CHECK(cpu.x[0] == flags);
    uint8_t pool_builder[32] = {0}, pool[0x110];
    memset(pool, 0xa5, sizeof pool);
    st64((uint64_t)(uintptr_t)pool_builder + 8, 0x4000000000ull);
    st32((uint64_t)(uintptr_t)pool_builder + 0x18, 0x62);
    cpu.x[0] = (uint64_t)(uintptr_t)pool;
    cpu.x[1] = (uint64_t)(uintptr_t)pool_builder;
    rt_call(&cpu, dkr_nvn_proc_address("nvnMemoryPoolInitialize"));
    CHECK(cpu.x[0] == 1);
    cpu.x[0] = (uint64_t)(uintptr_t)pool;
    rt_call(&cpu, dkr_nvn_proc_address("nvnMemoryPoolGetSize"));
    CHECK(cpu.x[0] == 0);
    test_command_lifetime();
    for (unsigned i = 0x100; i < sizeof pool; i++) CHECK(pool[i] == 0xa5);
    cpu.x[0] = (uint64_t)(uintptr_t)pool_builder;
    rt_call(&cpu, dkr_nvn_proc_address("nvnTextureBuilderGetZCullStorageSize"));
    CHECK(cpu.x[0] == 0);
    printf("NVN entry ABI: %s (lookup, legacy names, exact builder writes, register preservation)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
