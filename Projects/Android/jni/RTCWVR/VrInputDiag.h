/*
===========================================================================
VrInputDiag.h -- optional, rate-limited controller input diagnostics.

All output goes to Android logcat with the tag "RTCWInput".  One event per
line, format "v1 t=<ms> ev=<name> key=value ...".  See docs/INPUT_DIAGNOSTICS.md
for the full event list.

Controlled by the cvar "vr_input_diag":
  0 = silent
  1 = state changes only (default, rate limited)
===========================================================================
*/

#ifndef VRINPUTDIAG_H
#define VRINPUTDIAG_H

#include <stdint.h>

#define VRDIAG_TAG "RTCWInput"

/* Actions sampled for every hand each frame. */
enum {
    VRDIAG_ACT_TRIGGER,
    VRDIAG_ACT_GRIP,
    VRDIAG_ACT_FACE1,       /* X on the left hand, A on the right hand */
    VRDIAG_ACT_FACE2,       /* Y on the left hand, B on the right hand */
    VRDIAG_ACT_MENU,
    VRDIAG_ACT_STICKCLICK,
    VRDIAG_ACT_SELECT,
    VRDIAG_ACT_STICK,
    VRDIAG_ACT_COUNT
};

typedef struct {
    int      isActive[VRDIAG_ACT_COUNT];   /* XrActionState*::isActive         */
    float    value[VRDIAG_ACT_COUNT];      /* currentState (bool as 0/1)       */
    uint32_t buttons;                      /* composed Buttons bitmask         */
} vrDiagHandSample_t;

int  VrInputDiag_Enabled(void);

/* Generic event: "v1 t=<ms> ev=<ev> <body>". */
void VrInputDiag_Event(const char *ev, const char *fmt, ...);

void VrInputDiag_Extension(const char *name, int enabled);
void VrInputDiag_SuggestResult(const char *profile, int result, const char *resultName, int count, int retry);
void VrInputDiag_AttachResult(int result, const char *resultName);
void VrInputDiag_SyncResult(int result, const char *resultName);
void VrInputDiag_Profile(int hand, const char *path);
void VrInputDiag_ActionError(int hand, const char *action, int result, const char *resultName);

/* Called once per hand per frame; logs only when something changed. */
void VrInputDiag_HandSample(int hand, const vrDiagHandSample_t *sample);

/* Menu cursor hand selection; logs only on change. */
void VrInputDiag_MenuHand(int cursorHand, int menuRightHanded, int controlsLeftHanded, int screen);

/* A controller button produced an engine key event (queued with Sys_QueEvent). */
void VrInputDiag_KeyGenerated(int hand, int key, int down);

/* The engine dispatched a key event (called from CL_KeyEvent). */
void VrInputDiag_KeyDelivered(int key, int down, int keyCatchers);

/* Once per frame; emits a summary line every few seconds. */
void VrInputDiag_Heartbeat(void);

#endif /* VRINPUTDIAG_H */
