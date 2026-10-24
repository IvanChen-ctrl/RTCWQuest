/*
 * Cinematic / reliable-command tests.
 *
 * The decision functions are the production ones from VrCinematicLogic.h (the
 * same inline functions cl_main.c, cl_keys.c and cl_input.c call).  Only the
 * queue bookkeeping around them is modelled here, mirroring
 * CL_AddReliableCommand / CL_AddCgameReliableCommand / the camera block of
 * CL_KeyEvent line for line.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "VrCinematicLogic.h"
#include "VrM2bDiag.h"

#define MAX_RELIABLE 256   /* qcommon.h: MAX_RELIABLE_COMMANDS */

void HostDiagReset(void);
int  HostLinesContain(int channel, const char *needle);
extern int hostLastPrio[2];
extern int hostNow;
extern int hostLevel[2];
extern int hostLineCount[2];

typedef struct {
    int  seq, ack;
    char ring[MAX_RELIABLE][64];
    int  svPaused;           /* sv_paused and cl_paused together */
    int  cameraMode;
    int  catchers;
    vrCinGate_t gate;
    int  useFix;             /* 0 = behaviour before M2B                        */
    int  overflowed;         /* Com_Error( ERR_DROP, "Client command overflow" ) */
    int  interruptsQueued;
    int  interruptsSuppressed;
    int  superseded;
    int  maxOutstanding;
    int  keysDown[256];
} sim_t;

/* CL_AddReliableCommand */
static void simAdd(sim_t *s, const char *cmd)
{
    if (s->overflowed) {
        return;
    }
    if (VrCin_WouldOverflow(s->seq, s->ack, MAX_RELIABLE)) {
        s->overflowed = 1;
        return;
    }
    s->seq++;
    snprintf(s->ring[s->seq & (MAX_RELIABLE - 1)], sizeof(s->ring[0]), "%s", cmd);
    if (VrCin_Outstanding(s->seq, s->ack) > s->maxOutstanding) {
        s->maxOutstanding = VrCin_Outstanding(s->seq, s->ack);
    }
}

/* CL_AddCgameReliableCommand (trap_SendClientCommand) */
static void simCgameSend(sim_t *s, const char *cmd)
{
    if (s->useFix) {
        int paused = VrCin_ClientPaused(1, s->svPaused, s->svPaused);
        if (VrCin_ShouldSkipSupersedable(cmd, s->seq, s->ack, paused)) {
            s->superseded++;
            return;
        }
    }
    simAdd(s, cmd);
}

/* CL_KeyEvent: camera-mode block.  Returns 1 when the event was consumed. */
static int simKey(sim_t *s, int key, int down)
{
    s->keysDown[key & 255] = down;
    if (!s->cameraMode) {
        return 0;
    }
    switch (VrCin_CameraKey(s->cameraMode, s->catchers, key, down, &s->gate, s->seq, s->ack)) {
    case VRCIN_KEY_INTERRUPT:
        simAdd(s, "cameraInterrupt");
        VrCin_InterruptQueued(&s->gate, s->seq);
        s->interruptsQueued++;
        return 1;
    case VRCIN_KEY_INTERRUPT_SUPPRESSED:
        s->interruptsSuppressed++;
        return 1;
    case VRCIN_KEY_EAT:
    case VRCIN_KEY_CONSOLE_ESC:
        return 1;
    default:
        return 0;
    }
}

static void simCameraStart(sim_t *s)
{
    s->cameraMode = 1;
    VrCin_GateReset(&s->gate);
}

static void simCameraStop(sim_t *s)
{
    s->cameraMode = 0;
    VrCin_GateReset(&s->gate);
}

/* One rendered frame.  cgame queues setCameraOrigin while the camera runs; the
 * server acknowledges everything every `ackEvery` frames unless paused (no
 * packets are sent and no snapshots arrive while paused). */
static void simFrame(sim_t *s, int frameNo, int ackEvery)
{
    if (s->cameraMode) {
        simCgameSend(s, "setCameraOrigin 100.000000 200.000000 64.000000");
    }
    if (!s->svPaused && ackEvery > 0 && frameNo % ackEvery == 0) {
        s->ack = s->seq;
    }
}

static void simInit(sim_t *s, int useFix)
{
    memset(s, 0, sizeof(*s));
    s->useFix = useFix;
}

/* ===================== tests ===================== */

static void testOverflowBoundary(void)
{
    sim_t s;

    /* The error condition is unchanged: outstanding > 256 before queueing. */
    CHECK(!VrCin_WouldOverflow(256, 0, MAX_RELIABLE));
    CHECK(VrCin_WouldOverflow(257, 0, MAX_RELIABLE));
    CHECK(!VrCin_WouldOverflow(1256, 1000, MAX_RELIABLE));
    CHECK(VrCin_WouldOverflow(1257, 1000, MAX_RELIABLE));

    /* Non-supersedable commands are never dropped: 257 fit, the 258th raises the error. */
    simInit(&s, 1);
    for (int i = 0; i < 257; i++) {
        simCgameSend(&s, "say hello");
    }
    CHECK(!s.overflowed);
    CHECK(VrCin_Outstanding(s.seq, s.ack) == 257);
    simCgameSend(&s, "say hello");
    CHECK(s.overflowed);               /* the real error is not hidden */
    CHECK(s.superseded == 0);
}

static void testSupersedableClassification(void)
{
    CHECK(VrCin_IsSupersedable("setCameraOrigin 1 2 3"));
    CHECK(VrCin_IsSupersedable("setCameraOrigin"));
    CHECK(!VrCin_IsSupersedable("setCameraOriginX 1"));
    CHECK(!VrCin_IsSupersedable("cameraInterrupt"));
    CHECK(!VrCin_IsSupersedable("stopCamera"));
    CHECK(!VrCin_IsSupersedable("startCamera"));
    CHECK(!VrCin_IsSupersedable("savegame quicksave"));
    CHECK(!VrCin_IsSupersedable(""));
    CHECK(!VrCin_IsSupersedable(NULL));

    CHECK(!VrCin_ShouldSkipSupersedable("cameraInterrupt", 9999, 0, 1));
    CHECK(VrCin_ShouldSkipSupersedable("setCameraOrigin 1 2 3", 10, 10, 1));        /* paused */
    CHECK(!VrCin_ShouldSkipSupersedable("setCameraOrigin 1 2 3", 10, 10, 0));
    CHECK(!VrCin_ShouldSkipSupersedable("setCameraOrigin 1 2 3", 63, 0, 0));
    CHECK(VrCin_ShouldSkipSupersedable("setCameraOrigin 1 2 3", 64, 0, 0));         /* backlog */
}

/* The mechanism behind "Client command overflow": pause during a camera. */
static void testPausedCameraOverflowBeforeAndAfterFix(void)
{
    sim_t legacy;
    sim_t fixed;
    int legacyOverflowFrame = -1;

    simInit(&legacy, 0);
    simInit(&fixed, 1);
    simCameraStart(&legacy);
    simCameraStart(&fixed);

    /* 2 s of normal playback at 90 fps, then the game is paused (notebook). */
    for (int f = 1; f <= 180; f++) {
        simFrame(&legacy, f, 5);
        simFrame(&fixed, f, 5);
    }
    CHECK(!legacy.overflowed && !fixed.overflowed);
    legacy.svPaused = fixed.svPaused = 1;

    for (int f = 181; f <= 181 + 1000; f++) {
        simFrame(&legacy, f, 5);
        simFrame(&fixed, f, 5);
        if (legacy.overflowed && legacyOverflowFrame < 0) {
            legacyOverflowFrame = f - 180;
        }
    }
    /* Before the fix: overflow roughly 3 s (257 frames) after the pause. */
    CHECK(legacy.overflowed);
    CHECK(legacyOverflowFrame >= 250 && legacyOverflowFrame <= 262);
    /* After the fix: the queue stays far below the limit and nothing errors. */
    CHECK(!fixed.overflowed);
    CHECK(fixed.maxOutstanding < VRCIN_SUPERSEDE_HIGH_WATER + 10);
    CHECK(fixed.superseded > 900);

    /* Unpausing resumes normal queueing: the first acknowledgment clears the
     * backlog and later frames queue setCameraOrigin again. */
    fixed.svPaused = 0;
    int supersededBefore = fixed.superseded;
    int seqBefore = fixed.seq;
    for (int f = 1200; f < 1300; f++) {
        simFrame(&fixed, f, 5);
    }
    CHECK(fixed.superseded - supersededBefore < 5);
    CHECK(fixed.seq - seqBefore > 90);
    CHECK(!fixed.overflowed);
    CHECK(VrCin_Outstanding(fixed.seq, fixed.ack) < 10);
}

static void testNaturalCompletion(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    /* 50 s at 90 fps, snapshots (acks) at 20 Hz: every 4-5 frames. */
    for (int f = 1; f <= 4500; f++) {
        simFrame(&s, f, 4);
    }
    CHECK(!s.overflowed);
    CHECK(s.superseded == 0);              /* normal playback is untouched */
    CHECK(s.maxOutstanding <= 8);
    CHECK(s.interruptsQueued == 0);

    /* Camera ends by itself: stopCamera is a normal, never-dropped command. */
    simCgameSend(&s, "stopCamera");
    simCameraStop(&s);
    s.ack = s.seq;
    CHECK(!s.overflowed);
    CHECK(VrCin_Outstanding(s.seq, s.ack) == 0);
    CHECK(strcmp(s.ring[s.seq & (MAX_RELIABLE - 1)], "stopCamera") == 0);
}

static void testSingleShortSkip(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    for (int f = 1; f <= 90; f++) {
        simFrame(&s, f, 4);
    }
    CHECK(simKey(&s, VRCIN_K_ESCAPE, 1));      /* press   */
    CHECK(!simKey(&s, VRCIN_K_ESCAPE, 0));     /* release passes through */
    CHECK(s.interruptsQueued == 1);
    CHECK(s.interruptsSuppressed == 0);
    CHECK(strcmp(s.ring[s.seq & (MAX_RELIABLE - 1)], "cameraInterrupt") == 0);
    CHECK(s.keysDown[VRCIN_K_ESCAPE] == 0);    /* press/release state consistent */
}

static void testPressAndHold(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    /* Key repeat (as on a keyboard): 300 down events with no snapshot in between. */
    for (int i = 0; i < 300; i++) {
        simKey(&s, VRCIN_K_SPACE, 1);
    }
    CHECK(s.interruptsQueued == 1);
    CHECK(s.interruptsSuppressed == 299);
    CHECK(!s.overflowed);
    CHECK(VrCin_Outstanding(s.seq, s.ack) == 1);

    /* Held across many frames with acks arriving: still exactly one request per
     * unacknowledged window, never a stream. */
    simInit(&s, 1);
    simCameraStart(&s);
    for (int f = 1; f <= 270; f++) {          /* 3 s held at 90 fps */
        simKey(&s, VRCIN_K_ENTER, 1);
        simFrame(&s, f, 4);
    }
    CHECK(!s.overflowed);
    CHECK(s.interruptsQueued <= 270 / 4 + 1);  /* at most one per ack window  */
    CHECK(s.interruptsQueued >= 1);
}

static void testRapidPresses(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    const int keys[] = { VRCIN_K_ESCAPE, VRCIN_K_SPACE, VRCIN_K_ENTER, VRCIN_K_ESCAPE, VRCIN_K_SPACE };
    for (int i = 0; i < 5; i++) {
        simKey(&s, keys[i], 1);
        simKey(&s, keys[i], 0);
    }
    CHECK(s.interruptsQueued == 1);            /* five presses before any ack -> one request */
    CHECK(s.interruptsSuppressed == 4);

    /* After the server acknowledged it, a deliberate new press is accepted again. */
    s.ack = s.seq;
    simKey(&s, VRCIN_K_ESCAPE, 1);
    CHECK(s.interruptsQueued == 2);
}

static void testInputDuringTransition(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    simKey(&s, VRCIN_K_ESCAPE, 1);
    int seqAfterSkip = s.seq;

    /* Camera stops (cutscene finished); the player keeps mashing the button
     * while the next map loads and gameplay starts. */
    simCameraStop(&s);
    for (int i = 0; i < 50; i++) {
        CHECK(!simKey(&s, VRCIN_K_ESCAPE, 1));  /* normal key processing, nothing queued here */
        simKey(&s, VRCIN_K_ESCAPE, 0);
    }
    CHECK(s.seq == seqAfterSkip);
    CHECK(s.interruptsQueued == 1);

    /* Other keys during the camera are eaten, not forwarded. */
    simCameraStart(&s);
    CHECK(simKey(&s, 'w', 1));
    CHECK(!simKey(&s, 'w', 0));                /* releases still flow so nothing sticks */
    CHECK(s.seq == seqAfterSkip);

    /* Menu or console on top: the camera block does not intercept. */
    s.catchers = VRCIN_KEYCATCH_UI;
    CHECK(!simKey(&s, VRCIN_K_ESCAPE, 1));
    s.catchers = VRCIN_KEYCATCH_CONSOLE;
    CHECK(simKey(&s, VRCIN_K_ESCAPE, 1));      /* console + escape: swallowed as before */
    CHECK(simKey(&s, VRCIN_K_ESCAPE, 0));
    s.catchers = 0;
    CHECK(s.seq == seqAfterSkip);
}

static void testRepeatedCompletionNotifications(void)
{
    sim_t s;

    simInit(&s, 1);
    simCameraStart(&s);
    simKey(&s, VRCIN_K_ESCAPE, 1);
    CHECK(s.gate.lastSeq != 0);

    simCameraStop(&s);
    simCameraStop(&s);                          /* duplicate completion */
    simCameraStop(&s);
    CHECK(s.gate.lastSeq == 0);
    CHECK(s.interruptsQueued == 1);

    /* A second camera sequence (next cutscene) starts with a fresh gate. */
    simCameraStart(&s);
    CHECK(simKey(&s, VRCIN_K_SPACE, 1));
    CHECK(s.interruptsQueued == 2);
}

static void testGateSequenceRestart(void)
{
    vrCinGate_t g;

    VrCin_GateReset(&g);
    CHECK(VrCin_InterruptAllowed(&g, 0, 0));
    VrCin_InterruptQueued(&g, 500);
    CHECK(!VrCin_InterruptAllowed(&g, 500, 499));
    CHECK(VrCin_InterruptAllowed(&g, 500, 500));
    CHECK(VrCin_InterruptAllowed(&g, 3, 2));    /* new connection restarted the counters */
}

static void testStallWithoutPauseStillErrors(void)
{
    /* The fix is not a blanket suppression: a stall with a *non-superseding*
     * command stream still overflows exactly like before. */
    sim_t s;

    simInit(&s, 1);
    s.svPaused = 1;
    for (int i = 0; i < 400; i++) {
        simCgameSend(&s, "fogswitch 0 0 0");
    }
    CHECK(s.overflowed);
}

static void testPauseMenuRefusedDuringCamera(void)
{
    CHECK(!VrCin_PauseMenuAllowed(1));
    CHECK(VrCin_PauseMenuAllowed(0));
}

static void testHistogram(void)
{
    vrCinHistogram_t h;

    VrCin_HistogramInit(&h);
    for (int i = 0; i < 200; i++) {
        VrCin_HistogramAdd(&h, "setCameraOrigin 12.5 99.0 3.0");
    }
    VrCin_HistogramAdd(&h, "cameraInterrupt");
    VrCin_HistogramAdd(&h, "cameraInterrupt");
    VrCin_HistogramAdd(&h, "say secret text");
    VrCin_HistogramAdd(&h, "userinfo \"\\name\\x\"");
    VrCin_HistogramAdd(&h, "fifth kind");              /* no slot left */
    VrCin_HistogramAdd(&h, "");

    CHECK(strcmp(h.kind[0], "setCameraOrigin") == 0 && h.count[0] == 200);
    CHECK(strcmp(h.kind[1], "cameraInterrupt") == 0 && h.count[1] == 2);
    CHECK(strcmp(h.kind[2], "say") == 0 && h.count[2] == 1);
    CHECK(strcmp(h.kind[3], "userinfo") == 0 && h.count[3] == 1);
    CHECK(h.others == 2);
    for (int i = 0; i < VRCIN_KIND_SLOTS; i++) {          /* never a payload */
        CHECK(strchr(h.kind[i], ' ') == NULL);
        CHECK(strstr(h.kind[i], "secret") == NULL);
    }
}

/* ---- RTCWCinematic diagnostics (production VrM2bDiag.c) ---- */

static void testCinematicDiagOverflowAlwaysLogged(void)
{
    /* VrCinDiag_Overflow needs the engine; in host mode the generic event path
     * is what is testable: format, tag channel, rate limiting. */
    HostDiagReset();
    VrCinDiag_Event("cam_start", "map=%s seq=%d ack=%d", "cutscene1", 10, 10);
    CHECK(HostLinesContain(0, "v1 t=1000 ev=cam_start map=cutscene1 seq=10 ack=10"));
    CHECK(!HostLinesContain(1, "cam_start"));       /* not on the pointer channel */

    /* cvar off: silent */
    HostDiagReset();
    hostLevel[0] = 0;
    VrCinDiag_Event("cam_start", "map=%s", "x");
    CHECK(!HostLinesContain(0, "cam_start"));

    /* rate limit: a flood is cut at the burst size and reported once */
    HostDiagReset();
    for (int i = 0; i < 100; i++) {
        VrCinDiag_Event("queue", "outstanding=%d", i);
    }
    CHECK(hostLineCount[0] == 30);
    hostNow += 1000;                                 /* 10 tokens refill */
    VrCinDiag_Event("queue", "outstanding=%d", 999);
    CHECK(HostLinesContain(0, "ev=drop n=70"));
    CHECK(HostLinesContain(0, "outstanding=999"));
}

void RunCinematicTests(void)
{
    testOverflowBoundary();
    testSupersedableClassification();
    testPausedCameraOverflowBeforeAndAfterFix();
    testNaturalCompletion();
    testSingleShortSkip();
    testPressAndHold();
    testRapidPresses();
    testInputDuringTransition();
    testRepeatedCompletionNotifications();
    testGateSequenceRestart();
    testStallWithoutPauseStillErrors();
    testPauseMenuRefusedDuringCamera();
    testHistogram();
    testCinematicDiagOverflowAlwaysLogged();
}
