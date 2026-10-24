/*
 * Menu pointer tests: coordinate pipeline, reference yaw, pose validity and
 * the menu placement ("snap") decision.  All production code from
 * VrPointerLogic.h and VrM2bDiag.c.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "VrPointerLogic.h"
#include "VrM2bDiag.h"

void HostDiagReset(void);
int  HostLinesContain(int channel, const char *needle);
extern int hostNow;
extern int hostLevel[2];
extern int hostLineCount[2];
extern char hostLines[2][256][320];

static int nearf(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

/* The cursor mapping as it was before M2B (reference yaw supplied by the caller). */
static float legacyU(float ctrlYaw, float refYaw)
{
    return -sinf((ctrlYaw - refYaw) * 3.14159265358979f / 180.0f) + 0.5f;
}

/* Simple hit test in 640x480 UI space (what the UI does with the cursor). */
static int hits(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void testInitialPointer(void)
{
    float u = -1.0f;
    float v = -1.0f;
    int x;
    int y;

    /* Pointing at the middle of the panel (yaw = panel yaw, pitch 15 as before). */
    CHECK(VrPointer_FromAngles(40.0f, 15.0f, 40.0f, &u, &v));
    CHECK(nearf(u, 0.5f, 1e-5f) && nearf(v, 0.5f, 1e-5f));
    VrPointer_ToUi(u, v, &x, &y);
    CHECK(x == 320 && y == 240);

    /* Same mapping as before the change: +/-30 deg is the panel edge, right = larger u. */
    CHECK(VrPointer_FromAngles(-30.0f, 15.0f, 0.0f, &u, &v));
    CHECK(nearf(u, 1.0f, 1e-5f));
    CHECK(VrPointer_FromAngles(30.0f, 15.0f, 0.0f, &u, &v));
    CHECK(nearf(u, 0.0f, 1e-5f));
    CHECK(VrPointer_FromAngles(0.0f, 60.0f, 0.0f, &u, &v));
    CHECK(nearf(v, 1.0f, 1e-5f));
}

/* The defect: cursor reference yaw and menu quad yaw were different variables. */
static void testReferenceYawConsistency(void)
{
    /* Cold start: the user happens to face stage yaw 25 deg when the menu is placed. */
    const float quadYaw = 25.0f;
    float u = 0.0f;
    float v = 0.0f;
    int x;
    int y;

    /* Aiming at the centre of the panel. */
    float legacyCursor = legacyU(quadYaw, 0.0f);          /* old static menuYaw = 0 */
    CHECK(fabsf(legacyCursor - 0.5f) > 0.3f);             /* old code: far off the centre */

    CHECK(VrPointer_FromAngles(quadYaw, 15.0f, quadYaw, &u, &v));   /* new: reference = quad yaw */
    CHECK(nearf(u, 0.5f, 1e-5f));
    VrPointer_ToUi(u, v, &x, &y);
    CHECK(hits(x, y, 250, 200, 140, 80));                 /* a centred button is hit */

    /* Old behaviour: the same aim misses that button. */
    int ox = (int)(legacyCursor * 640.0f);
    CHECK(!hits(ox, y, 250, 200, 140, 80));

    /* The result depends only on the angle between controller and panel. */
    for (float yaw = -170.0f; yaw <= 170.0f; yaw += 17.0f) {
        float u1 = 0.0f;
        float u2 = 0.0f;
        float v1 = 0.0f;
        float v2 = 0.0f;
        CHECK(VrPointer_FromAngles(yaw + 7.0f, 20.0f, yaw, &u1, &v1));
        CHECK(VrPointer_FromAngles(7.0f, 20.0f, 0.0f, &u2, &v2));
        CHECK(nearf(u1, u2, 1e-4f) && nearf(v1, v2, 1e-6f));
    }
}

static void testRejectsNonFiniteInput(void)
{
    float u = 7.0f;
    float v = 8.0f;
    const float nanv = NAN;
    const float infv = INFINITY;

    CHECK(!VrPointer_FromAngles(nanv, 0.0f, 0.0f, &u, &v));
    CHECK(!VrPointer_FromAngles(0.0f, infv, 0.0f, &u, &v));
    CHECK(!VrPointer_FromAngles(0.0f, 0.0f, nanv, &u, &v));
    CHECK(u == 7.0f && v == 8.0f);                         /* untouched */
}

static void testViewportConversion(void)
{
    int x;
    int y;

    VrPointer_ToUi(0.0f, 0.0f, &x, &y);
    CHECK(x == 0 && y == 0);
    VrPointer_ToUi(1.0f, 1.0f, &x, &y);
    CHECK(x == 640 && y == 480);
    VrPointer_ToUi(0.25f, 0.75f, &x, &y);
    CHECK(x == 160 && y == 360);
    /* Identical truncation to the old PortableMouseAbs: absx = x * 640 (float -> int). */
    for (float f = -0.3f; f < 1.3f; f += 0.0173f) {
        int ox = (int)(f * 640);
        int oy = (int)(f * 480);
        VrPointer_ToUi(f, f, &x, &y);
        CHECK(x == ox && y == oy);
    }
    /* Beyond the edge the UI hides the cursor (it clamps to +/- cursorSize itself). */
    VrPointer_ToUi(-0.1f, 1.2f, &x, &y);
    CHECK(x < 0 && y > 480);

    /* Monotonic: turning right increases x, pitching down increases y. */
    float uPrev = -1.0f;
    for (float yaw = 30.0f; yaw >= -30.0f; yaw -= 5.0f) {
        float u = 0.0f;
        float v = 0.0f;
        CHECK(VrPointer_FromAngles(yaw, 15.0f, 0.0f, &u, &v));
        CHECK(u > uPrev);
        uPrev = u;
    }
}

static void testQuaternionValidity(void)
{
    CHECK(VrPointer_QuatUsable(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(VrPointer_QuatUsable(0.0f, 0.7071068f, 0.0f, 0.7071068f));
    CHECK(VrPointer_QuatUsable(0.0f, 0.0f, 0.0f, 1.04f));            /* normalisation noise */
    CHECK(!VrPointer_QuatUsable(0.0f, 0.0f, 0.0f, 0.0f));            /* unfilled location */
    CHECK(!VrPointer_QuatUsable(0.0f, 0.0f, 0.0f, 0.5f));
    CHECK(!VrPointer_QuatUsable(NAN, 0.0f, 0.0f, 1.0f));
    CHECK(!VrPointer_QuatUsable(0.0f, INFINITY, 0.0f, 1.0f));
    CHECK(!VrPointer_QuatUsable(0.0f, 0.0f, 0.0f, 3.0f));
}

static void testFirstValidControllerPose(void)
{
    vrPoseHold_t hold;
    vrQuat_t zero = { 0, 0, 0, 0 };
    vrQuat_t good = { 0, 0.7071068f, 0, 0.7071068f };
    vrQuat_t good2 = { 0.7071068f, 0, 0, 0.7071068f };
    vrQuat_t out;

    memset(&hold, 0, sizeof(hold));

    /* No pose yet: identity, flagged unusable (never a zero quaternion). */
    CHECK(!VrPointer_HoldPose(&hold, &zero, &out));
    CHECK(out.w == 1.0f && out.x == 0.0f && out.y == 0.0f && out.z == 0.0f);

    /* First valid pose is used as is. */
    CHECK(VrPointer_HoldPose(&hold, &good, &out));
    CHECK(out.y == good.y && out.w == good.w);

    /* Tracking lost: the last valid orientation is held, not garbage. */
    CHECK(!VrPointer_HoldPose(&hold, &zero, &out));
    CHECK(out.y == good.y && out.w == good.w);
    CHECK(!VrPointer_HoldPose(&hold, &zero, &out));
    CHECK(out.y == good.y && out.w == good.w);

    /* Invalid -> valid: the new pose wins immediately. */
    CHECK(VrPointer_HoldPose(&hold, &good2, &out));
    CHECK(out.x == good2.x && out.y == 0.0f);
}

static void testPoseUsableFallback(void)
{
    int streak = 0;

    CHECK(!VrPointer_PoseUsable(0, 1, &streak));      /* flags say valid but data is garbage */
    CHECK(VrPointer_PoseUsable(1, 1, &streak));
    CHECK(streak == 0);

    /* Flags missing: distrusted for a bounded time, then trusted so a runtime
     * with unreliable flags still gets a working menu. */
    int usableAt = -1;
    for (int f = 0; f < 400; f++) {
        if (VrPointer_PoseUsable(1, 0, &streak)) {
            usableAt = f;
            break;
        }
    }
    CHECK(usableAt == VRPTR_FLAG_FALLBACK_FRAMES);

    /* Hard-invalid data is never trusted and resets the streak. */
    CHECK(!VrPointer_PoseUsable(0, 0, &streak));
    CHECK(streak == 0);
}

static int countSnaps(vrSnapState_t *s, int frames, int screen, int usable, int *firstReason)
{
    int n = 0;

    for (int i = 0; i < frames; i++) {
        int r = VrSnap_Update(s, screen, usable);
        if (r != VRSNAP_NONE) {
            if (n == 0 && firstReason) {
                *firstReason = r;
            }
            n++;
        }
    }
    return n;
}

static void testSnapWarmupAndSteadyState(void)
{
    vrSnapState_t s;
    int first = 0;

    VrSnap_Init(&s);
    /* Startup with the screen layer on (menu): follow the HMD for the warm-up frames only. */
    CHECK(countSnaps(&s, 100, 1, 1, &first) == VRPTR_SNAP_WARMUP_FRAMES);
    CHECK(first == VRSNAP_POSE_VALID);
    CHECK(countSnaps(&s, 500, 1, 1, NULL) == 0);          /* then the menu stays put */
}

static void testSnapInvalidToValidTransition(void)
{
    vrSnapState_t s;
    int first = 0;

    VrSnap_Init(&s);
    /* 150 frames without a usable HMD pose (tracking not up yet): no snap is
     * taken, so no garbage can be frozen in and no warm-up frames are wasted. */
    CHECK(countSnaps(&s, 150, 1, 0, NULL) == 0);
    /* Tracking arrives: a fresh warm-up window starts. */
    CHECK(countSnaps(&s, 200, 1, 1, &first) == VRPTR_SNAP_WARMUP_FRAMES);
    CHECK(first == VRSNAP_POSE_VALID);

    /* Tracking is lost mid-session and returns: re-snapped on return. */
    CHECK(countSnaps(&s, 20, 1, 0, NULL) == 0);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_POSE_VALID);
}

static void testSnapMenuReopenAndToggle(void)
{
    vrSnapState_t s;

    VrSnap_Init(&s);
    countSnaps(&s, 300, 1, 1, NULL);                       /* menu, settled */
    CHECK(VrSnap_Update(&s, 0, 1) == VRSNAP_SCREEN_TOGGLE);   /* game starts */
    countSnaps(&s, 300, 0, 1, NULL);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_SCREEN_TOGGLE);   /* menu reopened */
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_NONE);
    CHECK(VrSnap_Update(&s, 0, 1) == VRSNAP_SCREEN_TOGGLE);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_SCREEN_TOGGLE);
}

static void testSnapRecenter(void)
{
    vrSnapState_t s;

    VrSnap_Init(&s);
    countSnaps(&s, 300, 1, 1, NULL);
    VrSnap_RequestRecenter(&s);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_RECENTER);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_NONE);        /* one snap per recenter */

    /* Recenter while tracking is not usable stays pending and is satisfied by
     * the snap taken when tracking returns. */
    VrSnap_RequestRecenter(&s);
    CHECK(VrSnap_Update(&s, 1, 0) == VRSNAP_NONE);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_POSE_VALID);
    CHECK(VrSnap_Update(&s, 1, 1) == VRSNAP_WARMUP);      /* still inside the new warm-up */
}

/* End-to-end: cursor aligned with the panel after startup and after recenter. */
static void testPanelAlignedAfterStartupAndRecenter(void)
{
    vrSnapState_t s;
    float snapYaw = 0.0f;
    float u = 0.0f;
    float v = 0.0f;

    VrSnap_Init(&s);
    /* Headset starts facing 25 deg in stage space; frames 1..120 */
    float hmdYaw = 25.0f;
    for (int f = 0; f < 120; f++) {
        if (VrSnap_Update(&s, 1, 1) != VRSNAP_NONE) {
            snapYaw = hmdYaw;
        }
    }
    CHECK(snapYaw == 25.0f);
    CHECK(VrPointer_FromAngles(25.0f, 15.0f, snapYaw, &u, &v) && nearf(u, 0.5f, 1e-5f));

    /* Without the fix the cursor reference was 0 and aiming at the panel gave: */
    CHECK(fabsf(legacyU(25.0f, 0.0f) - 0.5f) > 0.3f);

    /* User recenters: stage space is recreated, the user now faces -70 deg. */
    hmdYaw = -70.0f;
    VrSnap_RequestRecenter(&s);
    if (VrSnap_Update(&s, 1, 1) == VRSNAP_RECENTER) {
        snapYaw = hmdYaw;
    }
    CHECK(snapYaw == -70.0f);
    CHECK(VrPointer_FromAngles(-70.0f, 15.0f, snapYaw, &u, &v) && nearf(u, 0.5f, 1e-5f));
}

/* ---- RTCWPointer diagnostics (production VrM2bDiag.c) ---- */

static void testPointerDiagEvents(void)
{
    HostDiagReset();
    VrPtrDiag_Snap("pose_valid", 25.0f, 25.0f, 0.1f, 1.6f, -0.2f, 1);
    CHECK(HostLinesContain(1, "v1 t=1000 ev=snap reason=pose_valid hmdYaw=25.0 quadYaw=25.0 pos=0.10,1.60,-0.20 screen=1"));
    CHECK(!HostLinesContain(0, "ev=snap"));                /* pointer channel only */

    /* pose: change only */
    HostDiagReset();
    VrPtrDiag_Pose("HMD", 0, 0u);
    VrPtrDiag_Pose("HMD", 0, 0u);
    VrPtrDiag_Pose("HMD", 1, 0xfu);
    VrPtrDiag_Pose("L", 1, 0xfu);
    VrPtrDiag_Pose("L", 1, 0xfu);
    CHECK(hostLineCount[1] == 3);

    /* hand: change only */
    HostDiagReset();
    VrPtrDiag_MenuHand(1);
    VrPtrDiag_MenuHand(1);
    VrPtrDiag_MenuHand(0);
    CHECK(hostLineCount[1] == 2);
    CHECK(HostLinesContain(1, "ev=hand hand=L"));

    /* recenter */
    HostDiagReset();
    VrPtrDiag_Recenter(33.0f);
    CHECK(HostLinesContain(1, "ev=recenter hmdYawBefore=33.0"));
}

static void testPointerDiagSamplesAreRateLimited(void)
{
    HostDiagReset();
    /* 10 s of 90 fps in a menu: one sample per 2 s, not per frame. */
    for (int f = 0; f < 900; f++) {
        hostNow = 1000 + f * 11;
        VrPtrDiag_Sample(25.0f, 15.0f, 25.0f, 25.0f, 0.5f, 0.5f);
    }
    CHECK(hostLineCount[1] >= 4 && hostLineCount[1] <= 6);
    CHECK(HostLinesContain(1, "ev=ptr ctrlYaw=25.0 ctrlPitch=15.0 refYaw=25.0 hmdYaw=25.0 u=0.500 v=0.500 ui=320,240"));

    /* level 2: every 250 ms; level 0: nothing */
    HostDiagReset();
    hostLevel[1] = 2;
    for (int f = 0; f < 900; f++) {
        hostNow = 1000 + f * 11;
        VrPtrDiag_Sample(25.0f, 15.0f, 25.0f, 25.0f, 0.5f, 0.5f);
    }
    CHECK(hostLineCount[1] > 20 && hostLineCount[1] <= 40);
    HostDiagReset();
    hostLevel[1] = 0;
    VrPtrDiag_Sample(25.0f, 15.0f, 25.0f, 25.0f, 0.5f, 0.5f);
    VrPtrDiag_Snap("warmup", 0, 0, 0, 0, 0, 1);
    CHECK(hostLineCount[1] == 0);
}

static void testPointerDiagOffAxisAndReject(void)
{
    HostDiagReset();
    VrPtrDiag_Sample(0.0f, 15.0f, 0.0f, 0.0f, 0.5f, 0.5f);       /* in front: no event */
    CHECK(!HostLinesContain(1, "ev=offaxis"));
    hostNow += 3000;
    VrPtrDiag_Sample(0.0f, 15.0f, 0.0f, 120.0f, 0.5f, 0.5f);     /* panel is behind: event once */
    hostNow += 3000;
    VrPtrDiag_Sample(0.0f, 15.0f, 0.0f, 121.0f, 0.5f, 0.5f);
    CHECK(HostLinesContain(1, "ev=offaxis state=1"));
    int n = 0;
    for (int i = 0; i < hostLineCount[1]; i++) {
        if (strstr(hostLines[1][i], "ev=offaxis")) {
            n++;
        }
    }
    CHECK(n == 1);
    /* Wrap-around: 359 deg vs 1 deg is only 2 deg apart. */
    HostDiagReset();
    VrPtrDiag_Sample(0.0f, 15.0f, 359.0f, 1.0f, 0.5f, 0.5f);
    CHECK(!HostLinesContain(1, "ev=offaxis"));

    HostDiagReset();
    VrPtrDiag_Rejected(NAN, 0.0f, 0.0f);
    VrPtrDiag_Rejected(NAN, 0.0f, 0.0f);
    CHECK(hostLineCount[1] == 1);
}

void RunPointerTests(void)
{
    testInitialPointer();
    testReferenceYawConsistency();
    testRejectsNonFiniteInput();
    testViewportConversion();
    testQuaternionValidity();
    testFirstValidControllerPose();
    testPoseUsableFallback();
    testSnapWarmupAndSteadyState();
    testSnapInvalidToValidTransition();
    testSnapMenuReopenAndToggle();
    testSnapRecenter();
    testPanelAlignedAfterStartupAndRecenter();
    testPointerDiagEvents();
    testPointerDiagSamplesAreRateLimited();
    testPointerDiagOffAxisAndReject();
}
