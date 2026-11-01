/* Horizon IPC: HIPC message layout, CMIF payloads, domains, and the session handle table.
 * Layout from switchbrew "IPC Marshalling":
 *   word0: type (bits 0-15), number of X (16-19), A (20-23), B (24-27), W (28-31) descriptors
 *   word1: raw data size in words (0-9), C descriptor flags (10-13), handle descriptor present (31)
 *   [handle descriptor: send PID (bit 0), copy count (1-4), move count (5-8); PID; copy handles; move handles]
 *   X descriptors (8 bytes), A, B, W descriptors (12 bytes each), raw data (CMIF payload aligned to 16 bytes),
 *   C descriptors (8 bytes each) after the raw data.
 * CMIF request payload: "SFCI", version, command id, token, arguments. Reply: "SFCO", version, result, token.
 * Domain request header (16 bytes) before SFCI: command (1 send, 2 close), input object count, payload size,
 * object id; input object ids follow the payload. Domain reply header: output object count, then SFCO, data and
 * the output object ids. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SFCI 0x49434653u
#define SFCO 0x4F434653u

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

int ipc_parse(const uint8_t *msg, int session_is_domain, IpcRequest *r, char *err, size_t errn) {
    memset(r, 0, sizeof *r);
    uint32_t h0 = rd32(msg), h1 = rd32(msg + 4);
    r->type = h0 & 0xFFFF;
    r->nx = (h0 >> 16) & 15;
    r->na = (h0 >> 20) & 15;
    r->nb = (h0 >> 24) & 15;
    r->nw = (h0 >> 28) & 15;
    uint32_t raw_words = h1 & 0x3FF, cflags = (h1 >> 10) & 15, off = 8;
    if (r->nx > 8 || r->na > 8 || r->nb > 8 || r->nw > 8) {
        snprintf(err, errn, "too many buffer descriptors");
        return -1;
    }
    if (h1 >> 31) {
        uint32_t d = rd32(msg + off);
        off += 4;
        r->has_pid = d & 1;
        r->ncopy = (d >> 1) & 15;
        r->nmove = (d >> 5) & 15;
        if (r->ncopy > 8 || r->nmove > 8) {
            snprintf(err, errn, "too many handles");
            return -1;
        }
        if (r->has_pid) { r->pid = rd64(msg + off); off += 8; }
        for (int i = 0; i < r->ncopy; i++, off += 4) r->copy[i] = rd32(msg + off);
        for (int i = 0; i < r->nmove; i++, off += 4) r->move[i] = rd32(msg + off);
    }
    for (int i = 0; i < r->nx; i++, off += 8) {
        uint32_t a = rd32(msg + off), b = rd32(msg + off + 4);
        r->x[i].size = a >> 16;
        r->x[i].addr = b | ((uint64_t)((a >> 12) & 0xF) << 32) | ((uint64_t)((a >> 6) & 0x7) << 36);
        r->x[i].flags = (a & 0x3F) | (((a >> 9) & 7) << 6);
    }
    IpcBuf *lists[3] = {r->a, r->b, r->w};
    int counts[3] = {r->na, r->nb, r->nw};
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < counts[l]; i++, off += 12) {
            uint32_t s = rd32(msg + off), a = rd32(msg + off + 4), c = rd32(msg + off + 8);
            lists[l][i].size = s | ((uint64_t)((c >> 24) & 0xF) << 32);
            lists[l][i].addr = a | ((uint64_t)((c >> 28) & 0xF) << 32) | ((uint64_t)((c >> 2) & 0x7) << 36);
            lists[l][i].flags = c & 3;
        }
    uint32_t raw = off, raw_end = off + raw_words * 4;
    uint32_t coff = raw_end;
    r->nc = cflags <= 1 ? 0 : cflags == 2 ? 1 : (int)cflags - 2;
    if (r->nc > 8) r->nc = 8;
    for (int i = 0; i < r->nc; i++, coff += 8) {
        uint32_t a = rd32(msg + coff), b = rd32(msg + coff + 4);
        r->c[i].addr = a | ((uint64_t)(b & 0xFFFF) << 32);
        r->c[i].size = b >> 16;
    }
    if (r->type == 2) return 0; /* close: no payload */
    uint32_t p = align16(raw);
    if (session_is_domain && (r->type == 4 || r->type == 6)) {
        r->domain = 1;
        r->domain_cmd = msg[p];
        r->nin_objects = msg[p + 1] > 8 ? 8 : msg[p + 1];
        uint32_t dsize = msg[p + 2] | (msg[p + 3] << 8);
        r->object_id = rd32(msg + p + 4);
        for (int i = 0; i < r->nin_objects; i++) r->in_objects[i] = rd32(msg + p + 16 + dsize + 4 * i);
        if (r->domain_cmd == 2) return 0; /* close object */
        p += 16;
        raw_end = p + dsize;
    }
    if (rd32(msg + p) != SFCI) {
        snprintf(err, errn, "missing SFCI magic (found %08x)", rd32(msg + p));
        return -1;
    }
    r->cmd = rd32(msg + p + 8);
    r->args = msg + p + 16;
    r->args_size = raw_end > p + 16 ? raw_end - (p + 16) : 0;
    return 0;
}

uint32_t ipc_write_reply(uint8_t *msg, uint32_t cap, int session_is_domain, const IpcReply *rep) {
    uint8_t out[0x200];
    memset(out, 0, sizeof out);
    uint32_t off = 8;
    int hd = rep->ncopy || rep->nmove;
    if (hd) {
        wr32(out + off, ((uint32_t)rep->ncopy << 1) | ((uint32_t)rep->nmove << 5));
        off += 4;
        for (int i = 0; i < rep->ncopy; i++, off += 4) wr32(out + off, rep->copy[i]);
        for (int i = 0; i < rep->nmove; i++, off += 4) wr32(out + off, rep->move[i]);
    }
    uint32_t raw = off, p = align16(off);
    if (session_is_domain) {
        wr32(out + p, (uint32_t)rep->nout_objects);
        p += 16;
    }
    wr32(out + p, SFCO);
    wr32(out + p + 8, rep->result);
    memcpy(out + p + 16, rep->data, rep->data_size);
    uint32_t end = p + 16 + rep->data_size;
    if (session_is_domain)
        for (int i = 0; i < rep->nout_objects; i++, end += 4) wr32(out + end, rep->out_objects[i]);
    uint32_t raw_words = (end - raw + 3) / 4 + 4; /* + the 16 bytes the sender reserves for alignment */
    wr32(out, 0);
    wr32(out + 4, raw_words | (hd ? 0x80000000u : 0));
    if (end > cap) end = cap;
    memcpy(msg, out, end);
    return end;
}

/* ---- sessions -------------------------------------------------------------------------------------------- */
#define HANDLE_BASE 0xD000u
#define MAX_SESSIONS 1024
#define MAX_OBJECTS 128
typedef struct Domain {
    Service *objs[MAX_OBJECTS]; /* object ids; 1 is the service the session was opened on */
    int refs;                   /* sessions sharing this domain */
} Domain;
typedef struct Session {
    int used;
    Service *root;
    Domain *dom; /* non-NULL once converted to a domain; clones of a domain session share it */
} Session;
static Session g_sess[MAX_SESSIONS];
static Session *g_current; /* the session of the request being handled */

static void retain_service(Service *s) { s->refs++; }
static void release_service(Service *s) {
    if (s && --s->refs == 0 && s->destroy) s->destroy(s);
}

uint32_t hos_new_session(Service *svc) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!g_sess[i].used) {
            memset(&g_sess[i], 0, sizeof g_sess[i]);
            g_sess[i].used = 1;
            g_sess[i].root = svc;
            retain_service(svc);
            return HANDLE_BASE + (uint32_t)i;
        }
    hos_log("ipc: out of session handles\n");
    return 0;
}

uint32_t hos_new_domain_object(Service *svc) {
    for (uint32_t id = 1; id < MAX_OBJECTS; id++)
        if (!g_current->dom->objs[id]) {
            g_current->dom->objs[id] = svc;
            retain_service(svc);
            return id;
        }
    return 0;
}

static Session *session_of(uint32_t handle) {
    uint32_t i = handle - HANDLE_BASE;
    return handle >= HANDLE_BASE && i < MAX_SESSIONS && g_sess[i].used ? &g_sess[i] : NULL;
}

static void close_session(Session *s) {
    release_service(s->root);
    if (s->dom && --s->dom->refs == 0) {
        for (int i = 1; i < MAX_OBJECTS; i++) release_service(s->dom->objs[i]);
        free(s->dom);
    }
    memset(s, 0, sizeof *s);
}

/* ---- events ---------------------------------------------------------------------------------------------- */
#define EVENT_BASE 0xE000u
#define MAX_EVENTS 256
typedef struct Event { unsigned refs; int signaled; } Event;
static Event g_events[MAX_EVENTS];
static Event *g_event_handles[MAX_EVENTS];

static Event *event_of(uint32_t handle) {
    uint32_t i = handle - EVENT_BASE;
    return i < MAX_EVENTS ? g_event_handles[i] : NULL;
}

static uint32_t new_event_handle(Event *event) {
    for (uint32_t i = 0; i < MAX_EVENTS; i++) {
        if (g_event_handles[i]) continue;
        g_event_handles[i] = event;
        event->refs++;
        return EVENT_BASE + i;
    }
    return 0;
}

uint32_t hos_new_event(int signaled) {
    for (int i = 0; i < MAX_EVENTS; i++)
        if (!g_events[i].refs) {
            g_events[i].signaled = signaled;
            return new_event_handle(&g_events[i]);
        }
    return 0;
}

uint32_t hos_copy_event_handle(uint32_t handle) {
    Event *event = event_of(handle);
    return event ? new_event_handle(event) : 0;
}

void hos_event_set(uint32_t handle, int signaled) {
    Event *event = event_of(handle);
    if (event) {
        event->signaled = signaled;
        if (signaled)
            for (uint32_t i = 0; i < MAX_EVENTS; i++)
                if (g_event_handles[i] == event) kern_sync_poke(EVENT_BASE + i);
    }
}

int hos_event_state(uint32_t handle) {
    Event *event = event_of(handle);
    return event ? event->signaled : -1;
}

/* Transfer memory lends an existing guest range to a service; no second copy of its backing is needed. */
#define TRANSFER_BASE 0xF000u
#define MAX_TRANSFERS 256
static struct { int used; HosTransferMemory memory; } g_transfers[MAX_TRANSFERS];

uint32_t hos_new_transfer_memory(uint64_t addr, uint64_t size, uint32_t perm) {
    for (uint32_t i = 0; i < MAX_TRANSFERS; i++) {
        if (g_transfers[i].used) continue;
        g_transfers[i].used = 1;
        g_transfers[i].memory = (HosTransferMemory){addr, size, perm};
        return TRANSFER_BASE + i;
    }
    return 0;
}

const HosTransferMemory *hos_transfer_memory(uint32_t handle) {
    uint32_t i = handle - TRANSFER_BASE;
    return handle >= TRANSFER_BASE && i < MAX_TRANSFERS && g_transfers[i].used ? &g_transfers[i].memory : NULL;
}

uint32_t hos_close_handle(uint32_t handle) {
    Session *s = session_of(handle);
    if (s) close_session(s);
    Event *event = event_of(handle);
    if (event) {
        g_event_handles[handle - EVENT_BASE] = NULL;
        event->refs--;
    }
    if (hos_transfer_memory(handle)) g_transfers[handle - TRANSFER_BASE].used = 0;
#ifdef _WIN32
    hos_close_shared_memory(handle);
#endif
    return HOS_OK; /* other handle kinds are not tracked yet */
}

void hos_reply_interface(IpcReply *rp, Service *svc) {
    if (g_current && g_current->dom) rp->out_objects[rp->nout_objects++] = hos_new_domain_object(svc);
    else rp->move[rp->nmove++] = hos_new_session(svc);
}

void hos_unimplemented(Cpu *c, const Service *s, const IpcRequest *rq) {
    hos_dump_request(rq);
    hos_stop(c, "%s command %u is not implemented", s->name, rq->cmd);
}

void hos_dump_request(const IpcRequest *rq) {
    hos_log("     request: type %u cmd %u, args %u bytes:", rq->type, rq->cmd, rq->args_size);
    for (uint32_t i = 0; i < rq->args_size && i < 96; i += 4) hos_log(" %08x", *(const uint32_t *)(rq->args + i));
    hos_log("\n     buffers: %d X, %d A, %d B, %d W, %d C; pid %d, %d copy, %d move handles\n", rq->nx, rq->na,
            rq->nb, rq->nw, rq->nc, rq->has_pid, rq->ncopy, rq->nmove);
    for (int i = 0; i < rq->ncopy; i++) hos_log("       copy%d %#x\n", i, rq->copy[i]);
    for (int i = 0; i < rq->nmove; i++) hos_log("       move%d %#x\n", i, rq->move[i]);
    for (int i = 0; i < rq->nx; i++) hos_log("       X%d %#llx +%#llx\n", i, (unsigned long long)rq->x[i].addr, (unsigned long long)rq->x[i].size);
    for (int i = 0; i < rq->na; i++) hos_log("       A%d %#llx +%#llx\n", i, (unsigned long long)rq->a[i].addr, (unsigned long long)rq->a[i].size);
    for (int i = 0; i < rq->nb; i++) hos_log("       B%d %#llx +%#llx\n", i, (unsigned long long)rq->b[i].addr, (unsigned long long)rq->b[i].size);
    for (int i = 0; i < rq->nc; i++) hos_log("       C%d %#llx +%#llx\n", i, (unsigned long long)rq->c[i].addr, (unsigned long long)rq->c[i].size);
}

uint32_t hos_send_sync_request(Cpu *c, uint32_t handle, uint8_t *msg, uint32_t cap) {
    Session *s = session_of(handle);
    if (!s) {
        hos_log("ipc: request on unknown handle %#x\n", handle);
        return HOS_INVALID_HANDLE;
    }
    IpcRequest rq;
    IpcReply rp;
    char err[96];
    memset(&rp, 0, sizeof rp);
    if (ipc_parse(msg, s->dom != NULL, &rq, err, sizeof err)) hos_stop(c, "ipc: malformed request to %s: %s", s->root->name, err);
    g_current = s;
    if (rq.type == 2) { /* close session */
        close_session(s);
        return HOS_OK;
    }
    static int log_ipc = -1;
    if (log_ipc < 0) log_ipc = getenv("DKR_LOG_IPC") != NULL;
    if (rq.type == 5 || rq.type == 7) { /* control */
        if (log_ipc) hos_log("     control %u on %s\n", rq.cmd, s->root->name);
        switch (rq.cmd) {
        case 0: /* ConvertCurrentObjectToDomain -> object id */
            if (!s->dom) {
                s->dom = calloc(1, sizeof *s->dom);
                s->dom->refs = 1;
                s->dom->objs[1] = s->root;
                retain_service(s->root);
            }
            RP_PUT(&rp, uint32_t, 1);
            ipc_write_reply(msg, cap, 0, &rp);
            return HOS_OK;
        case 2: case 4: { /* CloneCurrentObject(Ex) -> move handle; a clone of a domain session is the same domain */
            uint32_t h = hos_new_session(s->root);
            Session *n = session_of(h);
            if (n && s->dom) {
                n->dom = s->dom;
                s->dom->refs++;
            }
            rp.move[rp.nmove++] = h;
            ipc_write_reply(msg, cap, 0, &rp);
            return HOS_OK;
        }
        case 3: /* QueryPointerBufferSize -> u16 */
            RP_PUT(&rp, uint16_t, 0x8000);
            ipc_write_reply(msg, cap, 0, &rp);
            return HOS_OK;
        default:
            hos_dump_request(&rq);
            hos_stop(c, "ipc: control command %u is not implemented", rq.cmd);
        }
    }
    Service *target = s->root;
    if (rq.domain) {
        if (rq.object_id >= MAX_OBJECTS || !s->dom->objs[rq.object_id])
            hos_stop(c, "ipc: domain object %u of %s does not exist", rq.object_id, s->root->name);
        target = s->dom->objs[rq.object_id];
        if (rq.domain_cmd == 2) { /* close object */
            release_service(target);
            s->dom->objs[rq.object_id] = NULL;
            ipc_write_reply(msg, cap, 1, &rp);
            return HOS_OK;
        }
    }
    if (log_ipc) hos_log("     -> %s cmd %u%s\n", target->name, rq.cmd, rq.domain ? " (domain)" : "");
    if (!target->handle) {
        hos_dump_request(&rq);
        hos_stop(c, "service \"%s\" is not implemented (first command %u)", target->name, rq.cmd);
    }
    target->handle(c, target, &rq, &rp);
    ipc_write_reply(msg, cap, s->dom != NULL, &rp);
    return HOS_OK;
}
