/* NVN's register ABI, proc lookup and the exact 16-byte device builder contract measured in the target SDK. */
#include "dkr/nvn.h"
#include "../../runtime/src/hos.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); exit(3);
}

int main(void) {
    dkr_nvn_register(NULL, 0);
    uint64_t defaults = dkr_nvn_proc_address("nvnDeviceBuilderSetDefaults");
    uint64_t flags = dkr_nvn_proc_address("nvnDeviceBuilderSetFlags");
    uint64_t lookup = dkr_nvn_proc_address("nvnDeviceGetProcAddress");
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
    printf("NVN entry ABI: %s (lookup, legacy names, exact builder writes, register preservation)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
