/*
===========================================================================
VrLifecycleLogic.h -- pure OpenXR session / Android lifecycle decisions.

Small static inline functions with plain C types, so the exact code that runs
on the headset is also compiled and unit tested on a host (tests/vr_lifecycle).
No OpenXR, Android or engine includes: the native glue (TBXR_Common.c,
RTCWVR_SurfaceView.c) translates XrSessionState / XrResult into the numbers
below and carries out the returned action.  The numeric values equal the
OpenXR ones; TBXR_Common.c asserts that at compile time.

Khronos lifecycle rules this implements (OpenXR 1.0, "Session Lifecycle"):
  IDLE -> READY            : the app calls xrBeginSession, then submits frames
  SYNCHRONIZED/VISIBLE/FOCUSED : the session keeps running; only FOCUSED gets input
  STOPPING                 : the app calls xrEndSession and stops submitting frames
  STOPPING -> IDLE -> READY : the session can be started again (headset woke up)
  LOSS_PENDING             : the session is lost; destroy it and create a new one
  EXITING                  : the app should shut down
  instance loss            : the instance is gone; recreate everything or exit
===========================================================================
*/

#ifndef VRLIFECYCLELOGIC_H
#define VRLIFECYCLELOGIC_H

/* XrSessionState values */
#define VRL_XR_UNKNOWN          0
#define VRL_XR_IDLE             1
#define VRL_XR_READY            2
#define VRL_XR_SYNCHRONIZED     3
#define VRL_XR_VISIBLE          4
#define VRL_XR_FOCUSED          5
#define VRL_XR_STOPPING         6
#define VRL_XR_LOSS_PENDING     7
#define VRL_XR_EXITING          8

/* XrResult values the lifecycle cares about */
#define VRL_XR_SUCCESS                  0
#define VRL_XR_TIMEOUT_EXPIRED          1
#define VRL_XR_SESSION_LOSS_PENDING     3
#define VRL_XR_FRAME_DISCARDED          9
#define VRL_XR_ERROR_INSTANCE_LOST      (-13)
#define VRL_XR_ERROR_SESSION_NOT_RUNNING (-16)
#define VRL_XR_ERROR_SESSION_LOST       (-17)

/* Give up (clean exit instead of a frozen black screen) after this many failures in a row. */
#define VRL_MAX_BEGIN_FAILURES      3
#define VRL_MAX_RECREATE_FAILURES   3
#define VRL_SWAPCHAIN_MAX_TIMEOUTS  5

typedef enum {
	VRL_ACT_NONE = 0,
	VRL_ACT_BEGIN_SESSION,          /* xrBeginSession                                    */
	VRL_ACT_END_SESSION,            /* xrEndSession                                      */
	VRL_ACT_DESTROY_AND_RECREATE,   /* destroy session + graphics objects, build again   */
	VRL_ACT_EXIT_APP                /* shut the application down cleanly                 */
} vrlAction_t;

typedef enum {
	VRL_FRAME_OK = 0,               /* keep going                                        */
	VRL_FRAME_SKIP,                 /* abandon this frame, do not submit                 */
	VRL_FRAME_RECREATE,             /* abandon this frame, session must be recreated     */
	VRL_FRAME_EXIT                  /* abandon this frame, application must exit         */
} vrlFrameVerdict_t;

typedef struct {
	int xrState;            /* last XrSessionState reported by the runtime           */
	int running;            /* xrBeginSession succeeded and xrEndSession not yet done */
	int focused;
	int visible;
	int sessionLost;        /* LOSS_PENDING / SESSION_LOST seen: session must be rebuilt */
	int exitRequested;      /* EXITING or instance loss, or unrecoverable failures    */
	int graphicsValid;      /* GL context, swapchains and spaces belong to the live session */
	int beginFailures;      /* consecutive xrBeginSession failures                    */
	int recreateFailures;   /* consecutive failures to create a new session           */
	int androidResumed;     /* Activity is between onResume and onPause               */
	int surfaceId;          /* identity of the current Android Surface/window, 0 = none */
	int beginCount;         /* statistics, used by tests and diagnostics              */
	int endCount;
	int recreateCount;
} vrLifecycle_t;

static inline void VrLife_Init( vrLifecycle_t *lc ) {
	int *p = (int *)lc;
	unsigned i;

	for ( i = 0; i < sizeof( *lc ) / sizeof( int ); i++ ) {
		p[i] = 0;
	}
	lc->graphicsValid = 1;
}

static inline const char *VrLife_StateName( int state ) {
	switch ( state ) {
	case VRL_XR_UNKNOWN:        return "UNKNOWN";
	case VRL_XR_IDLE:           return "IDLE";
	case VRL_XR_READY:          return "READY";
	case VRL_XR_SYNCHRONIZED:   return "SYNCHRONIZED";
	case VRL_XR_VISIBLE:        return "VISIBLE";
	case VRL_XR_FOCUSED:        return "FOCUSED";
	case VRL_XR_STOPPING:       return "STOPPING";
	case VRL_XR_LOSS_PENDING:   return "LOSS_PENDING";
	case VRL_XR_EXITING:        return "EXITING";
	default:                    return "INVALID";
	}
}

static inline const char *VrLife_ActionName( int action ) {
	switch ( action ) {
	case VRL_ACT_NONE:                  return "none";
	case VRL_ACT_BEGIN_SESSION:         return "begin";
	case VRL_ACT_END_SESSION:           return "end";
	case VRL_ACT_DESTROY_AND_RECREATE:  return "recreate";
	case VRL_ACT_EXIT_APP:              return "exit";
	default:                            return "?";
	}
}

/* A new session state arrived from xrPollEvent.  Returns what the app must do. */
static inline vrlAction_t VrLife_OnSessionState( vrLifecycle_t *lc, int newState ) {
	lc->xrState = newState;

	switch ( newState ) {
	case VRL_XR_IDLE:
		lc->focused = lc->visible = 0;
		/* a missed STOPPING: end the session so the app and the runtime agree */
		return lc->running ? VRL_ACT_END_SESSION : VRL_ACT_NONE;

	case VRL_XR_READY:
		lc->focused = lc->visible = 0;
		if ( lc->running || lc->exitRequested ) {
			return VRL_ACT_NONE;                    /* duplicate event / shutting down */
		}
		if ( lc->sessionLost || !lc->graphicsValid ) {
			return VRL_ACT_DESTROY_AND_RECREATE;    /* never begin a session on dead resources */
		}
		return VRL_ACT_BEGIN_SESSION;

	case VRL_XR_SYNCHRONIZED:
		lc->visible = lc->focused = 0;
		return VRL_ACT_NONE;

	case VRL_XR_VISIBLE:
		lc->visible = 1;
		lc->focused = 0;
		return VRL_ACT_NONE;

	case VRL_XR_FOCUSED:
		lc->visible = lc->focused = 1;
		return VRL_ACT_NONE;

	case VRL_XR_STOPPING:
		lc->focused = lc->visible = 0;
		return lc->running ? VRL_ACT_END_SESSION : VRL_ACT_NONE;   /* no assert: tolerate */

	case VRL_XR_LOSS_PENDING:
		lc->focused = lc->visible = 0;
		lc->sessionLost = 1;
		return VRL_ACT_DESTROY_AND_RECREATE;

	case VRL_XR_EXITING:
		lc->focused = lc->visible = 0;
		lc->exitRequested = 1;
		return VRL_ACT_EXIT_APP;

	default:
		return VRL_ACT_NONE;
	}
}

/* xrBeginSession returned. */
static inline vrlAction_t VrLife_OnBeginResult( vrLifecycle_t *lc, int xrResult ) {
	lc->beginCount++;
	if ( xrResult >= 0 ) {
		lc->running = 1;
		lc->beginFailures = 0;
		return VRL_ACT_NONE;
	}
	lc->running = 0;
	lc->beginFailures++;
	if ( lc->beginFailures >= VRL_MAX_BEGIN_FAILURES ) {
		lc->exitRequested = 1;
		return VRL_ACT_EXIT_APP;
	}
	return VRL_ACT_NONE;
}

/* xrEndSession returned.  The session is considered stopped unless the runtime
 * says it was not in the STOPPING state at all. */
static inline void VrLife_OnEndResult( vrLifecycle_t *lc, int xrResult ) {
	lc->endCount++;
	(void)xrResult;
	lc->running = 0;
	lc->focused = lc->visible = 0;
}

/* Result of xrWaitFrame / xrBeginFrame / xrEndFrame. */
static inline vrlFrameVerdict_t VrLife_OnFrameResult( vrLifecycle_t *lc, int xrResult ) {
	if ( xrResult >= 0 ) {
		if ( xrResult == VRL_XR_SESSION_LOSS_PENDING ) {
			lc->sessionLost = 1;            /* success code, the frame itself is fine */
			return VRL_FRAME_RECREATE;
		}
		return VRL_FRAME_OK;                /* includes XR_FRAME_DISCARDED */
	}
	switch ( xrResult ) {
	case VRL_XR_ERROR_SESSION_NOT_RUNNING:
		lc->running = 0;                    /* resynchronise with the runtime */
		lc->focused = lc->visible = 0;
		return VRL_FRAME_SKIP;
	case VRL_XR_ERROR_SESSION_LOST:
		lc->sessionLost = 1;
		lc->running = 0;
		return VRL_FRAME_RECREATE;
	case VRL_XR_ERROR_INSTANCE_LOST:
		lc->exitRequested = 1;
		lc->running = 0;
		return VRL_FRAME_EXIT;
	default:
		return VRL_FRAME_SKIP;
	}
}

/* Frames may only be waited for, begun, rendered and submitted while this is true. */
static inline int VrLife_CanRender( const vrLifecycle_t *lc ) {
	return lc->running && !lc->sessionLost && !lc->exitRequested && lc->graphicsValid;
}

typedef enum {
	VRL_WAIT_PROCEED = 0,   /* can render, go on with the frame                      */
	VRL_WAIT_KEEP,          /* nothing to do yet: poll events and sleep a little      */
	VRL_WAIT_RECREATE,      /* destroy and recreate the session before continuing     */
	VRL_WAIT_EXIT           /* the application is going away                          */
} vrlWait_t;

/* What the render thread does while it is waiting for the session. */
static inline vrlWait_t VrLife_WaitDecision( const vrLifecycle_t *lc, int activityDestroyed ) {
	if ( activityDestroyed || lc->exitRequested ) {
		return VRL_WAIT_EXIT;
	}
	if ( lc->sessionLost || !lc->graphicsValid ) {
		return VRL_WAIT_RECREATE;
	}
	if ( lc->running ) {
		return VRL_WAIT_PROCEED;
	}
	return VRL_WAIT_KEEP;
}

/* A new session could (not) be created after loss. */
static inline vrlAction_t VrLife_OnRecreateResult( vrLifecycle_t *lc, int success ) {
	lc->recreateCount++;
	if ( success ) {
		lc->sessionLost = 0;
		lc->graphicsValid = 1;
		lc->recreateFailures = 0;
		lc->running = 0;                    /* the new session starts in IDLE -> READY */
		lc->beginFailures = 0;
		return VRL_ACT_NONE;
	}
	lc->recreateFailures++;
	if ( lc->recreateFailures >= VRL_MAX_RECREATE_FAILURES ) {
		lc->exitRequested = 1;
		return VRL_ACT_EXIT_APP;
	}
	return VRL_ACT_DESTROY_AND_RECREATE;
}

/* The old graphics objects were destroyed (session gone). */
static inline void VrLife_OnSessionDestroyed( vrLifecycle_t *lc ) {
	lc->running = 0;
	lc->focused = lc->visible = 0;
	lc->graphicsValid = 0;                  /* until a new session + swapchains exist */
}

/* The GL context the session was created with is no longer current. */
static inline void VrLife_OnGraphicsLost( vrLifecycle_t *lc ) {
	lc->graphicsValid = 0;
}

/* Android Activity callbacks (UI thread). */
static inline void VrLife_OnAndroidResume( vrLifecycle_t *lc ) {
	lc->androidResumed = 1;
}

static inline void VrLife_OnAndroidPause( vrLifecycle_t *lc ) {
	lc->androidResumed = 0;
}

/* Android Surface created/changed (id != 0) or destroyed (id == 0).  Returns 1 when the
 * identity really changed; a repeated callback for the same surface changes nothing, so
 * no handle is released and re-acquired for no reason. */
static inline int VrLife_OnSurface( vrLifecycle_t *lc, int id ) {
	if ( lc->surfaceId == id ) {
		return 0;
	}
	lc->surfaceId = id;
	return 1;
}

/* Swapchain acquire: how long to keep waiting. */
static inline int VrLife_SwapchainShouldRetry( const vrLifecycle_t *lc, int timeoutsSoFar ) {
	return VrLife_CanRender( lc ) && timeoutsSoFar < VRL_SWAPCHAIN_MAX_TIMEOUTS;
}

/* Waiting messages are logged on the first call and then every `intervalMs`. */
static inline int VrLife_WaitLogDue( int waitedMs, int lastLogMs, int intervalMs ) {
	return lastLogMs < 0 || waitedMs - lastLogMs >= intervalMs;
}

#endif /* VRLIFECYCLELOGIC_H */
