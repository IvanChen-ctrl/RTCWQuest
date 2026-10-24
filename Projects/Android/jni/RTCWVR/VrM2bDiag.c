/*
===========================================================================
VrM2bDiag.c -- cinematic ("RTCWCinematic") and menu-pointer ("RTCWPointer")
diagnostics.  State changes only; never per-frame values (except the
documented 2 s / 250 ms pointer sample while a menu is shown).

The formatting and rate limiting also build on a host machine with
-DVRM2B_HOST_TEST; see tests/vr_m2b.  Event list: docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md.
===========================================================================
*/

#include "VrM2bDiag.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { CH_CIN = 0, CH_PTR = 1, CH_COUNT };

#ifdef VRM2B_HOST_TEST

int  VrM2bDiagHost_Now(void);
int  VrM2bDiagHost_CvarValue(int channel);
void VrM2bDiagHost_Sink(int channel, int priority, const char *line);

#define DIAG_NOW()              VrM2bDiagHost_Now()
#define DIAG_LEVEL(ch)          VrM2bDiagHost_CvarValue(ch)
#define DIAG_EMIT(ch, prio, l)  VrM2bDiagHost_Sink(ch, prio, l)
#define PRIO_INFO  4
#define PRIO_ERROR 6

#else

#include <android/log.h>
#include <src/game/q_shared.h>
#include <src/qcommon/qcommon.h>
#include <src/client/client.h>
#include "VrCinematicLogic.h"

static cvar_t *diagCvar[CH_COUNT];

static int DiagLevel(int ch)
{
    if (!diagCvar[ch]) {
        diagCvar[ch] = Cvar_Get(ch == CH_CIN ? "vr_cin_diag" : "vr_pointer_diag", "1", 0);
    }
    return diagCvar[ch] ? diagCvar[ch]->integer : 1;
}

#define DIAG_NOW()              Sys_Milliseconds()
#define DIAG_LEVEL(ch)          DiagLevel(ch)
#define DIAG_EMIT(ch, prio, l)  __android_log_print(prio, (ch) == CH_CIN ? VRCIN_DIAG_TAG : VRPTR_DIAG_TAG, "%s", (l))
#define PRIO_INFO  ANDROID_LOG_INFO
#define PRIO_ERROR ANDROID_LOG_ERROR

#endif

/* ---- per-channel token bucket ---- */
#define BUCKET_CAPACITY 30
#define BUCKET_REFILL_PER_SEC 10

typedef struct {
    int tokens;
    int lastRefill;
    int started;
    int droppedPending;
} bucket_t;

static bucket_t buckets[CH_COUNT] = { { BUCKET_CAPACITY, 0, 0, 0 }, { BUCKET_CAPACITY, 0, 0, 0 } };

/* Pointer event state (change detection). */
static int ptrPoseLast[3] = { -1, -1, -1 };
static int ptrHandLast = -2;
static int ptrSampleLastMs;
static int ptrSampleStarted;
static int ptrOffAxis;
static int ptrRejected;

void VrM2bDiag_ResetForTest(void)
{
    int i;

    ptrPoseLast[0] = ptrPoseLast[1] = ptrPoseLast[2] = -1;
    ptrHandLast = -2;
    ptrSampleLastMs = ptrSampleStarted = ptrOffAxis = ptrRejected = 0;

    for (i = 0; i < CH_COUNT; i++) {
        buckets[i].tokens = BUCKET_CAPACITY;
        buckets[i].lastRefill = 0;
        buckets[i].started = 0;
        buckets[i].droppedPending = 0;
    }
}

static void Refill(bucket_t *b, int now)
{
    int add;

    if (!b->started) {
        b->started = 1;
        b->lastRefill = now;
        return;
    }
    add = ((now - b->lastRefill) * BUCKET_REFILL_PER_SEC) / 1000;
    if (add > 0) {
        b->tokens += add;
        if (b->tokens > BUCKET_CAPACITY) {
            b->tokens = BUCKET_CAPACITY;
        }
        b->lastRefill += (add * 1000) / BUCKET_REFILL_PER_SEC;
    }
}

static void EmitLine(int ch, int prio, int now, const char *ev, const char *body)
{
    char line[320];

    snprintf(line, sizeof(line), "v1 t=%d ev=%s%s%s", now, ev, body[0] ? " " : "", body);
    DIAG_EMIT(ch, prio, line);
}

static void EventV(int ch, int force, const char *ev, const char *fmt, va_list ap)
{
    char body[240];
    bucket_t *b = &buckets[ch];
    int now = DIAG_NOW();

    if (!force && DIAG_LEVEL(ch) == 0) {
        return;
    }
    Refill(b, now);

    if (!force) {
        if (b->tokens <= 0) {
            b->droppedPending++;
            return;
        }
        if (b->droppedPending > 0) {
            char dropBody[32];

            if (b->tokens < 2) {
                b->droppedPending++;
                return;
            }
            b->tokens--;
            snprintf(dropBody, sizeof(dropBody), "n=%d", b->droppedPending);
            EmitLine(ch, PRIO_INFO, now, "drop", dropBody);
            b->droppedPending = 0;
        }
        b->tokens--;
    }
    vsnprintf(body, sizeof(body), fmt, ap);
    EmitLine(ch, force ? PRIO_ERROR : PRIO_INFO, now, ev, body);
}

void VrCinDiag_Event(const char *ev, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    EventV(CH_CIN, 0, ev, fmt, ap);
    va_end(ap);
}

void VrPtrDiag_Event(const char *ev, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    EventV(CH_PTR, 0, ev, fmt, ap);
    va_end(ap);
}

/* ===================== pointer events ===================== */

static int ptrLevel(void)
{
    return DIAG_LEVEL(CH_PTR);
}

void VrPtrDiag_Snap(const char *reason, float hmdYaw, float quadYaw, float x, float y, float z, int screen)
{
    VrPtrDiag_Event("snap", "reason=%s hmdYaw=%.1f quadYaw=%.1f pos=%.2f,%.2f,%.2f screen=%d",
                    reason, hmdYaw, quadYaw, x, y, z, screen);
}

void VrPtrDiag_Pose(const char *who, int usable, unsigned flags)
{
    int idx = who[0] == 'H' ? 0 : (who[0] == 'L' ? 1 : 2);

    if (ptrPoseLast[idx] == usable) {
        return;
    }
    ptrPoseLast[idx] = usable;
    VrPtrDiag_Event("pose", "who=%s usable=%d flags=0x%x", who, usable, flags);
}

void VrPtrDiag_Recenter(float hmdYawBefore)
{
    VrPtrDiag_Event("recenter", "hmdYawBefore=%.1f", hmdYawBefore);
}

void VrPtrDiag_MenuHand(int hand)
{
    if (hand == ptrHandLast) {
        return;
    }
    ptrHandLast = hand;
    VrPtrDiag_Event("hand", "hand=%c", hand == 0 ? 'L' : (hand == 1 ? 'R' : '?'));
}

void VrPtrDiag_Sample(float ctrlYaw, float ctrlPitch, float refYaw, float hmdYaw, float u, float v)
{
    int level = ptrLevel();
    int now;
    float dyaw = hmdYaw - refYaw;
    int off;

    if (level == 0) {
        return;
    }
    while (dyaw > 180.0f) {
        dyaw -= 360.0f;
    }
    while (dyaw < -180.0f) {
        dyaw += 360.0f;
    }
    /* The menu is not in front of the user: state change only. */
    off = fabsf(dyaw) > 45.0f;
    if (off != ptrOffAxis) {
        ptrOffAxis = off;
        VrPtrDiag_Event("offaxis", "state=%d hmdYaw=%.1f refYaw=%.1f dyaw=%.1f", off, hmdYaw, refYaw, dyaw);
    }

    now = DIAG_NOW();
    if (ptrSampleStarted && now - ptrSampleLastMs < (level >= 2 ? 250 : 2000)) {
        return;
    }
    ptrSampleStarted = 1;
    ptrSampleLastMs = now;
    VrPtrDiag_Event("ptr", "ctrlYaw=%.1f ctrlPitch=%.1f refYaw=%.1f hmdYaw=%.1f u=%.3f v=%.3f ui=%d,%d",
                    ctrlYaw, ctrlPitch, refYaw, hmdYaw, u, v, (int)(u * 640.0f), (int)(v * 480.0f));
}

void VrPtrDiag_Rejected(float ctrlYaw, float ctrlPitch, float refYaw)
{
    if (ptrRejected) {
        return;
    }
    ptrRejected = 1;      /* once per process: a flapping input must not flood */
    VrPtrDiag_Event("reject", "reason=non_finite ctrlYaw=%.1f ctrlPitch=%.1f refYaw=%.1f",
                    ctrlYaw, ctrlPitch, refYaw);
}

/* ===================== cinematic events (engine only) ===================== */
#ifndef VRM2B_HOST_TEST

static const char *StateName(int s)
{
    switch (s) {
    case CA_UNINITIALIZED: return "uninit";
    case CA_DISCONNECTED:  return "disconnected";
    case CA_AUTHORIZING:   return "authorizing";
    case CA_CONNECTING:    return "connecting";
    case CA_CHALLENGING:   return "challenging";
    case CA_CONNECTED:     return "connected";
    case CA_LOADING:       return "loading";
    case CA_PRIMED:        return "primed";
    case CA_ACTIVE:        return "active";
    case CA_CINEMATIC:     return "cinematic";
    default:               return "?";
    }
}

static const char *MapName(void)
{
    static char name[40];
    const char *m = Cvar_VariableString("mapname");
    int i;

    if (!m || !m[0]) {
        return "none";
    }
    Q_strncpyz(name, m, sizeof(name));
    for (i = 0; name[i]; i++) {
        if (name[i] == ' ') {
            name[i] = '_';
        }
    }
    return name;
}

static int ClientPaused(void)
{
    return VrCin_ClientPaused(com_sv_running ? com_sv_running->integer : 0,
                              sv_paused ? sv_paused->integer : 0,
                              cl_paused ? cl_paused->integer : 0);
}

/* Counters for the current camera episode, reported by cam_stop. */
static int epInterrupts, epSuppressed, epSuperseded, epMaxOutstanding;
static int supersedeRun;        /* consecutive dropped commands in the current run */
static int totalSuperseded;
static int queueLevel;

static void EndSupersedeRun(const char *why)
{
    if (supersedeRun > 0) {
        VrCinDiag_Event("supersede_end", "dropped=%d why=%s total=%d", supersedeRun, why, totalSuperseded);
        supersedeRun = 0;
    }
}

void VrCinDiag_Frame(void)
{
    static int lastState = -1, lastCam = -1, lastPaused = -1, lastCatch = -1;
    static int lastHb;
    int paused = ClientPaused();
    int now;

    if ((int)cls.state != lastState || cl.cameraMode != lastCam || paused != lastPaused ||
        cls.keyCatchers != lastCatch) {
        lastState = cls.state;
        lastCam = cl.cameraMode;
        lastPaused = paused;
        lastCatch = cls.keyCatchers;
        VrCinDiag_Event("state", "cls=%s cam=%d paused=%d catchers=0x%x map=%s seq=%d ack=%d",
                        StateName(cls.state), cl.cameraMode, paused, cls.keyCatchers, MapName(),
                        clc.reliableSequence, clc.reliableAcknowledge);
    }

    if (cl.cameraMode) {
        now = Sys_Milliseconds();
        if (now - lastHb >= 2000) {
            lastHb = now;
            VrCinDiag_Event("cam_hb", "seq=%d ack=%d outstanding=%d paused=%d superseded_total=%d",
                            clc.reliableSequence, clc.reliableAcknowledge,
                            VrCin_Outstanding(clc.reliableSequence, clc.reliableAcknowledge),
                            paused, totalSuperseded);
        }
    }
    if (!paused) {
        EndSupersedeRun("resumed");
    }
}

void VrCinDiag_CameraStart(void)
{
    epInterrupts = epSuppressed = epSuperseded = epMaxOutstanding = 0;
    VrCinDiag_Event("cam_start", "map=%s cls=%s seq=%d ack=%d", MapName(), StateName(cls.state),
                    clc.reliableSequence, clc.reliableAcknowledge);
}

void VrCinDiag_CameraStop(void)
{
    EndSupersedeRun("camera_stop");
    VrCinDiag_Event("cam_stop", "map=%s interrupts=%d suppressed=%d superseded=%d max_outstanding=%d seq=%d ack=%d",
                    MapName(), epInterrupts, epSuppressed, epSuperseded, epMaxOutstanding,
                    clc.reliableSequence, clc.reliableAcknowledge);
}

void VrCinDiag_Interrupt(int key, int sent)
{
    if (sent) {
        epInterrupts++;
    } else {
        epSuppressed++;
    }
    VrCinDiag_Event("interrupt", "key=%d result=%s seq=%d ack=%d outstanding=%d paused=%d",
                    key, sent ? "sent" : "suppressed_unacked", clc.reliableSequence, clc.reliableAcknowledge,
                    VrCin_Outstanding(clc.reliableSequence, clc.reliableAcknowledge), ClientPaused());
}

void VrCinDiag_Superseded(const char *cmd, int paused)
{
    (void)cmd;
    epSuperseded++;
    totalSuperseded++;
    if (supersedeRun++ == 0) {
        VrCinDiag_Event("supersede_start", "kind=setCameraOrigin paused=%d seq=%d ack=%d outstanding=%d",
                        paused, clc.reliableSequence, clc.reliableAcknowledge,
                        VrCin_Outstanding(clc.reliableSequence, clc.reliableAcknowledge));
    }
}

void VrCinDiag_QueueDepth(int outstanding)
{
    static const int levels[] = { 32, 64, 128, 192, 240 };
    int i;
    int level = 0;

    if (outstanding > epMaxOutstanding) {
        epMaxOutstanding = outstanding;
    }
    for (i = 0; i < (int)(sizeof(levels) / sizeof(levels[0])); i++) {
        if (outstanding >= levels[i]) {
            level = i + 1;
        }
    }
    if (level > queueLevel) {
        queueLevel = level;
        VrCinDiag_Event("queue", "outstanding=%d level=%d seq=%d ack=%d paused=%d cam=%d cls=%s",
                        outstanding, levels[level - 1], clc.reliableSequence, clc.reliableAcknowledge,
                        ClientPaused(), cl.cameraMode, StateName(cls.state));
    } else if (outstanding < 16) {
        queueLevel = 0;
    }
}

void VrCinDiag_PauseMenuRefused(const char *what)
{
    VrCinDiag_Event("pause_refused", "what=%s cam=%d cls=%s", what, cl.cameraMode, StateName(cls.state));
}

void VrCinDiag_Overflow(const char *kinds)
{
    char body[240];
    char line[320];

    /* Always logged (priority error), independent of the cvar and the limiter. */
    snprintf(body, sizeof(body),
             "seq=%d ack=%d outstanding=%d cls=%s cam=%d paused=%d catchers=0x%x map=%s packet_age_ms=%d kinds=%s",
             clc.reliableSequence, clc.reliableAcknowledge,
             VrCin_Outstanding(clc.reliableSequence, clc.reliableAcknowledge), StateName(cls.state),
             cl.cameraMode, ClientPaused(), cls.keyCatchers, MapName(),
             cls.realtime - clc.lastPacketTime, kinds ? kinds : "?");
    snprintf(line, sizeof(line), "v1 t=%d ev=overflow %s", Sys_Milliseconds(), body);
    DIAG_EMIT(CH_CIN, PRIO_ERROR, line);
}

#endif /* !VRM2B_HOST_TEST */
