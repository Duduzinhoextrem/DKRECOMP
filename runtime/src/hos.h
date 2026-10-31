/* Horizon layer internals shared by horizon.c (SVCs, boot), ipc.c (message format, sessions) and services.c.
 * Message layout references: switchbrew "IPC Marshalling" (HIPC header, descriptors, CMIF "SFCI"/"SFCO" payload,
 * domain headers). Not part of the runtime's public API. */
#ifndef DKR_HOS_H
#define DKR_HOS_H

#include "dkr/rt.h"

/* Result codes: module | description << 9. */
#define HOS_OK 0u
#define HOS_KERNEL(desc) (1u | ((desc) << 9))
#define HOS_INVALID_HANDLE HOS_KERNEL(114)
#define HOS_TIMED_OUT HOS_KERNEL(117)
#define HOS_SF_NOT_IMPLEMENTED (10u | (221u << 9)) /* sf: unknown command id */

void hos_log(const char *fmt, ...);
#if defined(_MSC_VER)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
void hos_stop(Cpu *c, const char *fmt, ...);

/* ---- parsed request ------------------------------------------------------------------------------------- */
typedef struct IpcBuf {
    uint64_t addr, size;
    uint32_t flags; /* X: counter; A/B/W: transfer flags */
} IpcBuf;

typedef struct IpcRequest {
    uint32_t type;                 /* 2 close, 4 request, 5 control, 6/7 with context */
    int has_pid;
    uint64_t pid;
    uint32_t copy[8], move[8];
    int ncopy, nmove;
    IpcBuf x[8], a[8], b[8], w[8], c[8];
    int nx, na, nb, nw, nc;
    /* CMIF payload */
    int domain;                    /* message carried a domain header */
    uint8_t domain_cmd;            /* 1 send message, 2 close object */
    uint32_t object_id;
    uint32_t in_objects[8];
    int nin_objects;
    uint32_t cmd;
    const uint8_t *args;           /* arguments after the 16-byte SFCI header */
    uint32_t args_size;
} IpcRequest;

typedef struct IpcReply {
    uint32_t result;
    uint8_t data[0x100];           /* output arguments after the SFCO header */
    uint32_t data_size;
    uint32_t copy[8], move[8];
    int ncopy, nmove;
    uint32_t out_objects[8];       /* domain object ids created by this command */
    int nout_objects;
} IpcReply;

/* Parse the request at `msg`. Returns 0, or -1 with a reason in `err`. */
int ipc_parse(const uint8_t *msg, int session_is_domain, IpcRequest *r, char *err, size_t errn);
/* Write the reply into `msg` (the request buffer, as the kernel does). Returns the bytes written. */
uint32_t ipc_write_reply(uint8_t *msg, uint32_t cap, int session_is_domain, const IpcReply *rep);

/* ---- services, sessions, handles ------------------------------------------------------------------------- */
typedef struct Service Service;
typedef void (*ServiceFn)(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp);
struct Service {
    char name[32];
    ServiceFn handle;      /* NULL: unknown service, logged and stopped at its first command */
    void *state;
};

/* New handle for a session to `svc` (a fresh session; not a domain until the guest converts it). */
uint32_t hos_new_session(Service *svc);
/* New domain object in the session the current request came from; returns its object id. */
uint32_t hos_new_domain_object(Service *svc);
/* SendSyncRequest on `handle` with the message at `msg`. Returns the SVC result. */
uint32_t hos_send_sync_request(Cpu *c, uint32_t handle, uint8_t *msg, uint32_t cap);
uint32_t hos_close_handle(uint32_t handle);
/* Kernel events (readable handles a service hands out). One thread only for now: nothing else can signal. */
uint32_t hos_new_event(int signaled);
void hos_event_set(uint32_t handle, int signaled);
int hos_event_state(uint32_t handle); /* 1 signaled, 0 clear, -1 not an event */
/* Return a new interface from a command: a move handle, or an output object in a domain session. */
void hos_reply_interface(IpcReply *rp, Service *svc);
/* Log the request and stop: for commands a service does not implement yet. */
#if defined(_MSC_VER)
__declspec(noreturn)
#else
__attribute__((noreturn))
#endif
void hos_unimplemented(Cpu *c, const Service *s, const IpcRequest *rq);
void hos_dump_request(const IpcRequest *rq);
/* sm: GetService. Unknown names get a logging service. */
Service *hos_get_service(const char *name);
Service *hos_service_sm(void);

/* Helpers for service implementations. */
#define RP_PUT(rp, type, value) do { type v_ = (value); memcpy((rp)->data + (rp)->data_size, &v_, sizeof v_); \
                                     (rp)->data_size += sizeof v_; } while (0)
#define RQ_GET(rq, type, off) (*(const type *)((rq)->args + (off)))

#endif
