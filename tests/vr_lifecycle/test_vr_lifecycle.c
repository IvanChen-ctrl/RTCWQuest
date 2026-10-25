/*
 * Host tests for VrLifecycleLogic.h (OpenXR session + Android lifecycle decisions).
 * A tiny fake runtime/driver applies the actions the way TBXR_Common.c does, so whole
 * pause/resume sequences can be replayed deterministically.  This does NOT prove the real
 * PICO 4 runtime behaviour; see docs/M3_GAMEPLAY_MERCENARY.md for the hardware plan.
 *
 * Build and run: make -C tests/vr_lifecycle test
 */

#include <stdio.h>
#include <string.h>

#include "VrLifecycleLogic.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* ---- fake native driver: performs actions like the glue code ---- */
typedef struct {
    vrLifecycle_t lc;
    int beginResult;        /* what the fake xrBeginSession returns */
    int createOk;           /* whether a new session can be created */
    int exited;             /* application exit was triggered */
    int recreates;          /* sessions destroyed + created */
    int framesSubmitted;
} driver_t;

static void drv_init(driver_t *d)
{
    memset(d, 0, sizeof(*d));
    VrLife_Init(&d->lc);
    d->createOk = 1;
}

static void drv_apply(driver_t *d, vrlAction_t act)
{
    int guard = 0;
    while (act != VRL_ACT_NONE) {
        if (++guard > 50) {                 /* an unbounded recovery loop must fail, not hang */
            printf("  FAIL: recovery did not terminate\n");
            failures++;
            return;
        }
        vrlAction_t next = VRL_ACT_NONE;
        switch (act) {
        case VRL_ACT_BEGIN_SESSION:
            next = VrLife_OnBeginResult(&d->lc, d->beginResult);
            break;
        case VRL_ACT_END_SESSION:
            VrLife_OnEndResult(&d->lc, 0);
            break;
        case VRL_ACT_DESTROY_AND_RECREATE:
            VrLife_OnSessionDestroyed(&d->lc);
            next = VrLife_OnRecreateResult(&d->lc, d->createOk);
            if (d->createOk) {
                d->recreates++;
            }
            break;
        case VRL_ACT_EXIT_APP:
            d->exited = 1;
            break;
        default:
            break;
        }
        act = next;
    }
}

static void drv_state(driver_t *d, int state)
{
    drv_apply(d, VrLife_OnSessionState(&d->lc, state));
}

static void drv_frame(driver_t *d)
{
    if (VrLife_CanRender(&d->lc)) {
        d->framesSubmitted++;
    }
}

/* ---- tests ---- */

static void test_initial_state(void)
{
    driver_t d;
    drv_init(&d);
    CHECK(!VrLife_CanRender(&d.lc));
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_KEEP);
}

static void test_normal_startup_to_focused(void)
{
    driver_t d;
    drv_init(&d);
    drv_state(&d, VRL_XR_IDLE);
    CHECK(!d.lc.running);
    drv_state(&d, VRL_XR_READY);
    CHECK(d.lc.running && d.lc.beginCount == 1);
    drv_state(&d, VRL_XR_SYNCHRONIZED);
    CHECK(d.lc.running && !d.lc.visible && !d.lc.focused);
    drv_state(&d, VRL_XR_VISIBLE);
    CHECK(d.lc.visible && !d.lc.focused);
    drv_state(&d, VRL_XR_FOCUSED);
    CHECK(d.lc.visible && d.lc.focused);
    CHECK(VrLife_CanRender(&d.lc));
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_PROCEED);
}

static void bring_up(driver_t *d)
{
    drv_state(d, VRL_XR_IDLE);
    drv_state(d, VRL_XR_READY);
    drv_state(d, VRL_XR_SYNCHRONIZED);
    drv_state(d, VRL_XR_VISIBLE);
    drv_state(d, VRL_XR_FOCUSED);
}

static void test_standby_ready_stopping_idle_ready(void)
{
    /* headset goes to sleep and wakes up: FOCUSED -> VISIBLE -> SYNCHRONIZED -> STOPPING -> IDLE -> READY */
    driver_t d;
    drv_init(&d);
    bring_up(&d);

    drv_state(&d, VRL_XR_VISIBLE);
    drv_state(&d, VRL_XR_SYNCHRONIZED);
    CHECK(d.lc.running && !d.lc.focused);
    drv_state(&d, VRL_XR_STOPPING);
    CHECK(!d.lc.running && d.lc.endCount == 1);
    CHECK(!VrLife_CanRender(&d.lc));                        /* no frame submission while stopped */
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_KEEP);
    drv_state(&d, VRL_XR_IDLE);
    CHECK(!d.lc.running);

    drv_state(&d, VRL_XR_READY);                            /* woke up */
    CHECK(d.lc.running && d.lc.beginCount == 2);
    drv_state(&d, VRL_XR_SYNCHRONIZED);
    drv_state(&d, VRL_XR_VISIBLE);
    drv_state(&d, VRL_XR_FOCUSED);
    CHECK(VrLife_CanRender(&d.lc));
    CHECK(d.lc.beginCount == d.lc.endCount + 1);            /* begin/end are balanced */
}

static void test_repeated_pause_resume_is_balanced(void)
{
    driver_t d;
    int i;
    drv_init(&d);
    bring_up(&d);
    for (i = 0; i < 50; i++) {
        drv_state(&d, VRL_XR_VISIBLE);
        drv_state(&d, VRL_XR_SYNCHRONIZED);
        drv_state(&d, VRL_XR_STOPPING);
        drv_state(&d, VRL_XR_IDLE);
        drv_state(&d, VRL_XR_READY);
        drv_state(&d, VRL_XR_SYNCHRONIZED);
        drv_state(&d, VRL_XR_VISIBLE);
        drv_state(&d, VRL_XR_FOCUSED);
        drv_frame(&d);
    }
    CHECK(d.lc.beginCount == 51 && d.lc.endCount == 50);
    CHECK(d.lc.running && VrLife_CanRender(&d.lc));
    CHECK(d.framesSubmitted == 50);
    CHECK(d.recreates == 0 && !d.exited);                   /* a plain sleep never rebuilds anything */
}

static void test_duplicate_and_unexpected_events_do_not_crash(void)
{
    /* The old code asserted (aborted the process in debug builds) on these. */
    driver_t d;
    drv_init(&d);

    drv_state(&d, VRL_XR_STOPPING);                         /* STOPPING without a running session */
    CHECK(!d.lc.running && d.lc.endCount == 0);

    drv_state(&d, VRL_XR_READY);
    drv_state(&d, VRL_XR_READY);                            /* READY while already running */
    CHECK(d.lc.running && d.lc.beginCount == 1);

    drv_state(&d, VRL_XR_STOPPING);
    drv_state(&d, VRL_XR_STOPPING);                         /* STOPPING twice */
    CHECK(!d.lc.running && d.lc.endCount == 1);

    drv_state(&d, 99);                                      /* unknown value */
    drv_state(&d, VRL_XR_UNKNOWN);
    CHECK(!d.exited);
}

static void test_idle_while_running_resynchronises(void)
{
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    drv_state(&d, VRL_XR_IDLE);                             /* STOPPING was missed */
    CHECK(!d.lc.running && d.lc.endCount == 1);
}

static void test_begin_failure_is_bounded(void)
{
    driver_t d;
    int i;
    drv_init(&d);
    d.beginResult = -28;                                    /* XR_ERROR_SESSION_NOT_READY */
    for (i = 0; i < VRL_MAX_BEGIN_FAILURES - 1; i++) {
        drv_state(&d, VRL_XR_READY);
        CHECK(!d.lc.running && !d.exited);
    }
    drv_state(&d, VRL_XR_READY);
    CHECK(d.exited);                                        /* clean exit, not an endless black screen */
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_EXIT);

    /* a success in between resets the counter */
    drv_init(&d);
    d.beginResult = -28;
    drv_state(&d, VRL_XR_READY);
    drv_state(&d, VRL_XR_READY);
    d.beginResult = 0;
    drv_state(&d, VRL_XR_READY);
    CHECK(d.lc.running && d.lc.beginFailures == 0 && !d.exited);
}

static void test_loss_pending_recreates_the_session(void)
{
    driver_t d;
    drv_init(&d);
    bring_up(&d);

    drv_state(&d, VRL_XR_LOSS_PENDING);
    CHECK(d.recreates == 1 && !d.exited);
    CHECK(!d.lc.running && !d.lc.sessionLost && d.lc.graphicsValid);   /* new graphics objects exist */
    CHECK(!VrLife_CanRender(&d.lc));                        /* until the new session is READY */

    drv_state(&d, VRL_XR_IDLE);
    drv_state(&d, VRL_XR_READY);
    CHECK(d.lc.running);
    drv_state(&d, VRL_XR_FOCUSED);
    CHECK(VrLife_CanRender(&d.lc));
}

static void test_loss_pending_with_unrecoverable_runtime_exits(void)
{
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    d.createOk = 0;
    drv_state(&d, VRL_XR_LOSS_PENDING);
    CHECK(d.exited);                                        /* bounded retries, then a clean exit */
    CHECK(!VrLife_CanRender(&d.lc));
}

static void test_exiting_state_exits_the_app(void)
{
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    drv_state(&d, VRL_XR_STOPPING);
    drv_state(&d, VRL_XR_IDLE);
    drv_state(&d, VRL_XR_EXITING);
    CHECK(d.exited);
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_EXIT);

    drv_state(&d, VRL_XR_READY);                            /* no restart while shutting down */
    CHECK(!d.lc.running);
}

static void test_ready_with_invalid_graphics_recreates_first(void)
{
    /* resume after the GL context / swapchains became invalid: never begin on dead resources */
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    drv_state(&d, VRL_XR_STOPPING);
    drv_state(&d, VRL_XR_IDLE);
    VrLife_OnGraphicsLost(&d.lc);
    CHECK(!d.lc.graphicsValid);
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_RECREATE);
    drv_state(&d, VRL_XR_READY);
    CHECK(d.recreates == 1);                                /* rebuilt ... */
    CHECK(d.lc.graphicsValid && !d.lc.running);             /* ... and not yet begun */
    drv_state(&d, VRL_XR_IDLE);
    drv_state(&d, VRL_XR_READY);
    CHECK(d.lc.running);                                    /* the next READY begins the new session */
}

static void test_frame_results(void)
{
    vrLifecycle_t lc;
    VrLife_Init(&lc);
    lc.running = 1;

    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_SUCCESS) == VRL_FRAME_OK);
    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_FRAME_DISCARDED) == VRL_FRAME_OK);
    CHECK(lc.running);

    CHECK(VrLife_OnFrameResult(&lc, -2) == VRL_FRAME_SKIP);                 /* runtime failure: skip, keep state */
    CHECK(lc.running);

    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_SESSION_LOSS_PENDING) == VRL_FRAME_RECREATE);
    CHECK(lc.sessionLost);

    VrLife_Init(&lc);
    lc.running = 1;
    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_ERROR_SESSION_NOT_RUNNING) == VRL_FRAME_SKIP);
    CHECK(!lc.running);                                                     /* resynchronised */
    CHECK(!VrLife_CanRender(&lc));

    VrLife_Init(&lc);
    lc.running = 1;
    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_ERROR_SESSION_LOST) == VRL_FRAME_RECREATE);
    CHECK(lc.sessionLost && !lc.running);

    VrLife_Init(&lc);
    lc.running = 1;
    CHECK(VrLife_OnFrameResult(&lc, VRL_XR_ERROR_INSTANCE_LOST) == VRL_FRAME_EXIT);
    CHECK(lc.exitRequested);
}

static void test_no_frames_submitted_while_not_running(void)
{
    driver_t d;
    int i;
    drv_init(&d);
    for (i = 0; i < 10; i++) drv_frame(&d);                 /* before READY */
    CHECK(d.framesSubmitted == 0);
    bring_up(&d);
    for (i = 0; i < 10; i++) drv_frame(&d);
    CHECK(d.framesSubmitted == 10);
    drv_state(&d, VRL_XR_STOPPING);
    for (i = 0; i < 10; i++) drv_frame(&d);                 /* asleep */
    CHECK(d.framesSubmitted == 10);
}

static void test_surface_identity(void)
{
    vrLifecycle_t lc;
    VrLife_Init(&lc);

    CHECK(VrLife_OnSurface(&lc, 111) == 1);                 /* created */
    CHECK(VrLife_OnSurface(&lc, 111) == 0);                 /* surfaceChanged with the same surface: nothing to do */
    CHECK(VrLife_OnSurface(&lc, 111) == 0);
    CHECK(VrLife_OnSurface(&lc, 0) == 1);                   /* destroyed */
    CHECK(VrLife_OnSurface(&lc, 0) == 0);                   /* destroyed twice */
    CHECK(VrLife_OnSurface(&lc, 222) == 1);                 /* recreated: a new identity */
    CHECK(VrLife_OnSurface(&lc, 333) == 1);                 /* destroy callback missing: still detected */
}

static void test_surface_does_not_gate_rendering(void)
{
    /* The XR session renders into swapchain images on a pbuffer-based EGL context; the Android
     * window is not part of it.  Missing or delayed Surface callbacks must not block frames. */
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    VrLife_OnSurface(&d.lc, 0);
    CHECK(VrLife_CanRender(&d.lc));
    VrLife_OnAndroidPause(&d.lc);
    CHECK(VrLife_CanRender(&d.lc));                         /* the runtime state decides, not the Activity flag */
    VrLife_OnAndroidResume(&d.lc);
    VrLife_OnSurface(&d.lc, 5);
    CHECK(VrLife_CanRender(&d.lc));
}

static void test_android_pause_resume_flags(void)
{
    vrLifecycle_t lc;
    VrLife_Init(&lc);
    VrLife_OnAndroidResume(&lc);
    CHECK(lc.androidResumed);
    VrLife_OnAndroidPause(&lc);
    CHECK(!lc.androidResumed);
    VrLife_OnAndroidPause(&lc);                             /* repeated callbacks are harmless */
    CHECK(!lc.androidResumed);
}

static void test_activity_destroyed_ends_waiting(void)
{
    driver_t d;
    drv_init(&d);
    CHECK(VrLife_WaitDecision(&d.lc, 0) == VRL_WAIT_KEEP);
    CHECK(VrLife_WaitDecision(&d.lc, 1) == VRL_WAIT_EXIT);  /* never wait forever once the Activity is gone */
    bring_up(&d);
    CHECK(VrLife_WaitDecision(&d.lc, 1) == VRL_WAIT_EXIT);
}

static void test_swapchain_wait_is_bounded(void)
{
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    CHECK(VrLife_SwapchainShouldRetry(&d.lc, 0));
    CHECK(VrLife_SwapchainShouldRetry(&d.lc, VRL_SWAPCHAIN_MAX_TIMEOUTS - 1));
    CHECK(!VrLife_SwapchainShouldRetry(&d.lc, VRL_SWAPCHAIN_MAX_TIMEOUTS));   /* the old loop had no bound */
    drv_state(&d, VRL_XR_STOPPING);                                           /* headset went to sleep mid-frame */
    CHECK(!VrLife_SwapchainShouldRetry(&d.lc, 0));
}

static void test_wait_log_throttle(void)
{
    CHECK(VrLife_WaitLogDue(0, -1, 5000));                  /* first message */
    CHECK(!VrLife_WaitLogDue(1000, 0, 5000));
    CHECK(!VrLife_WaitLogDue(4999, 0, 5000));
    CHECK(VrLife_WaitLogDue(5000, 0, 5000));
    CHECK(VrLife_WaitLogDue(10001, 5000, 5000));
}

static void test_names(void)
{
    CHECK(strcmp(VrLife_StateName(VRL_XR_LOSS_PENDING), "LOSS_PENDING") == 0);
    CHECK(strcmp(VrLife_StateName(VRL_XR_EXITING), "EXITING") == 0);
    CHECK(strcmp(VrLife_StateName(-3), "INVALID") == 0);
    CHECK(strcmp(VrLife_ActionName(VRL_ACT_DESTROY_AND_RECREATE), "recreate") == 0);
}

static void test_full_day_in_the_life(void)
{
    /* sleep, system menu (visible but not focused), loss, wake, repeated: ends in a healthy state */
    driver_t d;
    drv_init(&d);
    bring_up(&d);
    drv_state(&d, VRL_XR_VISIBLE);                          /* PICO system menu on top */
    drv_state(&d, VRL_XR_FOCUSED);
    drv_state(&d, VRL_XR_VISIBLE);
    drv_state(&d, VRL_XR_SYNCHRONIZED);
    drv_state(&d, VRL_XR_STOPPING);
    drv_state(&d, VRL_XR_IDLE);
    drv_state(&d, VRL_XR_READY);
    drv_state(&d, VRL_XR_FOCUSED);
    drv_state(&d, VRL_XR_LOSS_PENDING);
    drv_state(&d, VRL_XR_IDLE);
    drv_state(&d, VRL_XR_READY);
    drv_state(&d, VRL_XR_FOCUSED);
    CHECK(VrLife_CanRender(&d.lc));
    CHECK(!d.exited && d.recreates == 1);
}

int main(void)
{
    test_initial_state();
    test_normal_startup_to_focused();
    test_standby_ready_stopping_idle_ready();
    test_repeated_pause_resume_is_balanced();
    test_duplicate_and_unexpected_events_do_not_crash();
    test_idle_while_running_resynchronises();
    test_begin_failure_is_bounded();
    test_loss_pending_recreates_the_session();
    test_loss_pending_with_unrecoverable_runtime_exits();
    test_exiting_state_exits_the_app();
    test_ready_with_invalid_graphics_recreates_first();
    test_frame_results();
    test_no_frames_submitted_while_not_running();
    test_surface_identity();
    test_surface_does_not_gate_rendering();
    test_android_pause_resume_flags();
    test_activity_destroyed_ends_waiting();
    test_swapchain_wait_is_bounded();
    test_wait_log_throttle();
    test_names();
    test_full_day_in_the_life();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
