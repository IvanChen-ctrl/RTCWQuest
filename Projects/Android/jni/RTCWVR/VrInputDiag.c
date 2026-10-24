/*
===========================================================================
VrInputDiag.c -- optional, rate-limited controller input diagnostics.

Logs state changes only (never per-frame values) to logcat, tag "RTCWInput".
Format and event list: docs/INPUT_DIAGNOSTICS.md.

The pure logic (change detection, rate limiting, formatting) also builds on a
host machine with -DVRINPUT_HOST_TEST; see tests/vr_input.
===========================================================================
*/

#include "VrInputDiag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef VRINPUT_HOST_TEST

/* Host test shims: the test provides time, the cvar value and the log sink. */
int  VrInputDiagHost_Now(void);
int  VrInputDiagHost_CvarValue(void);
void VrInputDiagHost_Sink(const char *line);

#define DIAG_NOW()          VrInputDiagHost_Now()
#define DIAG_ENABLED()      (VrInputDiagHost_CvarValue() != 0)
#define DIAG_EMIT(line)     VrInputDiagHost_Sink(line)

#define K_ESCAPE    27
#define K_SPACE     32
#define K_MOUSE1    178
#define KEYCATCH_CONSOLE 0x0001
#define KEYCATCH_UI      0x0002
#define KEYCATCH_CGAME   0x0008

#else

#include <android/log.h>
#include <src/game/q_shared.h>
#include <src/qcommon/qcommon.h>
#include <src/client/client.h>

static cvar_t *vr_input_diag_cvar;

static int DiagCvarEnabled(void)
{
    if (!vr_input_diag_cvar) {
        vr_input_diag_cvar = Cvar_Get("vr_input_diag", "1", 0);
    }
    return vr_input_diag_cvar ? vr_input_diag_cvar->integer != 0 : 1;
}

#define DIAG_NOW()          Sys_Milliseconds()
#define DIAG_ENABLED()      DiagCvarEnabled()
#define DIAG_EMIT(line)     __android_log_print(ANDROID_LOG_INFO, VRDIAG_TAG, "%s", (line))

#endif

/* ---- rate limiter: token bucket, state-change events are rare so this is a
 * safety net against a flapping input, not part of normal operation. ---- */
#define DIAG_BUCKET_CAPACITY    40
#define DIAG_REFILL_PER_SEC     20
#define DIAG_HEARTBEAT_MS       5000

static int bucketTokens = DIAG_BUCKET_CAPACITY;
static int bucketLastRefill;
static int bucketStarted;
static int droppedPending;
static int droppedTotal;

static void RefillBucket(int now)
{
    int add;

    if (!bucketStarted) {
        bucketStarted = 1;
        bucketLastRefill = now;
        return;
    }
    add = ((now - bucketLastRefill) * DIAG_REFILL_PER_SEC) / 1000;
    if (add > 0) {
        bucketTokens += add;
        if (bucketTokens > DIAG_BUCKET_CAPACITY) {
            bucketTokens = DIAG_BUCKET_CAPACITY;
        }
        bucketLastRefill += (add * 1000) / DIAG_REFILL_PER_SEC;
    }
}

static void EmitLine(int now, const char *ev, const char *body)
{
    char line[320];

    snprintf(line, sizeof(line), "v1 t=%d ev=%s%s%s", now, ev, body[0] ? " " : "", body);
    DIAG_EMIT(line);
}

static void EmitV(const char *ev, const char *fmt, va_list ap)
{
    char body[240];
    int now;

    if (!DIAG_ENABLED()) {
        return;
    }

    now = DIAG_NOW();
    RefillBucket(now);

    if (bucketTokens <= 0) {
        droppedPending++;
        droppedTotal++;
        return;
    }
    if (droppedPending > 0) {
        char dropBody[32];

        if (bucketTokens < 2) {
            droppedPending++;
            droppedTotal++;
            return;
        }
        bucketTokens--;
        snprintf(dropBody, sizeof(dropBody), "n=%d", droppedPending);
        EmitLine(now, "drop", dropBody);
        droppedPending = 0;
    }
    bucketTokens--;

    vsnprintf(body, sizeof(body), fmt, ap);
    EmitLine(now, ev, body);
}

int VrInputDiag_Enabled(void)
{
    return DIAG_ENABLED();
}

void VrInputDiag_Event(const char *ev, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    EmitV(ev, fmt, ap);
    va_end(ap);
}

static char HandChar(int hand)
{
    return hand == 0 ? 'L' : (hand == 1 ? 'R' : '?');
}

static const char *ShortProfile(const char *path)
{
    static const char prefix[] = "/interaction_profiles/";

    if (path[0] == '\0') {
        return "none";
    }
    if (strncmp(path, prefix, sizeof(prefix) - 1) == 0) {
        return path + sizeof(prefix) - 1;
    }
    return path;
}

static const char *KeyLabel(int key)
{
    if (key == K_MOUSE1) {
        return "MOUSE1";
    }
    if (key == K_ESCAPE) {
        return "ESCAPE";
    }
    if (key == K_SPACE) {
        return "SPACE";
    }
    return "other";
}

/* ---- setup / result events ---- */

static int stateMouse1Generated;
static int stateMouse1Delivered;

void VrInputDiag_Extension(const char *name, int enabled)
{
    VrInputDiag_Event("ext", "name=%s enabled=%d", name, enabled);
}

void VrInputDiag_SuggestResult(const char *profile, int result, const char *resultName, int count, int retry)
{
    VrInputDiag_Event("suggest", "profile=%s result=%d name=%s bindings=%d retry=%d",
                      ShortProfile(profile), result, resultName, count, retry);
}

void VrInputDiag_AttachResult(int result, const char *resultName)
{
    VrInputDiag_Event("attach", "result=%d name=%s", result, resultName);
}

static int syncReported;
static int syncLast;

void VrInputDiag_SyncResult(int result, const char *resultName)
{
    if (syncReported && syncLast == result) {
        return;
    }
    syncReported = 1;
    syncLast = result;
    VrInputDiag_Event("sync", "result=%d name=%s", result, resultName);
}

static char profileCurrent[2][96];

void VrInputDiag_Profile(int hand, const char *path)
{
    if (hand < 0 || hand > 1) {
        return;
    }
    if (strcmp(profileCurrent[hand], path) == 0) {
        return;
    }
    snprintf(profileCurrent[hand], sizeof(profileCurrent[hand]), "%s", path);
    VrInputDiag_Event("profile", "hand=%c path=%s", HandChar(hand), path[0] ? path : "none");
}

static const char *errLastAction[2];
static int errLastResult[2];

void VrInputDiag_ActionError(int hand, const char *action, int result, const char *resultName)
{
    if (hand < 0 || hand > 1) {
        return;
    }
    if (errLastAction[hand] == action && errLastResult[hand] == result) {
        return;
    }
    errLastAction[hand] = action;
    errLastResult[hand] = result;
    VrInputDiag_Event("actionerr", "hand=%c action=%s result=%d name=%s",
                      HandChar(hand), action, result, resultName);
}

/* ---- per-frame sampling: logs on change only ---- */

typedef struct {
    int      init;
    unsigned activeMask;
    int      down[VRDIAG_ACT_COUNT];
    float    triggerPeak;
} handDiagState_t;

static handDiagState_t handState[2];

static const char *ActionName(int hand, int act)
{
    switch (act) {
    case VRDIAG_ACT_TRIGGER:    return "trigger";
    case VRDIAG_ACT_GRIP:       return "grip";
    case VRDIAG_ACT_FACE1:      return hand == 0 ? "X" : "A";
    case VRDIAG_ACT_FACE2:      return hand == 0 ? "Y" : "B";
    case VRDIAG_ACT_MENU:       return "menu";
    case VRDIAG_ACT_STICKCLICK: return "stickclick";
    case VRDIAG_ACT_SELECT:     return "select";
    case VRDIAG_ACT_STICK:      return "stick";
    default:                    return "?";
    }
}

static int ActionIsDown(int act, float value)
{
    if (act == VRDIAG_ACT_TRIGGER || act == VRDIAG_ACT_GRIP) {
        return value > 0.5f;
    }
    return value != 0.0f;
}

void VrInputDiag_HandSample(int hand, const vrDiagHandSample_t *s)
{
    handDiagState_t *h;
    unsigned mask = 0;
    int i;

    if (hand < 0 || hand > 1 || !DIAG_ENABLED()) {
        return;
    }
    h = &handState[hand];

    for (i = 0; i < VRDIAG_ACT_COUNT; i++) {
        if (s->isActive[i]) {
            mask |= 1u << i;
        }
    }

    if (!h->init || mask != h->activeMask) {
        char names[120];
        size_t used = 0;

        names[0] = '\0';
        for (i = 0; i < VRDIAG_ACT_COUNT; i++) {
            if (mask & (1u << i)) {
                used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%s",
                                         used ? "|" : "", ActionName(hand, i));
                if (used >= sizeof(names)) {
                    used = sizeof(names) - 1;
                    break;
                }
            }
        }
        VrInputDiag_Event("active", "hand=%c mask=0x%02x actions=%s",
                          HandChar(hand), mask, used ? names : "none");
        h->activeMask = mask;
    }

    for (i = 0; i < VRDIAG_ACT_STICK; i++) {
        int down = ActionIsDown(i, s->value[i]);

        if (h->init && down != h->down[i]) {
            VrInputDiag_Event("btn", "hand=%c name=%s down=%d val=%.2f active=%d buttons=0x%08x",
                              HandChar(hand), ActionName(hand, i), down, s->value[i],
                              s->isActive[i], (unsigned)s->buttons);
        }
        h->down[i] = down;
    }

    /* A trigger that moves but never crosses the press threshold is a binding
     * or scaling problem worth reporting once per pull. */
    {
        float v = s->value[VRDIAG_ACT_TRIGGER];

        if (v > h->triggerPeak) {
            h->triggerPeak = v;
        }
        if (v <= 0.02f) {
            if (h->triggerPeak > 0.05f && h->triggerPeak <= 0.5f) {
                VrInputDiag_Event("trigpeak", "hand=%c max=%.2f note=below_press_threshold",
                                  HandChar(hand), h->triggerPeak);
            }
            h->triggerPeak = 0.0f;
        }
    }

    h->init = 1;
}

/* ---- menu hand selection ---- */

static int menuReported;
static int menuLastCursor;
static int menuLastRight;
static int menuLastControls;
static int menuLastScreen;

void VrInputDiag_MenuHand(int cursorHand, int menuRightHanded, int controlsLeftHanded, int screen)
{
    if (menuReported && menuLastCursor == cursorHand && menuLastRight == menuRightHanded &&
        menuLastControls == controlsLeftHanded && menuLastScreen == screen) {
        return;
    }
    menuReported = 1;
    menuLastCursor = cursorHand;
    menuLastRight = menuRightHanded;
    menuLastControls = controlsLeftHanded;
    menuLastScreen = screen;
    VrInputDiag_Event("menuhand", "cursor=%c menu_right_handed=%d controls_left=%d screen=%d",
                      HandChar(cursorHand), menuRightHanded, controlsLeftHanded, screen);
}

/* ---- key events ---- */

void VrInputDiag_KeyGenerated(int hand, int key, int down)
{
    if (key == K_MOUSE1 && down) {
        stateMouse1Generated++;
    }
    VrInputDiag_Event("key", "stage=gen hand=%c name=%s code=%d down=%d",
                      HandChar(hand), KeyLabel(key), key, down);
}

void VrInputDiag_KeyDelivered(int key, int down, int keyCatchers)
{
    const char *route;

    if (key != K_MOUSE1 && key != K_ESCAPE && key != K_SPACE) {
        return;
    }
    if (key == K_MOUSE1 && down) {
        stateMouse1Delivered++;
    }

    if (keyCatchers & KEYCATCH_CONSOLE) {
        route = "console";
    } else if (keyCatchers & KEYCATCH_UI) {
        route = "ui";
    } else if (keyCatchers & KEYCATCH_CGAME) {
        route = "cgame";
    } else {
        route = "game";
    }
    VrInputDiag_Event("key", "stage=deliver name=%s code=%d down=%d catchers=0x%x route=%s",
                      KeyLabel(key), key, down, keyCatchers, route);
}

/* ---- heartbeat ---- */

static int hbFrames;
static int hbLast;
static int hbStarted;

void VrInputDiag_Heartbeat(void)
{
    int now;

    if (!DIAG_ENABLED()) {
        return;
    }
    hbFrames++;
    now = DIAG_NOW();
    if (!hbStarted) {
        hbStarted = 1;
        hbLast = now;
        return;
    }
    if (now - hbLast < DIAG_HEARTBEAT_MS) {
        return;
    }
    hbLast = now;
    VrInputDiag_Event("hb", "frames=%d profileL=%s profileR=%s sync=%d mouse1_gen=%d mouse1_deliver=%d dropped_total=%d",
                      hbFrames, ShortProfile(profileCurrent[0]), ShortProfile(profileCurrent[1]),
                      syncReported ? syncLast : 0, stateMouse1Generated, stateMouse1Delivered, droppedTotal);
    hbFrames = 0;
}

#ifdef VRINPUT_HOST_TEST
/* Test-only: restore the module to its power-on state between scenarios. */
void VrInputDiag_ResetForTest(void)
{
    bucketTokens = DIAG_BUCKET_CAPACITY;
    bucketLastRefill = 0;
    bucketStarted = 0;
    droppedPending = 0;
    droppedTotal = 0;
    stateMouse1Generated = 0;
    stateMouse1Delivered = 0;
    syncReported = 0;
    syncLast = 0;
    memset(profileCurrent, 0, sizeof(profileCurrent));
    memset(errLastAction, 0, sizeof(errLastAction));
    memset(errLastResult, 0, sizeof(errLastResult));
    memset(handState, 0, sizeof(handState));
    menuReported = 0;
    hbFrames = 0;
    hbLast = 0;
    hbStarted = 0;
}
#endif
