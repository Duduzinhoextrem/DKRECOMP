/* Horizon services, implemented as the boot log asks for them. Command ids and argument layouts follow
 * switchbrew's service pages for firmware 4.x; each case notes what it returns. A service that is not in
 * g_known is still handed out by sm:GetService, and stops the run with a report at its first command. */
#include "hos.h"

#include <stdlib.h>
#include <string.h>

/* ---- sm: (service manager) ------------------------------------------------------------------------------- */
static void sm_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: /* Initialize(pid) */
        return;
    case 1: { /* GetService(u64 name) -> move handle */
        char name[9] = {0};
        memcpy(name, rq->args, 8);
        hos_log("     sm:GetService(\"%s\")\n", name);
        rp->move[rp->nmove++] = hos_new_session(hos_get_service(name));
        return;
    }
    default:
        hos_unimplemented(c, s, rq);
    }
}

/* ---- lm (log manager) ------------------------------------------------------------------------------------ */
/* ILogger: 0 Log(buffer with a binary log packet), 1 SetDestination(u32). The packet's printable text is
 * copied to the boot log. */
static void lm_logger_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    (void)rp;
    switch (rq->cmd) {
    case 0: {
        const IpcBuf *b = rq->na ? &rq->a[0] : rq->nx ? &rq->x[0] : NULL;
        if (b && b->size) {
            char text[512];
            size_t n = 0;
            const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr;
            for (uint64_t i = 0; i < b->size && n < sizeof text - 1; i++)
                if (p[i] >= 0x20 && p[i] < 0x7F) text[n++] = (char)p[i];
                else if (n && text[n - 1] != ' ') text[n++] = ' ';
            text[n] = 0;
            hos_log("     guest log: %s\n", text);
        }
        return;
    }
    case 1: return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* lm: 0 OpenLogger(u64 pid placeholder, PID) -> ILogger */
static void lm_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service logger = {"lm:ILogger", lm_logger_handle, NULL};
    switch (rq->cmd) {
    case 0: hos_reply_interface(rp, &logger); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- applet framework (appletOE) ------------------------------------------------------------------------- */
/* A sub-interface the application proxy hands out; it logs and stops at its first command until implemented. */
static Service *named_interface(const char *name, ServiceFn fn) {
    Service *s = calloc(1, sizeof *s);
    strncpy(s->name, name, sizeof s->name - 1);
    s->handle = fn;
    return s;
}

#define AM_RESULT(desc) (128u | ((desc) << 9)) /* applet manager module */

/* The applet message queue: the application starts in focus, so the first message is FocusStateChanged (15).
 * Its event is signaled while messages are queued. */
static uint32_t g_msgs[16] = {15};
static int g_nmsgs = 1;
static uint32_t g_msg_event;

/* ICommonStateGetter */
static void am_common_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: /* GetEventHandle -> copy handle */
        if (!g_msg_event) g_msg_event = hos_new_event(g_nmsgs > 0);
        rp->copy[rp->ncopy++] = g_msg_event;
        return;
    case 1: /* ReceiveMessage -> u32, or "no message" */
        if (!g_nmsgs) { rp->result = AM_RESULT(3); return; }
        RP_PUT(rp, uint32_t, g_msgs[0]);
        hos_log("     applet message %u\n", g_msgs[0]);
        memmove(g_msgs, g_msgs + 1, sizeof g_msgs - sizeof *g_msgs);
        if (--g_nmsgs == 0 && g_msg_event) hos_event_set(g_msg_event, 0);
        return;
    case 5: RP_PUT(rp, uint8_t, 1); return;  /* GetOperationMode: 1 = console docked (TV) */
    case 6: RP_PUT(rp, uint32_t, 1); return; /* GetPerformanceMode: 1 = boost (docked) */
    case 8: RP_PUT(rp, uint8_t, 0); return;  /* GetBootMode: normal */
    case 9: RP_PUT(rp, uint8_t, 1); return;  /* GetCurrentFocusState: in focus */
    default: hos_unimplemented(c, s, rq);
    }
}

/* ISelfController: the setters below only record a preference the host does not act on yet. */
static void am_self_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 1: case 2:                 /* LockExit, UnlockExit */
    case 10: case 11: case 12:      /* SetScreenShotPermission, Set{Operation,Performance}ModeChangedNotification */
    case 13: case 14: case 16:      /* SetFocusHandlingMode, SetRestartMessageEnabled, SetOutOfFocusSuspendingEnabled */
    case 19: case 50: case 62:      /* SetAlbumImageOrientation, SetHandlesRequestToDisplay, SetIdleTimeDetectionExtension */
        return;
    case 9: /* GetLibraryAppletLaunchableEvent -> copy handle */
        rp->copy[rp->ncopy++] = hos_new_event(1);
        return;
    case 40: RP_PUT(rp, uint64_t, 1); return; /* CreateManagedDisplayLayer -> layer id */
    default: hos_unimplemented(c, s, rq);
    }
}

/* IWindowController */
static void am_window_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 1: RP_PUT(rp, uint64_t, 0x10); return; /* GetAppletResourceUserId */
    case 10: return;                            /* AcquireForegroundRights */
    default: hos_unimplemented(c, s, rq);
    }
}

/* IAudioController */
static void am_audio_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: return;                                      /* SetExpectedMasterVolume */
    case 1: case 2: RP_PUT(rp, float, 1.0f); return;     /* Get{MainApplet,LibraryApplet}ExpectedMasterVolume */
    default: hos_unimplemented(c, s, rq);
    }
}

/* IApplicationFunctions */
static void am_app_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 1: rp->result = AM_RESULT(2); return;     /* PopLaunchParameter: no parameter */
    case 20: RP_PUT(rp, uint64_t, 0); return;      /* EnsureSaveData(user) -> required size 0 (exists) */
    case 21: {                                     /* GetDesiredLanguage -> language code "en-US" */
        uint64_t code = 0;
        memcpy(&code, "en-US", 5);
        RP_PUT(rp, uint64_t, code);
        return;
    }
    case 22: {                                     /* SetTerminateResult */
        uint32_t result = RQ_GET(rq, uint32_t, 0);
        hos_log("     SetTerminateResult: %#x (%u-%04u)\n", result, 2000 + (result & 0x1ff), result >> 9);
        return;
    }
    case 40: RP_PUT(rp, uint8_t, 1); return;       /* NotifyRunning */
    case 50: RP_PUT(rp, uint64_t, 0x0123456789ABCDEFull); RP_PUT(rp, uint64_t, 0xFEDCBA9876543210ull); return; /* GetPseudoDeviceId */
    case 66: case 67: return;                      /* InitializeGamePlayRecording, SetGamePlayRecordingState */
    default: hos_unimplemented(c, s, rq);
    }
}

/* IApplicationProxy (switchbrew "Applet Manager services"): every command returns an interface. */
static void applet_proxy_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static const struct { uint32_t cmd; const char *name; ServiceFn fn; } subs[] = {
        {0, "ICommonStateGetter", am_common_handle}, {1, "ISelfController", am_self_handle},
        {2, "IWindowController", am_window_handle}, {3, "IAudioController", am_audio_handle},
        {4, "IDisplayController", NULL}, {10, "IProcessWindingController", NULL},
        {11, "ILibraryAppletCreator", NULL}, {20, "IApplicationFunctions", am_app_handle},
        {1000, "IDebugFunctions", NULL}};
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++)
        if (subs[i].cmd == rq->cmd) {
            static Service *made[16];
            if (!made[i]) made[i] = named_interface(subs[i].name, subs[i].fn);
            hos_reply_interface(rp, made[i]);
            return;
        }
    hos_unimplemented(c, s, rq);
}

/* appletOE: 0 OpenApplicationProxy(u64 pid placeholder, PID, copy handle to the process) -> IApplicationProxy */
static void applet_oe_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service proxy = {"IApplicationProxy", applet_proxy_handle, NULL};
    switch (rq->cmd) {
    case 0: hos_reply_interface(rp, &proxy); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- pctl (parental control) ----------------------------------------------------------------------------- */
/* IParentalControlService: no restrictions are configured. */
static void pctl_service_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 1: return;                                   /* Initialize */
    case 1001: return;                                /* CheckFreeCommunicationPermission: allowed */
    case 1006: RP_PUT(rp, uint8_t, 0); return;        /* IsRestrictionTemporaryUnlocked */
    case 1007: RP_PUT(rp, uint8_t, 0); return;        /* IsRestrictedSystemSettingsEntered */
    case 1031: RP_PUT(rp, uint8_t, 0); return;        /* IsRestrictionEnabled */
    default: hos_unimplemented(c, s, rq);
    }
}

/* pctl: 0 CreateService, 1 CreateServiceWithoutInitialize (u64 pid placeholder, PID) -> IParentalControlService */
static void pctl_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service svc = {"IParentalControlService", pctl_service_handle, NULL};
    switch (rq->cmd) {
    case 0: case 1: hos_reply_interface(rp, &svc); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- set (system settings for applications) ------------------------------------------------------------- */
/* The 15 language codes of firmware 4.x, in the system's order (switchbrew "Settings services"). */
static const char *const g_languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko",
                                          "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419"};
#define NLANG (sizeof g_languages / sizeof *g_languages)

static uint64_t language_code(const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s));
    return v;
}

/* Copy the language codes into the command's output buffer (B or C). Returns how many were written. */
static uint32_t write_languages(const IpcRequest *rq) {
    const IpcBuf *b = rq->nb ? &rq->b[0] : rq->nc ? &rq->c[0] : NULL;
    uint32_t n = 0;
    if (!b) return 0;
    for (; n < NLANG && (n + 1) * 8 <= b->size; n++) {
        uint64_t code = language_code(g_languages[n]);
        memcpy((void *)(uintptr_t)(b->addr + 8 * n), &code, 8);
    }
    return n;
}

static void set_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: RP_PUT(rp, uint64_t, language_code("en-US")); return;            /* GetLanguageCode */
    case 1: case 5: RP_PUT(rp, uint32_t, write_languages(rq)); return;       /* GetAvailableLanguageCodes(2) */
    case 2: {                                                                 /* MakeLanguageCode(u32 index) */
        uint32_t i = RQ_GET(rq, uint32_t, 0);
        if (i >= NLANG) { rp->result = 105u | (623u << 9); return; }          /* settings: invalid language */
        RP_PUT(rp, uint64_t, language_code(g_languages[i]));
        return;
    }
    case 3: case 6: RP_PUT(rp, uint32_t, (uint32_t)NLANG); return;           /* GetAvailableLanguageCodeCount(2) */
    case 4: RP_PUT(rp, uint32_t, 1); return;                                  /* GetRegionCode: 1 = USA */
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- apm (performance / power) --------------------------------------------------------------------------- */
/* ISession: 0 SetPerformanceConfiguration(u32 mode, u32 config), 1 GetPerformanceConfiguration(u32 mode) -> u32.
 * The configuration is only remembered; the host does not change clocks. */
static uint32_t g_perf_config[2] = {0x00010000, 0x92220007}; /* handheld, docked defaults (switchbrew "PTM services") */
static void apm_session_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    uint32_t mode = rq->args_size >= 4 ? RQ_GET(rq, uint32_t, 0) : 0;
    switch (rq->cmd) {
    case 0: if (mode < 2) g_perf_config[mode] = RQ_GET(rq, uint32_t, 4); return;
    case 1: RP_PUT(rp, uint32_t, g_perf_config[mode < 2 ? mode : 0]); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* apm: 0 OpenSession -> ISession, 1 GetPerformanceMode -> u32 (1 = boost, docked) */
static void apm_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service session = {"apm:ISession", apm_session_handle, NULL};
    switch (rq->cmd) {
    case 0: hos_reply_interface(rp, &session); return;
    case 1: RP_PUT(rp, uint32_t, 1); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- acc:u0 (accounts) ------------------------------------------------------------------------------------- */
/* One local user. ProfileBase (0x38 bytes): uid (16), last edit timestamp (8), nickname (0x21, NUL padded).
 * Layouts from switchbrew "Account services". */
static const uint64_t g_uid[2] = {0x0123456789ABCDEFull, 0x0FEDCBA987654321ull};

static void put_profile_base(IpcReply *rp) {
    uint8_t base[0x38] = {0};
    memcpy(base, g_uid, 16);
    memcpy(base + 0x18, "DKRecomp", 8);
    memcpy(rp->data + rp->data_size, base, sizeof base);
    rp->data_size += sizeof base;
}

static void write_uid_list(const IpcRequest *rq) {
    const IpcBuf *b = rq->nc ? &rq->c[0] : rq->nb ? &rq->b[0] : NULL;
    if (!b || b->size < 16) return;
    memset((void *)(uintptr_t)b->addr, 0, (size_t)b->size);
    memcpy((void *)(uintptr_t)b->addr, g_uid, 16);
}

/* IProfile: 0 Get(user data buffer) -> ProfileBase, 1 GetBase -> ProfileBase, 10 GetImageSize, 11 LoadImage */
static void acc_profile_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0:
        if (rq->nc) memset((void *)(uintptr_t)rq->c[0].addr, 0, (size_t)rq->c[0].size);
        put_profile_base(rp);
        return;
    case 1: put_profile_base(rp); return;
    case 10: RP_PUT(rp, uint32_t, 0); return; /* no profile image */
    case 11: RP_PUT(rp, uint32_t, 0); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* IManagerForApplication: 0 CheckAvailability, 1 GetAccountId -> u64 */
static void acc_manager_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: return;
    case 1: RP_PUT(rp, uint64_t, 0x1122334455667788ull); return;
    default: hos_unimplemented(c, s, rq);
    }
}

static void acc_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service profile = {"acc:IProfile", acc_profile_handle, NULL};
    static Service manager = {"acc:IManagerForApplication", acc_manager_handle, NULL};
    switch (rq->cmd) {
    case 0: RP_PUT(rp, int32_t, 1); return;                              /* GetUserCount */
    case 1: RP_PUT(rp, uint8_t, !memcmp(rq->args, g_uid, 16)); return;   /* GetUserExistence(uid) */
    case 2: case 3: write_uid_list(rq); return;                           /* ListAllUsers, ListOpenUsers */
    case 4: RP_PUT(rp, uint64_t, g_uid[0]); RP_PUT(rp, uint64_t, g_uid[1]); return; /* GetLastOpenedUser */
    case 5: hos_reply_interface(rp, &profile); return;                    /* GetProfile(uid) */
    case 100: return;                                                     /* InitializeApplicationInfo(PID) */
    case 101: hos_reply_interface(rp, &manager); return;                  /* GetBaasAccountManagerForApplication */
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- time:u ------------------------------------------------------------------------------------------------ */
/* Clocks come from the host, in UTC. Layouts from switchbrew "Glue services" (time):
 *   SteadyClockTimePoint: u64 seconds, u128 clock source id (0x18 bytes)
 *   SystemClockContext: u64 offset, SteadyClockTimePoint (0x20 bytes)
 *   CalendarTime: u16 year, u8 month, day, hour, minute, second, pad (8 bytes)
 *   CalendarAdditionalInfo: u32 day of week, u32 day of year, char tz name[8], u32 is dst, s32 utc offset */
#include <time.h>

static const uint64_t g_clock_source[2] = {0x444B526563436C6Bull, 0x0000000000000001ull};

static void put_steady_point(IpcReply *rp) {
    RP_PUT(rp, uint64_t, rt_cntvct() / rt_cntfrq());
    RP_PUT(rp, uint64_t, g_clock_source[0]);
    RP_PUT(rp, uint64_t, g_clock_source[1]);
}

/* ISystemClock: 0 GetCurrentTime -> u64 POSIX seconds, 2 GetSystemClockContext -> SystemClockContext */
static void time_system_clock_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: RP_PUT(rp, uint64_t, (uint64_t)time(NULL)); return;
    case 2: RP_PUT(rp, uint64_t, (uint64_t)time(NULL) - rt_cntvct() / rt_cntfrq()); put_steady_point(rp); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ISteadyClock: 0 GetCurrentTimePoint -> SteadyClockTimePoint */
static void time_steady_clock_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: put_steady_point(rp); return;
    default: hos_unimplemented(c, s, rq);
    }
}

static void put_calendar(IpcReply *rp, uint64_t posix) {
    time_t t = (time_t)posix;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    RP_PUT(rp, uint16_t, (uint16_t)(tmv.tm_year + 1900));
    RP_PUT(rp, uint8_t, (uint8_t)(tmv.tm_mon + 1));
    RP_PUT(rp, uint8_t, (uint8_t)tmv.tm_mday);
    RP_PUT(rp, uint8_t, (uint8_t)tmv.tm_hour);
    RP_PUT(rp, uint8_t, (uint8_t)tmv.tm_min);
    RP_PUT(rp, uint8_t, (uint8_t)tmv.tm_sec);
    RP_PUT(rp, uint8_t, 0);
    RP_PUT(rp, uint32_t, (uint32_t)tmv.tm_wday);
    RP_PUT(rp, uint32_t, (uint32_t)tmv.tm_yday);
    char tz[8] = "UTC";
    memcpy(rp->data + rp->data_size, tz, 8);
    rp->data_size += 8;
    RP_PUT(rp, uint32_t, 0);
    RP_PUT(rp, int32_t, 0);
}

/* ITimeZoneService: 0 GetDeviceLocationName -> 0x24 bytes, 100 ToCalendarTime(u64, rule buffer),
 * 101 ToCalendarTimeWithMyRule(u64) -> CalendarTime + CalendarAdditionalInfo */
static void time_zone_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: {
        char name[0x24] = "UTC";
        memcpy(rp->data + rp->data_size, name, sizeof name);
        rp->data_size += sizeof name;
        return;
    }
    case 100: case 101: put_calendar(rp, RQ_GET(rq, uint64_t, 0)); return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* time:u: 0 user clock, 1 network clock, 2 steady clock, 3 time zone service, 4 local clock */
static void time_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service sys = {"time:ISystemClock", time_system_clock_handle, NULL};
    static Service steady = {"time:ISteadyClock", time_steady_clock_handle, NULL};
    static Service zone = {"time:ITimeZoneService", time_zone_handle, NULL};
    switch (rq->cmd) {
    case 0: case 1: case 4: hos_reply_interface(rp, &sys); return;
    case 2: hos_reply_interface(rp, &steady); return;
    case 3: hos_reply_interface(rp, &zone); return;
    case 300: /* CalculateMonotonicSystemClockBaseTimePoint(SystemClockContext) -> s64: system time at steady 0 */
        RP_PUT(rp, int64_t, RQ_GET(rq, int64_t, 0));
        return;
    default: hos_unimplemented(c, s, rq);
    }
}

/* ---- registry ----------------------------------------------------------------------------------------------- */
typedef struct Known {
    const char *name;
    ServiceFn fn;
} Known;
static const Known g_known[] = {
    {"sm:", sm_handle},
    {"lm", lm_handle},
    {"appletOE", applet_oe_handle},
    {"pctl", pctl_handle},
    {"pctl:s", pctl_handle},
    {"pctl:r", pctl_handle},
    {"pctl:a", pctl_handle},
    {"set", set_handle},
    {"apm", apm_handle},
    {"acc:u0", acc_handle},
    {"time:u", time_handle},
    {"time:s", time_handle},
    {"fsp-srv", hos_fsp_handle},
};

Service *hos_get_service(const char *name) {
    Service *s = calloc(1, sizeof *s);
    strncpy(s->name, name, sizeof s->name - 1);
    for (size_t i = 0; i < sizeof g_known / sizeof *g_known; i++)
        if (!strcmp(g_known[i].name, name)) s->handle = g_known[i].fn;
    return s;
}

Service *hos_service_sm(void) {
    static Service *sm;
    if (!sm) sm = hos_get_service("sm:");
    return sm;
}
