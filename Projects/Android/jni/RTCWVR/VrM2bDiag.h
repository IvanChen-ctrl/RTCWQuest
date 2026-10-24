/*
===========================================================================
VrM2bDiag.h -- optional, rate-limited cinematic and menu-pointer diagnostics
(M2B).  Same line format as RTCWInput ("v1 t=<ms> ev=<name> k=v ..."), so the
Windows Feedback Bridge and any existing parser handle it unchanged.

Logcat tags and cvars:
  RTCWCinematic   vr_cin_diag      0 = off, 1 = state changes (default)
  RTCWPointer     vr_pointer_diag  0 = off, 1 = state changes + one sample
                                   every 2 s in menus (default),
                                   2 = a sample every 250 ms in menus
Each tag has its own token bucket (burst 30, 10 lines/s); lost lines are
reported by an "ev=drop n=<count>" line.  "ev=overflow" is always logged.
See docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md.
===========================================================================
*/

#ifndef VRM2BDIAG_H
#define VRM2BDIAG_H

#define VRCIN_DIAG_TAG "RTCWCinematic"
#define VRPTR_DIAG_TAG "RTCWPointer"

/* ---- cinematic (engine side, implemented only in the engine build) ---- */
void VrCinDiag_Frame(void);                          /* once per client frame        */
void VrCinDiag_CameraStart(void);                    /* CG_STARTCAMERA (primary)     */
void VrCinDiag_CameraStop(void);                     /* CG_STOPCAMERA (primary)      */
void VrCinDiag_Interrupt(int key, int sent);         /* skip key in camera mode      */
void VrCinDiag_Superseded(const char *cmd, int paused);
void VrCinDiag_QueueDepth(int outstanding);          /* after every queued command   */
void VrCinDiag_PauseMenuRefused(const char *what);   /* notebook/help in a cutscene  */
void VrCinDiag_Overflow(const char *kinds);          /* just before ERR_DROP         */

/* ---- pointer (value-passing, no engine types) ---- */
void VrPtrDiag_Snap(const char *reason, float hmdYaw, float quadYaw, float x, float y, float z, int screen);
void VrPtrDiag_Pose(const char *who, int usable, unsigned flags);   /* on change only */
void VrPtrDiag_Recenter(float hmdYawBefore);
void VrPtrDiag_MenuHand(int hand);                                   /* on change only */
void VrPtrDiag_Sample(float ctrlYaw, float ctrlPitch, float refYaw, float hmdYaw, float u, float v);
void VrPtrDiag_Rejected(float ctrlYaw, float ctrlPitch, float refYaw);

/* Generic channel events (also used by the tests). */
void VrCinDiag_Event(const char *ev, const char *fmt, ...);
void VrPtrDiag_Event(const char *ev, const char *fmt, ...);

#endif /* VRM2BDIAG_H */
