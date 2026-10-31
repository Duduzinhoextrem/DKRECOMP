/* Initial HID service: the applet resource owns the service's 256 KiB input shared memory.
 * Command/layout reference: switchbrew HID services, firmware 4.x. Initially all controllers are disconnected.
 * Activation/configuration and host input are implemented only as their commands are reached by the guest. */
#include "hos.h"

static uint32_t g_memory;

static void applet_resource_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: { /* GetSharedMemoryHandle -> copy handle; the service retains its own writable view. */
        uint32_t handle = hos_copy_shared_memory_handle(g_memory);
        if (!handle) { rp->result = HOS_KERNEL(105); return; }
        rp->copy[rp->ncopy++] = handle;
        return;
    }
    default: hos_unimplemented(c, s, rq);
    }
}

void hos_hid_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service applet = {"hid:IAppletResource", applet_resource_handle, NULL};
    switch (rq->cmd) {
    case 0: /* CreateAppletResource(AppletResourceUserId, PID) -> IAppletResource */
        if (rq->args_size < 8 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        if (!g_memory) g_memory = hos_new_shared_memory(0x40000, 1);
        if (!g_memory) hos_stop(c, "hid: cannot create shared memory (Windows error %lu)", rt_last_os_error());
        hos_reply_interface(rp, &applet);
        return;
    default: hos_unimplemented(c, s, rq);
    }
}
