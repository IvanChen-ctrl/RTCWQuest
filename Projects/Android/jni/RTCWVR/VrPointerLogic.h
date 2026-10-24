/*
===========================================================================
VrPointerLogic.h -- pure menu pointer / reference-yaw logic (M2B).

No engine, Android or OpenXR includes; compiled by the engine and by the host
tests in tests/vr_m2b.

Pipeline (see docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md):
  OpenXR aim pose (StageSpace) -> yaw/pitch (QuatToYawPitchRoll)
  -> VrPointer_FromAngles (relative to the yaw the menu quad was placed at)
  -> normalized (u,v) -> VrPointer_ToUi (640x480) -> UI cursor == hit test.

The cursor mapping itself (-sin(dyaw) + 0.5, (pitch - 15) / 90 + 0.5) is the
pre-existing behaviour and is deliberately unchanged.
===========================================================================
*/

#ifndef VRPOINTERLOGIC_H
#define VRPOINTERLOGIC_H

#include <math.h>
#include <string.h>

#define VRPTR_UI_WIDTH   640
#define VRPTR_UI_HEIGHT  480

/* Frames (with a usable HMD pose) during which the menu placement keeps
 * following the HMD.  Same value the original code used. */
#define VRPTR_SNAP_WARMUP_FRAMES 100

static inline int VrPointer_Finite(float f)
{
    return f == f && fabsf(f) < 1.0e18f;
}

/* A quaternion that can describe an orientation: finite and close to unit
 * length.  An all-zero quaternion is what an unfilled XrSpaceLocation holds
 * when the runtime has no pose yet. */
static inline int VrPointer_QuatUsable(float x, float y, float z, float w)
{
    float n2;

    if (!VrPointer_Finite(x) || !VrPointer_Finite(y) || !VrPointer_Finite(z) || !VrPointer_Finite(w)) {
        return 0;
    }
    n2 = x * x + y * y + z * z + w * w;
    return n2 > 0.81f && n2 < 1.21f;
}

typedef struct {
    float x, y, z, w;
} vrQuat_t;

/* How long (frames) an orientation that is a proper unit quaternion is
 * distrusted because the runtime does not flag it valid.  After that the data
 * is used anyway, so a runtime with unreliable flags still gets a menu. */
#define VRPTR_FLAG_FALLBACK_FRAMES 300

static inline int VrPointer_PoseUsable(int quatOk, int flagsOk, int *flagsMissingStreak)
{
    if (!quatOk) {
        *flagsMissingStreak = 0;
        return 0;
    }
    if (flagsOk) {
        *flagsMissingStreak = 0;
        return 1;
    }
    if (*flagsMissingStreak < VRPTR_FLAG_FALLBACK_FRAMES) {
        (*flagsMissingStreak)++;
        return 0;
    }
    return 1;
}

typedef struct {
    vrQuat_t held;
    int      haveHeld;
} vrPoseHold_t;

/* Keeps the newest usable orientation.  Returns 1 when `in` is usable (out =
 * in).  Otherwise out = the last usable orientation (identity before the
 * first one) and 0 is returned. */
static inline int VrPointer_HoldPose(vrPoseHold_t *h, const vrQuat_t *in, vrQuat_t *out)
{
    if (VrPointer_QuatUsable(in->x, in->y, in->z, in->w)) {
        h->held = *in;
        h->haveHeld = 1;
        *out = *in;
        return 1;
    }
    if (h->haveHeld) {
        *out = h->held;
    } else {
        out->x = out->y = out->z = 0.0f;
        out->w = 1.0f;
    }
    return 0;
}

/* Normalized cursor position from controller angles (degrees) relative to the
 * yaw the menu quad faces.  Returns 0 and leaves u/v untouched for non-finite
 * input. */
static inline int VrPointer_FromAngles(float ctrlYawDeg, float ctrlPitchDeg, float refYawDeg,
                                       float *u, float *v)
{
    const float deg2rad = 3.14159265358979f / 180.0f;
    float du;
    float dv;

    if (!VrPointer_Finite(ctrlYawDeg) || !VrPointer_Finite(ctrlPitchDeg) || !VrPointer_Finite(refYawDeg)) {
        return 0;
    }
    du = -sinf((ctrlYawDeg - refYawDeg) * deg2rad) + 0.5f;
    dv = (float)((ctrlPitchDeg - 15.0f) / 90.0) + 0.5f;
    *u = du;
    *v = dv;
    return 1;
}

/* Normalized -> 640x480 UI coordinates (same truncation PortableMouseAbs
 * always used). */
static inline void VrPointer_ToUi(float u, float v, int *x, int *y)
{
    *x = (int)(u * VRPTR_UI_WIDTH);
    *y = (int)(v * VRPTR_UI_HEIGHT);
}

/* ---- menu placement ("snap") decision ----
 *
 * The menu quad is placed at the HMD yaw captured by the last snap and the
 * cursor must be computed relative to that very yaw.  A snap is taken
 *   - on each of the first VRPTR_SNAP_WARMUP_FRAMES frames with a usable HMD
 *     pose (original behaviour), restarted whenever the pose becomes usable
 *     again after being unusable;
 *   - whenever the screen layer is switched on or off;
 *   - after a reference-space change (recenter).
 * No snap is ever taken from an unusable pose, so a zero/stale orientation
 * can no longer be frozen in as the menu direction. */

enum {
    VRSNAP_NONE = 0,
    VRSNAP_WARMUP,
    VRSNAP_POSE_VALID,
    VRSNAP_SCREEN_TOGGLE,
    VRSNAP_RECENTER
};

typedef struct {
    int framesValid;
    int poseWasValid;
    int lastScreen;
    int recenterPending;
} vrSnapState_t;

static inline void VrSnap_Init(vrSnapState_t *s)
{
    memset(s, 0, sizeof(*s));
}

static inline void VrSnap_RequestRecenter(vrSnapState_t *s)
{
    s->recenterPending = 1;
}

/* Returns one of VRSNAP_*; anything but VRSNAP_NONE means "take a snap now". */
static inline int VrSnap_Update(vrSnapState_t *s, int useScreen, int poseUsable)
{
    int reason = VRSNAP_NONE;
    int screenChanged = (s->lastScreen != (useScreen != 0));

    s->lastScreen = (useScreen != 0);

    if (!poseUsable) {
        s->poseWasValid = 0;
        return VRSNAP_NONE;     /* keep the previous snap, remember the toggle */
    }
    if (!s->poseWasValid) {
        s->poseWasValid = 1;
        s->framesValid = 0;
        reason = VRSNAP_POSE_VALID;
    } else if (s->recenterPending) {
        reason = VRSNAP_RECENTER;
    } else if (screenChanged) {
        reason = VRSNAP_SCREEN_TOGGLE;
    }
    s->recenterPending = 0;

    if (reason == VRSNAP_NONE && s->framesValid < VRPTR_SNAP_WARMUP_FRAMES) {
        reason = VRSNAP_WARMUP;
    }
    if (s->framesValid < VRPTR_SNAP_WARMUP_FRAMES) {
        s->framesValid++;
    }
    return reason;
}

#endif /* VRPOINTERLOGIC_H */
