/*
===========================================================================
VrCinematicLogic.h -- pure reliable-command / cinematic-skip logic (M2B).

No engine, Android or OpenXR includes, so the exact code that runs in the
client (cl_main.c, cl_keys.c, cl_input.c) is also compiled by the host tests
in tests/vr_m2b.

Background (see docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md):
  * While a camera sequence runs, cgame queues one reliable "setCameraOrigin"
    command per rendered frame (cg_view.c).
  * Reliable commands are only acknowledged by server snapshots.  When the
    game is paused (cl_paused && sv_paused) CL_SendCmd sends nothing and the
    server sends no snapshots, so the backlog grows by one entry per frame
    until CL_AddReliableCommand raises "Client command overflow".
===========================================================================
*/

#ifndef VRCINEMATICLOGIC_H
#define VRCINEMATICLOGIC_H

#include <string.h>

/* Key codes and catcher bits as defined by the engine (keys.h / client.h). */
#define VRCIN_K_ESCAPE        27
#define VRCIN_K_SPACE         32
#define VRCIN_K_ENTER         13
#define VRCIN_KEYCATCH_CONSOLE 0x0001
#define VRCIN_KEYCATCH_UI      0x0002

/* Outstanding unacknowledged commands at which a "latest value wins"
 * command is no longer queued.  A quarter of MAX_RELIABLE_COMMANDS (256):
 * far above the 5-10 entries seen in normal play, far below the overflow. */
#define VRCIN_SUPERSEDE_HIGH_WATER 64

static inline int VrCin_Outstanding(int seq, int ack)
{
    return seq - ack;
}

/* Identical to the test in CL_AddReliableCommand. */
static inline int VrCin_WouldOverflow(int seq, int ack, int maxCommands)
{
    return (seq - ack) > maxCommands;
}

/* Same condition CL_SendCmd uses to stop sending packets. */
static inline int VrCin_ClientPaused(int comSvRunning, int svPaused, int clPaused)
{
    return comSvRunning != 0 && svPaused != 0 && clPaused != 0;
}

/* Commands that carry only the newest value of some state and are re-sent by
 * cgame every frame.  Dropping one is lossless: the next frame sends a newer
 * one.  Everything else (chat, save, skip, ...) is never dropped. */
static inline int VrCin_IsSupersedable(const char *cmd)
{
    static const char name[] = "setCameraOrigin";
    size_t n = sizeof(name) - 1;

    if (!cmd || strncmp(cmd, name, n) != 0) {
        return 0;
    }
    return cmd[n] == '\0' || cmd[n] == ' ';
}

/* 1 = do not queue this command. */
static inline int VrCin_ShouldSkipSupersedable(const char *cmd, int seq, int ack, int clientPaused)
{
    if (!VrCin_IsSupersedable(cmd)) {
        return 0;
    }
    return clientPaused || VrCin_Outstanding(seq, ack) >= VRCIN_SUPERSEDE_HIGH_WATER;
}

/* The notebook and help menus pause the game.  Stock RTCW cannot reach them
 * during a camera sequence because CL_KeyEvent eats every key; the VR layer
 * issues the console command directly, so the command itself must refuse. */
static inline int VrCin_PauseMenuAllowed(int cameraMode)
{
    return cameraMode == 0;
}

/* ---- camera interrupt ("skip cinematic") gate ---- */

typedef struct {
    int lastSeq;    /* reliable sequence of the last queued cameraInterrupt, 0 = none */
} vrCinGate_t;

static inline void VrCin_GateReset(vrCinGate_t *g)
{
    g->lastSeq = 0;
}

/* At most one unacknowledged cameraInterrupt may be in flight.  A new press
 * is accepted once the server has acknowledged the previous one. */
static inline int VrCin_InterruptAllowed(const vrCinGate_t *g, int seq, int ack)
{
    if (g->lastSeq == 0) {
        return 1;
    }
    if (g->lastSeq > seq) {
        return 1;           /* sequence numbers restarted (new connection) */
    }
    return ack >= g->lastSeq;
}

/* Call after the command was queued; seqAfter is clc.reliableSequence. */
static inline void VrCin_InterruptQueued(vrCinGate_t *g, int seqAfter)
{
    g->lastSeq = seqAfter;
}

/* Result of CL_KeyEvent's camera-mode block. */
enum {
    VRCIN_KEY_PASS = 0,                 /* not handled here, continue normal processing */
    VRCIN_KEY_EAT,                      /* swallowed (cameras eat all other key downs)  */
    VRCIN_KEY_CONSOLE_ESC,              /* escape with the console open: swallowed      */
    VRCIN_KEY_INTERRUPT,                /* queue one "cameraInterrupt"                   */
    VRCIN_KEY_INTERRUPT_SUPPRESSED      /* skip key, but one is already in flight        */
};

/* Mirrors the original block in CL_KeyEvent except that a skip key whose
 * previous cameraInterrupt is still unacknowledged is suppressed. */
static inline int VrCin_CameraKey(int cameraMode, int keyCatchers, int key, int down,
                                  const vrCinGate_t *gate, int seq, int ack)
{
    if (!cameraMode) {
        return VRCIN_KEY_PASS;
    }
    if (!(keyCatchers & (VRCIN_KEYCATCH_UI | VRCIN_KEYCATCH_CONSOLE))) {
        if (down && (key == VRCIN_K_ESCAPE || key == VRCIN_K_SPACE || key == VRCIN_K_ENTER)) {
            return VrCin_InterruptAllowed(gate, seq, ack) ? VRCIN_KEY_INTERRUPT
                                                           : VRCIN_KEY_INTERRUPT_SUPPRESSED;
        }
        if (down) {
            return VRCIN_KEY_EAT;
        }
    }
    if ((keyCatchers & VRCIN_KEYCATCH_CONSOLE) && key == VRCIN_K_ESCAPE) {
        return VRCIN_KEY_CONSOLE_ESC;
    }
    return VRCIN_KEY_PASS;
}

/* ---- backlog summary for diagnostics (command names only, never payloads) ---- */

#define VRCIN_KIND_SLOTS     4
#define VRCIN_KIND_NAME_MAX  24

typedef struct {
    char kind[VRCIN_KIND_SLOTS][VRCIN_KIND_NAME_MAX];
    int  count[VRCIN_KIND_SLOTS];
    int  others;
} vrCinHistogram_t;

static inline void VrCin_HistogramInit(vrCinHistogram_t *h)
{
    memset(h, 0, sizeof(*h));
}

/* Counts the first token of cmd (letters, digits, '_', '+', '-' only). */
static inline void VrCin_HistogramAdd(vrCinHistogram_t *h, const char *cmd)
{
    char kind[VRCIN_KIND_NAME_MAX];
    int n = 0;
    int i;

    while (cmd && n < VRCIN_KIND_NAME_MAX - 1) {
        char c = cmd[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '+' || c == '-';
        if (!ok) {
            break;
        }
        kind[n++] = c;
    }
    if (n == 0) {
        strcpy(kind, "?");
    } else {
        kind[n] = '\0';
    }

    for (i = 0; i < VRCIN_KIND_SLOTS; i++) {
        if (h->count[i] > 0 && strcmp(h->kind[i], kind) == 0) {
            h->count[i]++;
            return;
        }
    }
    for (i = 0; i < VRCIN_KIND_SLOTS; i++) {
        if (h->count[i] == 0) {
            strcpy(h->kind[i], kind);
            h->count[i] = 1;
            return;
        }
    }
    h->others++;
}

#endif /* VRCINEMATICLOGIC_H */
