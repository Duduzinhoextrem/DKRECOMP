/* IPC message format tests (runtime/src/ipc.c), built by CMake. The first message is the one the game's SDK
 * sends to sm: at boot (its header words were logged by dkctf.exe: 00000004 8000000a 00000001 ...). */
#include "../../runtime/src/hos.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* ipc.c's dependencies, reduced to what the parser and writer need. */
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(2);
}
Service *hos_get_service(const char *name) { (void)name; return NULL; }
void kern_sync_poke(uint32_t handle) { (void)handle; }

static uint8_t msg[0x100];
static void put32(int off, uint32_t v) { memcpy(msg + off, &v, 4); }
static uint32_t get32(int off) { uint32_t v; memcpy(&v, msg + off, 4); return v; }

static void request(uint32_t type, uint32_t cmd, uint32_t object) {
    memset(msg, 0, sizeof msg);
    put32(0, type);
    put32(4, object ? 0x10 : 0xc);
    int p = 0x10;
    if (object) {
        msg[p] = 1;
        msg[p + 2] = 0x10;
        put32(p + 4, object);
        p += 16;
    }
    put32(p, 0x49434653);
    put32(p + 8, cmd);
}

static void leaf_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    (void)c; (void)s;
    CHECK(rq->cmd == 7);
    RP_PUT(rp, uint32_t, 0xcafe);
}

static int leaf_destroyed;
static void leaf_destroy(Service *s) { (void)s; leaf_destroyed++; }

static void root_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    (void)c; (void)s;
    static Service leaf = {"leaf", leaf_handle, NULL, leaf_destroy};
    CHECK(rq->cmd == 5);
    hos_reply_interface(rp, &leaf);
}

static void test_domain_clones(void) {
    Cpu cpu = {0};
    Service root = {"root", root_handle, NULL};
    uint32_t session = hos_new_session(&root);
    request(5, 0, 0); /* convert to domain */
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK);
    CHECK(get32(0x20) == 1);
    request(4, 5, 1); /* root creates a child object */
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK);
    uint32_t child = get32(0x30);
    CHECK(child > 1 && get32(0x10) == 1);
    request(5, 2, 0); /* clone shares the domain, including already-created child objects */
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK);
    uint32_t clone = get32(12);
    CHECK(clone && clone != session);
    CHECK(hos_close_handle(session) == HOS_OK);
    CHECK(leaf_destroyed == 0);
    request(4, 7, child);
    CHECK(hos_send_sync_request(&cpu, clone, msg, sizeof msg) == HOS_OK);
    CHECK(get32(0x28) == HOS_OK && get32(0x30) == 0xcafe);
    CHECK(hos_close_handle(clone) == HOS_OK);
    CHECK(leaf_destroyed == 1);
    CHECK(hos_send_sync_request(&cpu, clone, msg, sizeof msg) == HOS_INVALID_HANDLE);
}

int main(void) {
    IpcRequest r;
    char err[96];

    /* sm:Initialize: request, raw 10 words, handle descriptor with send-PID; payload at 0x20 */
    memset(msg, 0, sizeof msg);
    put32(0, 0x00000004); put32(4, 0x8000000a); put32(8, 0x00000001);
    put32(0x20, 0x49434653); put32(0x28, 0); /* SFCI, cmd 0 */
    CHECK(ipc_parse(msg, 0, &r, err, sizeof err) == 0);
    CHECK(r.type == 4 && r.has_pid && r.ncopy == 0 && r.nmove == 0 && r.cmd == 0);
    CHECK(r.args == msg + 0x30);

    /* sm:GetService("fsp-srv"): no handle descriptor, payload at 0x10, name as the first argument */
    memset(msg, 0, sizeof msg);
    put32(0, 0x00000004); put32(4, 0x0000000a);
    put32(0x10, 0x49434653); put32(0x18, 1);
    memcpy(msg + 0x20, "fsp-srv\0", 8);
    CHECK(ipc_parse(msg, 0, &r, err, sizeof err) == 0);
    CHECK(r.cmd == 1 && !memcmp(r.args, "fsp-srv", 7));

    /* X and B descriptors and one C descriptor, decoded with their split address bits */
    memset(msg, 0, sizeof msg);
    put32(0, 0x01010004);                 /* 1 X, 1 B */
    put32(4, 0x00000808);                 /* raw 8 words, C flags 2 (one descriptor) */
    put32(8, (0x40u << 16) | (0x3u << 12) | (0x5u << 6) | 0x2); put32(12, 0x11223344); /* X: size 0x40 */
    put32(16, 0x100); put32(20, 0x55667788); put32(24, (0x2u << 28) | (0x1u << 2) | 1); /* B: size 0x100 */
    put32(0x20, 0x49434653); put32(0x28, 7);
    put32(28 + 8 * 4, 0x99AABBCC); put32(28 + 8 * 4 + 4, (0x200u << 16) | 0x3); /* C after the raw data */
    CHECK(ipc_parse(msg, 0, &r, err, sizeof err) == 0);
    CHECK(r.nx == 1 && r.x[0].size == 0x40 && r.x[0].addr == ((5ull << 36) | (3ull << 32) | 0x11223344));
    CHECK(r.x[0].flags == 2);
    CHECK(r.nb == 1 && r.b[0].size == 0x100 && r.b[0].addr == ((1ull << 36) | (2ull << 32) | 0x55667788));
    CHECK(r.b[0].flags == 1);
    CHECK(r.nc == 1 && r.c[0].addr == ((3ull << 32) | 0x99AABBCC) && r.c[0].size == 0x200);
    CHECK(r.cmd == 7);

    /* domain request: header (send, 0 objects, 0x18 bytes, object 3), then SFCI */
    memset(msg, 0, sizeof msg);
    put32(0, 0x00000004); put32(4, 0x0000000e);
    msg[0x10] = 1; msg[0x12] = 0x18; put32(0x14, 3);
    put32(0x20, 0x49434653); put32(0x28, 5);
    CHECK(ipc_parse(msg, 1, &r, err, sizeof err) == 0);
    CHECK(r.domain && r.domain_cmd == 1 && r.object_id == 3 && r.cmd == 5 && r.args_size == 8);

    /* missing magic is reported, not misread */
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 8);
    CHECK(ipc_parse(msg, 0, &r, err, sizeof err) == -1);

    /* reply with one move handle and a u32: handle descriptor, SFCO at 0x10, result, data */
    IpcReply rp;
    memset(&rp, 0, sizeof rp);
    rp.move[rp.nmove++] = 0xD005;
    RP_PUT(&rp, uint32_t, 0xCAFE);
    memset(msg, 0xEE, sizeof msg);
    ipc_write_reply(msg, sizeof msg, 0, &rp);
    CHECK(get32(0) == 0 && (get32(4) >> 31) == 1);
    CHECK(get32(8) == (1u << 5) && get32(12) == 0xD005);
    CHECK(get32(0x10) == 0x4F434653 && get32(0x18) == 0 && get32(0x20) == 0xCAFE);

    /* domain reply: output object count, then SFCO 16 bytes later, then data and object ids */
    memset(&rp, 0, sizeof rp);
    rp.out_objects[rp.nout_objects++] = 2;
    ipc_write_reply(msg, sizeof msg, 1, &rp);
    CHECK(get32(0x10) == 1 && get32(0x20) == 0x4F434653 && get32(0x30) == 2);

    test_domain_clones();
    uint32_t transfer = hos_new_transfer_memory(0x1001000000ull, 0x800000, 0);
    const HosTransferMemory *memory = hos_transfer_memory(transfer);
    CHECK(transfer && memory && memory->addr == 0x1001000000ull && memory->size == 0x800000 && memory->perm == 0);
    CHECK(hos_close_handle(transfer) == HOS_OK && !hos_transfer_memory(transfer));
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
