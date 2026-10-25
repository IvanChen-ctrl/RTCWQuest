/*
 * Host tests for the mercenary squad logic: adaptive follow / catch-up / recovery policy
 * (g_vrmove.h) and the multi-companion bookkeeping (g_vrsquad.h).
 *
 * These test the exact functions that run inside the game module.  They do NOT exercise the
 * engine glue in g_merc.c (traces, AAS, the stock AI) and prove nothing about behaviour on a
 * headset.  The kinematic simulations below are models of the decision logic, not of the engine.
 *
 * Build and run: make -C tests/vr_squad test
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "g_vrmove.h"
#include "g_vrtactics.h"
#include "g_vrsquad.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* ===================== movement modes ===================== */

static vrMoveIn_t move_in(float euclid, float path, int los, float pspeed)
{
    vrMoveIn_t in;

    memset(&in, 0, sizeof(in));
    in.euclid = euclid;
    in.pathDist = path;
    in.hasLOS = los;
    in.playerSpeed = pspeed;
    return in;
}

static void test_effective_distance(void)
{
    CHECK(VrMove_EffectiveDist(200, -1) == 200);                 /* unknown path: straight line */
    CHECK(VrMove_EffectiveDist(200, 150) == 200);                /* never shorter than straight */
    CHECK(VrMove_EffectiveDist(200, 900) == 900);                /* the long way round counts   */
    CHECK(VrMove_EffectiveDist(200, VRMOVE_UNREACHABLE_DIST) == VRMOVE_UNREACHABLE_DIST);
}

static void test_path_distance_from_travel_time(void)
{
    CHECK(VrMove_PathDistFromTravel(0, 1, 123) == 123);          /* same area */
    CHECK(VrMove_PathDistFromTravel(0, 0, 123) >= VRMOVE_UNREACHABLE_DIST);
    CHECK(VrMove_PathDistFromTravel(-5, 0, 123) >= VRMOVE_UNREACHABLE_DIST);
    CHECK(fabsf(VrMove_PathDistFromTravel(200, 0, 0) - 600.0f) < 0.01f);    /* 2 s at 300 u/s */
}

static void test_mode_near_stays_near_when_close(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(150, 160, 1, 250);

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &in, 100) == VRMOVE_NEAR);
    in = move_in(VRMOVE_RUN_ENTER_MOVING - 1, -1, 1, 250);
    CHECK(VrMove_Update(&m, &in, 200) == VRMOVE_NEAR);
}

static void test_mode_run_thresholds_depend_on_player_motion(void)
{
    vrMove_t m;
    vrMoveIn_t moving = move_in(VRMOVE_RUN_ENTER_MOVING + 5, -1, 1, 250);
    vrMoveIn_t still = move_in(VRMOVE_RUN_ENTER_MOVING + 5, -1, 1, 0);

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &moving, 100) == VRMOVE_RUN);        /* player walks away: run */
    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &still, 100) == VRMOVE_NEAR);        /* player stands: no hurry */
    still.euclid = VRMOVE_RUN_ENTER_STILL + 5;
    CHECK(VrMove_Update(&m, &still, 200) == VRMOVE_RUN);
}

static void test_mode_catchup_thresholds(void)
{
    vrMove_t m;
    vrMoveIn_t far_in = move_in(VRMOVE_CATCHUP_ENTER + 5, -1, 1, 250);
    vrMoveIn_t nolos = move_in(150, VRMOVE_CATCHUP_ENTER_NOLOS + 5, 0, 250);   /* around a corner */

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &far_in, 100) == VRMOVE_CATCHUP);    /* upgrade is immediate, skips RUN */
    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &nolos, 100) == VRMOVE_CATCHUP);     /* short straight line, long path, no LOS */
    nolos.hasLOS = 1;
    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &nolos, 100) == VRMOVE_RUN);         /* same path with LOS: only a run */
}

static void test_mode_unreachable_counts_as_catchup(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(100, VRMOVE_UNREACHABLE_DIST, 1, 0);

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &in, 100) == VRMOVE_CATCHUP);
}

/* The stock bug: around a corner (small straight distance, no LOS) the companion used to stop. */
static void test_corner_case_is_not_ignored(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(200, 600, 0, 280);          /* 200 straight, 600 by path, no LOS */

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &in, 100) == VRMOVE_CATCHUP);
    CHECK(VrMove_ForcesChase(m.mode));
}

static void test_mode_hysteresis_and_dwell(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(VRMOVE_CATCHUP_ENTER + 50, -1, 1, 250);

    VrMove_Reset(&m, 0);
    CHECK(VrMove_Update(&m, &in, 0) == VRMOVE_CATCHUP);

    /* back inside the exit threshold, but before the dwell time: stay */
    in = move_in(VRMOVE_CATCHUP_EXIT - 20, -1, 1, 250);
    CHECK(VrMove_Update(&m, &in, VRMOVE_MIN_DWELL_MS - 1) == VRMOVE_CATCHUP);
    CHECK(VrMove_Update(&m, &in, VRMOVE_MIN_DWELL_MS) == VRMOVE_RUN);
    /* between the exit and the entry threshold it does not flip back */
    in = move_in(VRMOVE_CATCHUP_ENTER - 20, -1, 1, 250);
    CHECK(VrMove_Update(&m, &in, VRMOVE_MIN_DWELL_MS + 100) == VRMOVE_RUN);
    /* run -> near only below the exit threshold and after the dwell */
    in = move_in(VRMOVE_RUN_EXIT + 10, -1, 1, 250);
    CHECK(VrMove_Update(&m, &in, VRMOVE_MIN_DWELL_MS * 3) == VRMOVE_RUN);
    in = move_in(VRMOVE_RUN_EXIT - 10, -1, 1, 250);
    CHECK(VrMove_Update(&m, &in, VRMOVE_MIN_DWELL_MS * 3) == VRMOVE_NEAR);
}

static void test_mode_no_los_catchup_exit_is_stricter(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(100, 700, 0, 250);

    VrMove_Reset(&m, 0);
    VrMove_Update(&m, &in, 0);
    CHECK(m.mode == VRMOVE_CATCHUP);
    in = move_in(300, 300, 0, 250);                      /* 300: fine with LOS, not without */
    CHECK(VrMove_Update(&m, &in, 5000) == VRMOVE_CATCHUP);
    in = move_in(300, 300, 1, 250);
    CHECK(VrMove_Update(&m, &in, 6000) == VRMOVE_RUN);
}

static void test_mode_suppressed_drops_to_near_at_once(void)
{
    vrMove_t m;
    vrMoveIn_t in = move_in(900, 900, 1, 250);

    VrMove_Reset(&m, 0);
    VrMove_Update(&m, &in, 0);
    CHECK(m.mode == VRMOVE_CATCHUP);
    in.suppressed = 1;                                   /* cover / flank / hold / script */
    CHECK(VrMove_Update(&m, &in, 10) == VRMOVE_NEAR);    /* no dwell for suppression */
}

/* No mode flapping: jitter of +-30 units around every threshold changes mode only a few times. */
static void test_mode_does_not_flap_on_distance_jitter(void)
{
    float centers[4] = { VRMOVE_RUN_ENTER_MOVING, VRMOVE_RUN_EXIT, VRMOVE_CATCHUP_ENTER, VRMOVE_CATCHUP_EXIT };
    int c, i;

    for (c = 0; c < 4; c++) {
        vrMove_t m;
        int changes = 0, last;

        VrMove_Reset(&m, 0);
        last = m.mode;
        for (i = 0; i < 400; i++) {                       /* 400 ticks of 100 ms */
            float d = centers[c] + ((i % 2) ? 30.0f : -30.0f);
            vrMoveIn_t in = move_in(d, -1, 1, 250);

            VrMove_Update(&m, &in, i * 100);
            if (m.mode != last) {
                changes++;
                last = m.mode;
            }
        }
        CHECK(changes <= 3);
    }
}

static void test_speed_floor_per_mode(void)
{
    CHECK(VrMove_SpeedFloor(VRMOVE_NEAR, 220) == 0.0f);                  /* stock value untouched */
    CHECK(VrMove_SpeedFloor(VRMOVE_RUN, 220) == VRMOVE_RUN_SCALE);
    /* catch-up scales the run speed to the player's top speed: 300 / 220 */
    CHECK(fabsf(VrMove_SpeedFloor(VRMOVE_CATCHUP, 220) - 300.0f / 220.0f) < 0.001f);
    CHECK(VrMove_SpeedFloor(VRMOVE_CATCHUP, 300) == VRMOVE_RUN_SCALE);    /* never below the stock cap */
    CHECK(VrMove_SpeedFloor(VRMOVE_CATCHUP, 100) == VRMOVE_CATCHUP_SCALE_MAX);   /* slow animation: capped */
    CHECK(VrMove_SpeedFloor(VRMOVE_CATCHUP, 10) == VRMOVE_RUN_SCALE);     /* implausible data: no over-boost */
    CHECK(VrMove_SpeedFloor(VRMOVE_CATCHUP, 0) == VRMOVE_RUN_SCALE);
    CHECK(VrMove_SpeedFloor(99, 220) == 0.0f);
}

static void test_chase_flags(void)
{
    CHECK(!VrMove_ForcesChase(VRMOVE_NEAR));
    CHECK(VrMove_ForcesChase(VRMOVE_RUN));
    CHECK(VrMove_ForcesChase(VRMOVE_CATCHUP));
    CHECK(VrMove_ChaseReach(VRMOVE_CATCHUP) < VrMove_ChaseReach(VRMOVE_RUN));
}

/* Model of the stock cap versus the policy.  Companion top speed in units/s, from the stock formula:
 * speedScale x RUN, bi.speed = min(400, 400 x speed / 300), forwardmove = bi.speed x 127/400, ps.speed 320.
 * The player runs at 300 u/s.  This models the decision logic only, not the engine. */
static float companion_speed(float scale, float runSpeed)
{
    float speed = scale * runSpeed;
    float bi = 400.0f * (speed / 300.0f);

    if (bi > 400.0f) {
        bi = 400.0f;
    }
    return (bi * 127.0f / 400.0f) / 127.0f * 320.0f;
}

static void test_model_stock_speed_is_below_the_player(void)
{
    /* this is the measured-from-source cause of "falls behind": 73% / 88% of the player's top speed */
    CHECK(fabsf(companion_speed(1.0f, 220) - 234.7f) < 1.0f);
    CHECK(fabsf(companion_speed(1.2f, 220) - 281.6f) < 1.0f);
    CHECK(companion_speed(1.2f, 220) < 300.0f);
    CHECK(companion_speed(VrMove_SpeedFloor(VRMOVE_CATCHUP, 220), 220) >= 300.0f);
}

static float simulate_follow(int usePolicy, float runSpeed, float *maxDist)
{
    vrMove_t m;
    float player = 0.0f, comp = 0.0f;
    int t;

    VrMove_Reset(&m, 0);
    *maxDist = 0.0f;
    for (t = 0; t < 600; t++) {                                  /* 60 s in 100 ms ticks */
        float d, scale = 1.0f;
        vrMoveIn_t in;

        player += 300.0f * 0.1f;                                 /* the player runs straight ahead */
        d = player - comp;
        if (usePolicy) {
            float floor_;

            in = move_in(d, -1, 1, 300.0f);
            VrMove_Update(&m, &in, t * 100);
            floor_ = VrMove_SpeedFloor(m.mode, runSpeed);
            if (floor_ > scale) {
                scale = floor_;
            }
        }
        /* the stock chase only starts beyond 256 units; it stops again below 240 */
        if (usePolicy ? (m.mode != VRMOVE_NEAR || d > 256.0f) : d > 256.0f) {
            comp += companion_speed(scale, runSpeed) * 0.1f;
        }
        if (d > *maxDist) {
            *maxDist = d;
        }
    }
    return player - comp;
}

static void test_model_policy_keeps_up_where_stock_falls_behind(void)
{
    float maxStock, maxPolicy;
    float endStock = simulate_follow(0, 220, &maxStock);
    float endPolicy = simulate_follow(1, 220, &maxPolicy);

    CHECK(endStock > 1000.0f);                  /* stock: 60 s of full-speed running leaves it far behind */
    CHECK(endPolicy < 700.0f);                  /* policy: catches up and holds the distance              */
    CHECK(maxPolicy < maxStock);
    CHECK(endPolicy < endStock / 2);
}

/* ===================== stuck recovery ===================== */

static void test_recovery_ladder_escalates_and_waits(void)
{
    vrRecover_t r;
    float pos[3] = { 10, 20, 30 };
    int now = 0, seen[4] = { 0, 0, 0, 0 }, a, steps = 0;

    VrRecover_Reset(&r, pos, 0);
    for (now = 100; now < 60000 && steps < 20; now += 100) {
        a = VrRecover_Step(&r, pos, 1, now);                      /* never moves */
        if (a != VRREC_NONE) {
            seen[a]++;
            steps++;
        }
    }
    CHECK(seen[VRREC_REPATH] >= 1);
    CHECK(seen[VRREC_TRAIL] >= 1);
    CHECK(seen[VRREC_WAIT] >= 1);
    /* the first three steps are in ladder order */
    {
        vrRecover_t q;
        int order[3], n = 0;

        VrRecover_Reset(&q, pos, 0);
        for (now = 100; now < 30000 && n < 3; now += 100) {
            a = VrRecover_Step(&q, pos, 1, now);
            if (a != VRREC_NONE) {
                order[n++] = a;
            }
        }
        CHECK(n == 3 && order[0] == VRREC_REPATH && order[1] == VRREC_TRAIL && order[2] == VRREC_WAIT);
    }
}

static void test_recovery_step_timing(void)
{
    vrRecover_t r;
    float pos[3] = { 0, 0, 0 };
    int first = -1, second = -1, now;

    VrRecover_Reset(&r, pos, 0);
    for (now = 0; now < 20000; now += 50) {
        int a = VrRecover_Step(&r, pos, 1, now);

        if (a != VRREC_NONE) {
            if (first < 0) {
                first = now;
            } else if (second < 0) {
                second = now;
            }
        }
    }
    CHECK(first >= VRMOVE_STUCK_MS);                              /* not before the stuck window */
    CHECK(second - first >= VRMOVE_RECOVER_GAP_MS);               /* never spammed */
}

static void test_recovery_not_triggered_while_moving_or_idle(void)
{
    vrRecover_t r;
    float pos[3] = { 0, 0, 0 };
    int now, a, steps = 0;

    VrRecover_Reset(&r, pos, 0);
    for (now = 100; now < 30000; now += 100) {                    /* walks 60 units per second */
        pos[0] = now * 0.06f;
        a = VrRecover_Step(&r, pos, 1, now);
        steps += (a != VRREC_NONE);
    }
    CHECK(steps == 0);

    VrRecover_Reset(&r, pos, 0);
    for (now = 100; now < 30000; now += 100) {                    /* standing still but not travelling */
        a = VrRecover_Step(&r, pos, 0, now);
        steps += (a != VRREC_NONE);
    }
    CHECK(steps == 0);
}

static void test_recovery_waits_then_starts_a_new_episode(void)
{
    vrRecover_t r;
    float pos[3] = { 0, 0, 0 };
    int now, a, waits = 0, repaths = 0, lastWait = -1;

    VrRecover_Reset(&r, pos, 0);
    for (now = 50; now < 120000; now += 50) {
        a = VrRecover_Step(&r, pos, 1, now);
        if (a == VRREC_WAIT) {
            waits++;
            lastWait = now;
        } else if (a == VRREC_REPATH) {
            repaths++;
        }
    }
    CHECK(waits >= 2);                                            /* episodes repeat, they do not stop forever */
    CHECK(repaths == waits || repaths == waits + 1);              /* each episode restarts at step 1 */
    /* total ladder steps are bounded by time: at most one per gap */
    CHECK(waits * 3 <= 120000 / VRMOVE_RECOVER_GAP_MS);
    CHECK(lastWait > 0);
}

static void test_recovery_forgets_old_failures_after_good_movement(void)
{
    vrRecover_t r;
    float pos[3] = { 0, 0, 0 };
    int now, a;

    VrRecover_Reset(&r, pos, 0);
    for (now = 50; now < 6000; now += 50) {                       /* one step happens (REPATH) */
        a = VrRecover_Step(&r, pos, 1, now);
        if (a == VRREC_REPATH) {
            break;
        }
    }
    CHECK(r.attempts == 1);
    for (; now < 30000 && r.attempts; now += 100) {               /* then it moves well for a while */
        pos[0] += 10.0f;
        VrRecover_Step(&r, pos, 1, now);
    }
    CHECK(r.attempts == 0);
}

/* ===================== breadcrumb trail ===================== */

static void test_trail_records_only_real_movement(void)
{
    vrTrail_t t;
    float p[3] = { 0, 0, 0 };

    VrTrail_Reset(&t);
    CHECK(VrTrail_Add(&t, p, 0) == 1);                            /* first crumb */
    p[0] = 50;
    CHECK(VrTrail_Add(&t, p, 2000) == 0);                         /* too close */
    p[0] = 300;
    CHECK(VrTrail_Add(&t, p, 100) == 0);                          /* too soon */
    CHECK(VrTrail_Add(&t, p, 2000) == 1);
    CHECK(t.count == 2);
}

static void test_trail_ring_keeps_newest(void)
{
    vrTrail_t t;
    float p[3] = { 0, 0, 0 };
    int i;

    VrTrail_Reset(&t);
    for (i = 0; i < VRTRAIL_MAX + 5; i++) {
        p[0] = (float)(i * 200);
        VrTrail_Add(&t, p, i * 1000);
    }
    CHECK(t.count == VRTRAIL_MAX);
    CHECK(t.pos[VrTrail_Index(&t, 0)][0] == (VRTRAIL_MAX + 4) * 200.0f);      /* newest first */
    CHECK(t.pos[VrTrail_Index(&t, 1)][0] == (VRTRAIL_MAX + 3) * 200.0f);
    CHECK(VrTrail_Index(&t, VRTRAIL_MAX) == -1);
    CHECK(VrTrail_Index(&t, -1) == -1);
}

static void test_trail_pick(void)
{
    vrTrail_t t;
    float p[3] = { 0, 0, 0 };
    int travel[VRTRAIL_MAX] = { 0 }, far_[VRTRAIL_MAX] = { 0 }, i;

    VrTrail_Reset(&t);
    for (i = 0; i < 5; i++) {
        p[0] = (float)(i * 200);
        VrTrail_Add(&t, p, 1000 + i * 1000);
    }
    /* nothing reachable */
    CHECK(VrTrail_Pick(&t, 6000, travel, far_) == -1);
    /* only the 3rd newest is reachable and far enough */
    travel[2] = 300; far_[2] = 1;
    CHECK(VrTrail_Pick(&t, 6000, travel, far_) == 2);
    /* a nearer, newer one that is reachable but too close is skipped */
    travel[0] = 50; far_[0] = 0;
    CHECK(VrTrail_Pick(&t, 6000, travel, far_) == 2);
    travel[0] = 50; far_[0] = 1;
    CHECK(VrTrail_Pick(&t, 6000, travel, far_) == 0);              /* newest usable wins */
    /* stale crumbs are not used */
    CHECK(VrTrail_Pick(&t, 6000 + VRMOVE_TRAIL_MAX_MS, travel, far_) == -1);
}


/* ===================== squad bookkeeping ===================== */

static void test_squad_limit(void)
{
    CHECK(VrSquad_Limit(0) == VRSQUAD_DEFAULT_LIMIT);            /* default for the first hardware test: 2 */
    CHECK(VrSquad_Limit(-3) == VRSQUAD_DEFAULT_LIMIT);
    CHECK(VrSquad_Limit(1) == 1);
    CHECK(VrSquad_Limit(2) == 2);
    CHECK(VrSquad_Limit(3) == 3);
    CHECK(VrSquad_Limit(4) == 4);
    CHECK(VrSquad_Limit(5) == VRMERC_MAX_SQUAD);                 /* never above the instance array */
    CHECK(VrSquad_Limit(1000) == VRMERC_MAX_SQUAD);
    CHECK(VRSQUAD_DEFAULT_LIMIT == 2);
}

static void test_squad_can_add(void)
{
    CHECK(VrSquad_CanAdd(0, 0, 2) == VRSQ_OK);
    CHECK(VrSquad_CanAdd(1, 0, 2) == VRSQ_OK);
    CHECK(VrSquad_CanAdd(2, 0, 2) == VRSQ_FULL);                  /* clear refusal, not a silent failure */
    CHECK(VrSquad_CanAdd(1, 1, 2) == VRSQ_PENDING);               /* one recruit at a time */
    CHECK(VrSquad_CanAdd(2, 1, 2) == VRSQ_PENDING);
    CHECK(VrSquad_CanAdd(3, 0, 4) == VRSQ_OK);
    CHECK(VrSquad_CanAdd(4, 0, 4) == VRSQ_FULL);
    CHECK(VrSquad_CanAdd(4, 0, 99) == VRSQ_NO_INSTANCE);          /* a bad limit cannot overrun the array */
}

static void test_squad_free_instance(void)
{
    int used[VRMERC_MAX_SQUAD] = { 0, 0, 0, 0 };

    CHECK(VrSquad_PickFreeInstance(used, VRMERC_MAX_SQUAD) == 0);
    used[0] = 1;
    CHECK(VrSquad_PickFreeInstance(used, VRMERC_MAX_SQUAD) == 1);
    used[1] = 1; used[3] = 1;
    CHECK(VrSquad_PickFreeInstance(used, VRMERC_MAX_SQUAD) == 2);
    used[2] = 1;
    CHECK(VrSquad_PickFreeInstance(used, VRMERC_MAX_SQUAD) == -1);
    used[0] = 0;                                                  /* a dismissed soldier frees its record */
    CHECK(VrSquad_PickFreeInstance(used, VRMERC_MAX_SQUAD) == 0);
}

static void test_squad_unique_identity(void)
{
    int i, j;

    for (i = 0; i < VRMERC_MAX_SQUAD; i++) {
        CHECK(VrSquad_IdxFromName(VrSquad_AiName(i)) == i);       /* round trip */
        for (j = i + 1; j < VRMERC_MAX_SQUAD; j++) {
            CHECK(strcmp(VrSquad_AiName(i), VrSquad_AiName(j)) != 0);
        }
    }
    CHECK(VrSquad_IdxFromName(NULL) == -1);
    CHECK(VrSquad_IdxFromName("") == -1);
    CHECK(VrSquad_IdxFromName("vr_merc_") == -1);
    CHECK(VrSquad_IdxFromName("vr_merc_0") == -1);
    CHECK(VrSquad_IdxFromName("vr_merc_5") == -1);
    CHECK(VrSquad_IdxFromName("vr_merc_12") == -1);
    CHECK(VrSquad_IdxFromName("vr_merc_1x") == -1);
    CHECK(VrSquad_IdxFromName("american") == -1);                 /* a story character is not ours */
    CHECK(VrSquad_IdxFromName("Vr_merc_1") == -1);
    CHECK(strcmp(VrSquad_AiName(-1), "") == 0);
    CHECK(strcmp(VrSquad_AiName(VRMERC_MAX_SQUAD), "") == 0);
}

static void test_squad_selection(void)
{
    int mask;
    int live = 0x5;                                               /* companions 1 and 3 */

    CHECK(VrSquad_ResolveSelection("", 0, live, &mask) == VRSQ_SEL_OK && mask == 0x5);       /* default: all */
    CHECK(VrSquad_ResolveSelection("all", 2, live, &mask) == VRSQ_SEL_OK && mask == 0x5);    /* explicit beats cvar */
    CHECK(VrSquad_ResolveSelection("ALL", 0, live, &mask) == VRSQ_SEL_OK && mask == 0x5);
    CHECK(VrSquad_ResolveSelection("1", 0, live, &mask) == VRSQ_SEL_OK && mask == 0x1);
    CHECK(VrSquad_ResolveSelection("3", 0, live, &mask) == VRSQ_SEL_OK && mask == 0x4);
    CHECK(VrSquad_ResolveSelection("2", 0, live, &mask) == VRSQ_SEL_NO_SUCH && mask == 0);   /* not in the squad */
    CHECK(VrSquad_ResolveSelection("4", 0, live, &mask) == VRSQ_SEL_NO_SUCH);
    CHECK(VrSquad_ResolveSelection("5", 0, live, &mask) == VRSQ_SEL_BAD_ARG);
    CHECK(VrSquad_ResolveSelection("0", 0, live, &mask) == VRSQ_SEL_BAD_ARG);
    CHECK(VrSquad_ResolveSelection("12", 0, live, &mask) == VRSQ_SEL_BAD_ARG);
    CHECK(VrSquad_ResolveSelection("x", 0, live, &mask) == VRSQ_SEL_BAD_ARG);
    CHECK(VrSquad_ResolveSelection("", 3, live, &mask) == VRSQ_SEL_OK && mask == 0x4);       /* notebook selector */
    CHECK(VrSquad_ResolveSelection("", 2, live, &mask) == VRSQ_SEL_NO_SUCH);                 /* selected one has died */
    CHECK(VrSquad_ResolveSelection("", 9, live, &mask) == VRSQ_SEL_OK && mask == 0x5);       /* garbage selector = all */
    CHECK(VrSquad_ResolveSelection("all", 0, 0, &mask) == VRSQ_SEL_NONE_ALIVE && mask == 0);
    CHECK(VrSquad_ResolveSelection("", 0, 0, &mask) == VRSQ_SEL_NONE_ALIVE);
    CHECK(VrSquad_ResolveSelection("1", 0, 0, &mask) == VRSQ_SEL_NO_SUCH);
    CHECK(VrSquad_ResolveSelection("", 0, 0xFF, &mask) == VRSQ_SEL_OK && mask == 0xF);       /* extra bits ignored */
}

static void test_squad_rank_and_count(void)
{
    CHECK(VrSquad_CountBits(0) == 0);
    CHECK(VrSquad_CountBits(0xF) == 4);
    CHECK(VrSquad_Rank(0x5, 0) == 0);
    CHECK(VrSquad_Rank(0x5, 2) == 1);                             /* companion 3 is the 2nd living soldier */
    CHECK(VrSquad_Rank(0xF, 3) == 3);
    CHECK(VrSquad_Rank(0x0, 3) == 0);
    CHECK(VrSquad_WishAfterRemoval(3) == 2);
    CHECK(VrSquad_WishAfterRemoval(1) == 0);
    CHECK(VrSquad_WishAfterRemoval(0) == 0);
}

/* ===================== formation ===================== */

static float dist3(const float *a, const float *b)
{
    return sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
}

static void test_formation_roles_are_separate_and_behind(void)
{
    float player[3] = { 100, -50, 20 };
    int yaw, narrow, a, b;

    for (narrow = 0; narrow < 2; narrow++) {
        for (yaw = 0; yaw < 360; yaw += 30) {
            float slots[VRMERC_MAX_SQUAD][3], fx, fy, rx, ry;

            VrTac_YawBasis((float)yaw, &fx, &fy, &rx, &ry);
            for (a = 0; a < VRMERC_MAX_SQUAD; a++) {
                VrSquad_Slot(player, (float)yaw, a, narrow, slots[a]);
                CHECK((slots[a][0] - player[0]) * fx + (slots[a][1] - player[1]) * fy < 0.0f);   /* behind */
                CHECK(VrTac_RepositionReason(slots[a], player, fx, fy) == VRTAC_REPOS_NONE);     /* not in the way */
                CHECK(slots[a][2] == player[2]);
            }
            for (a = 0; a < VRMERC_MAX_SQUAD; a++) {
                for (b = a + 1; b < VRMERC_MAX_SQUAD; b++) {
                    CHECK(dist3(slots[a], slots[b]) > VRSQUAD_MIN_SEPARATION * (narrow ? 1.3f : 1.5f));   /* never stacked */
                }
            }
        }
    }
}

static void test_formation_left_right_roles(void)
{
    float player[3] = { 0, 0, 0 }, l[3], r[3];

    VrSquad_Slot(player, 0.0f, VRSQ_ROLE_LEFT, 0, l);             /* player faces +x: left is +y */
    VrSquad_Slot(player, 0.0f, VRSQ_ROLE_RIGHT, 0, r);
    CHECK(l[1] > 0.0f && r[1] < 0.0f);
    CHECK(fabsf(l[0] - r[0]) < 0.01f);                            /* same depth */
    CHECK(fabsf(l[1] + r[1]) < 0.01f);                            /* mirror images */
}

static void test_formation_narrow_is_single_file(void)
{
    float player[3] = { 0, 0, 0 }, s[VRMERC_MAX_SQUAD][3];
    int i;

    for (i = 0; i < VRMERC_MAX_SQUAD; i++) {
        VrSquad_Slot(player, 0.0f, i, 1, s[i]);
        CHECK(fabsf(s[i][1]) < 0.01f);                            /* on the player's axis */
        if (i > 0) {
            CHECK(s[i][0] < s[i - 1][0] - 95.0f);                 /* strictly behind the previous one, spaced */
        }
    }
    CHECK(VrSquad_IsNarrow(80, 90));
    CHECK(!VrSquad_IsNarrow(150, 150));
    CHECK(VrSquad_IsNarrow(0, 0));
    CHECK(!VrSquad_IsNarrow(VRSQUAD_NARROW_WIDTH, 0));            /* threshold is exclusive */
}

static void test_formation_position_need(void)
{
    /* two soldiers stand on each other: only the higher number steps away */
    CHECK(VrSquad_PositionNeed(1, 0, 0, 0, 30) == VRSQ_POS_SEPARATE);
    CHECK(VrSquad_PositionNeed(0, 0, 0, 1, 30) == VRSQ_POS_NONE);           /* mate has a higher number: stay */
    CHECK(VrSquad_PositionNeed(1, 0, 0, 0, VRSQUAD_MIN_SEPARATION) == VRSQ_POS_NONE);
    /* player stood still, soldier far from its slot: go to it */
    CHECK(VrSquad_PositionNeed(0, VRSQUAD_SLOT_RADIUS + 10, VRSQUAD_STILL_MS, -1, 0) == VRSQ_POS_SLOT);
    CHECK(VrSquad_PositionNeed(0, VRSQUAD_SLOT_RADIUS + 10, VRSQUAD_STILL_MS - 1, -1, 0) == VRSQ_POS_NONE);
    CHECK(VrSquad_PositionNeed(0, VRSQUAD_SLOT_RADIUS - 10, 99999, -1, 0) == VRSQ_POS_NONE);
    /* separation wins over the slot rule */
    CHECK(VrSquad_PositionNeed(2, 400, 5000, 0, 20) == VRSQ_POS_SEPARATE);
}

static void test_formation_settles_without_ping_pong(void)
{
    /* a soldier standing in its slot never asks to move; separation between two soldiers resolves in one step */
    float player[3] = { 0, 0, 0 }, slot0[3], slot1[3];

    VrSquad_Slot(player, 0.0f, 0, 0, slot0);
    VrSquad_Slot(player, 0.0f, 1, 0, slot1);
    CHECK(VrSquad_PositionNeed(0, dist3(slot0, slot0), 9999, -1, 0) == VRSQ_POS_NONE);
    CHECK(dist3(slot0, slot1) >= VRSQUAD_MIN_SEPARATION);
    CHECK(VrSquad_PositionNeed(1, 0, 9999, 0, dist3(slot0, slot1)) == VRSQ_POS_NONE);
}

static void test_spawn_clear(void)
{
    float mates[6] = { 0, 0, 0, 200, 0, 0 };
    float a[3] = { 30, 0, 0 }, b[3] = { 100, 0, 0 }, c[3] = { 200, 20, 0 };

    CHECK(!VrSquad_SpawnClear(a, mates, 2));                      /* on top of mate 1 */
    CHECK(VrSquad_SpawnClear(b, mates, 2));
    CHECK(!VrSquad_SpawnClear(c, mates, 2));                      /* on top of mate 2 */
    CHECK(VrSquad_SpawnClear(a, mates, 0));                       /* nobody else: fine */
}

/* ===================== independent state ===================== */

typedef struct {
    int id;
    vrMove_t move;
    vrRecover_t rec;
    vrTacDmgLog_t dmg;
    int target;
    int targetSince;
} simSoldier_t;

static void test_instances_are_independent(void)
{
    simSoldier_t sq[3];
    float pos[3] = { 0, 0, 0 };
    int i, t;

    for (i = 0; i < 3; i++) {
        memset(&sq[i], 0, sizeof(sq[i]));
        sq[i].id = i + 1;
        VrMove_Reset(&sq[i].move, 0);
        VrRecover_Reset(&sq[i].rec, pos, 0);
        VrTac_DmgReset(&sq[i].dmg);
        sq[i].target = -1;
    }
    /* soldier 2 falls far behind, soldier 3 gets shot, soldier 1 is fine */
    for (t = 0; t < 40; t++) {
        vrMoveIn_t near_in = move_in(150, -1, 1, 200), far_in = move_in(900, -1, 1, 200);

        VrMove_Update(&sq[0].move, &near_in, t * 100);
        VrMove_Update(&sq[1].move, &far_in, t * 100);
        VrMove_Update(&sq[2].move, &near_in, t * 100);
        if (t % 5 == 0) {
            VrTac_DmgAdd(&sq[2].dmg, t * 100, 12);
        }
    }
    CHECK(sq[0].move.mode == VRMOVE_NEAR);
    CHECK(sq[1].move.mode == VRMOVE_CATCHUP);
    CHECK(sq[2].move.mode == VRMOVE_NEAR);
    {
        int hits, total;

        VrTac_DmgWindow(&sq[0].dmg, 4000, 4000, &hits, &total);
        CHECK(hits == 0);
        VrTac_DmgWindow(&sq[2].dmg, 4000, 4000, &hits, &total);
        CHECK(hits >= 3 && total >= 36);
    }
    /* soldier 2 gets stuck: its recovery ladder advances, the others are untouched */
    {
        int now, steps = 0;

        for (now = 100; now < 20000; now += 100) {
            steps += (VrRecover_Step(&sq[1].rec, pos, 1, now) != VRREC_NONE);
        }
        CHECK(steps >= 3);
        CHECK(sq[0].rec.attempts == 0 && sq[2].rec.attempts == 0);
    }
}

/* ===================== squad target distribution ===================== */

static vrTacCand_t sq_cand(int ent, float dist)
{
    vrTacCand_t c;

    memset(&c, 0, sizeof(c));
    c.entNum = ent;
    c.dist = dist;
    c.distToPlayer = dist + 100;
    c.visibleNow = 1;
    c.msSinceSeen = 100;
    c.msSinceHurtPlayer = -1;
    c.msSinceHurtSelf = -1;
    c.inWeaponRange = 1;
    return c;
}

/* Soldiers choose one after the other and see the claims of those before them. */
static void choose_targets(const vrTacCand_t *enemies, int n, int soldiers, int *chosen)
{
    vrTacScoreParams_t p;
    int s, k;

    p.stance = VRTAC_STANCE_DEFENSIVE;
    p.coverMeActive = 0;
    for (s = 0; s < soldiers; s++) {
        float scores[8];
        int claimsOnTarget[8], sw, pick;

        memset(claimsOnTarget, 0, sizeof(claimsOnTarget));
        for (k = 0; k < s; k++) {
            int e;

            for (e = 0; e < n; e++) {
                if (enemies[e].entNum == chosen[k]) {
                    claimsOnTarget[e]++;
                }
            }
        }
        for (k = 0; k < n; k++) {
            vrTacCand_t c = enemies[k];

            c.squadClaims = claimsOnTarget[k];
            scores[k] = VrTac_ThreatScore(&c, &p);
        }
        pick = VrTac_SelectTarget(scores, n, -1, 0, &sw);
        chosen[s] = pick >= 0 ? enemies[pick].entNum : -1;
    }
}

static void test_squad_spreads_fire_over_similar_enemies(void)
{
    vrTacCand_t e[3];
    int chosen[3];

    e[0] = sq_cand(10, 500);
    e[1] = sq_cand(11, 520);
    e[2] = sq_cand(12, 540);
    choose_targets(e, 3, 3, chosen);
    CHECK(chosen[0] == 10);
    CHECK(chosen[1] != chosen[0]);
    CHECK(chosen[2] != chosen[0] && chosen[2] != chosen[1]);       /* three soldiers, three different targets */
}

static void test_squad_concentrates_on_the_dangerous_enemy(void)
{
    vrTacCand_t e[3];
    int chosen[3];

    e[0] = sq_cand(10, 500);
    e[1] = sq_cand(11, 520);
    e[1].targetsPlayer = 1;                                         /* shooting at the player */
    e[2] = sq_cand(12, 540);
    choose_targets(e, 3, 3, chosen);
    CHECK(chosen[0] == 11 && chosen[1] == 11 && chosen[2] == 11);   /* concentrated fire is fine here */

    e[1].targetsPlayer = 0;
    e[1].designated = 1;                                            /* the player pointed at it */
    choose_targets(e, 3, 3, chosen);
    CHECK(chosen[0] == 11 && chosen[1] == 11 && chosen[2] == 11);
}

static void test_squad_single_enemy_all_engage(void)
{
    vrTacCand_t e[1];
    int chosen[3];

    e[0] = sq_cand(10, 500);
    choose_targets(e, 1, 3, chosen);
    CHECK(chosen[0] == 10 && chosen[1] == 10 && chosen[2] == 10);   /* nothing else to shoot: nobody stands idle */
}

static void test_squad_claim_penalty_is_bounded(void)
{
    vrTacScoreParams_t p;
    vrTacCand_t c = sq_cand(10, 500);
    float base, one, two, many;

    p.stance = VRTAC_STANCE_DEFENSIVE;
    p.coverMeActive = 0;
    base = VrTac_ThreatScore(&c, &p);
    c.squadClaims = 1; one = VrTac_ThreatScore(&c, &p);
    c.squadClaims = 2; two = VrTac_ThreatScore(&c, &p);
    c.squadClaims = 9; many = VrTac_ThreatScore(&c, &p);
    CHECK(one < base && two < one);
    CHECK(many == two);                                             /* capped: never a hole in the targeting */
    CHECK(many > 0.0f);
}

/* ===================== wounded / out of ammo ===================== */

static void test_wounded_and_no_ammo_seek_cover(void)
{
    vrTacCoverWant_t w;
    int why;

    memset(&w, 0, sizeof(w));
    w.state = VRMERC_ENGAGE;
    w.order = VRMERC_ORDER_FOLLOW;
    w.stance = VRTAC_STANCE_AGGRESSIVE;
    w.assist = 1;
    w.haveTarget = 1;
    w.targetVisible = 1;
    w.targetDist = 700;
    w.healthFrac = 1.0f;
    w.cooldownOk = 1;
    CHECK(!VrTac_WantCover(&w, &why));
    w.healthFrac = 0.4f;
    CHECK(!VrTac_WantCover(&w, &why));                              /* wounded but not being hit */
    w.recentDamage = 1;
    CHECK(VrTac_WantCover(&w, &why) && why == VRTAC_R_LOW_HEALTH);
    w.healthFrac = 1.0f; w.recentDamage = 0; w.outOfAmmo = 1;
    CHECK(VrTac_WantCover(&w, &why) && why == VRTAC_R_NO_AMMO);
    w.targetVisible = 0;
    CHECK(!VrTac_WantCover(&w, &why));                              /* nothing to hide from */
}

/* ===================== recruitment sequence ===================== */

typedef struct {
    int inUse[VRMERC_MAX_SQUAD];
    int pending;
    int limit;
} simSquad_t;

static int sim_live(const simSquad_t *s)
{
    int i, n = 0;

    for (i = 0; i < VRMERC_MAX_SQUAD; i++) {
        n += s->inUse[i];
    }
    return n;
}

static int sim_hire(simSquad_t *s)
{
    int r = VrSquad_CanAdd(sim_live(s), s->pending, s->limit), idx;

    if (r != VRSQ_OK) {
        return r;
    }
    idx = VrSquad_PickFreeInstance(s->inUse, VRMERC_MAX_SQUAD);
    if (idx < 0) {
        return VRSQ_NO_INSTANCE;
    }
    s->inUse[idx] = 1;
    return VRSQ_OK;
}

static void test_repeated_recruitment_and_limits(void)
{
    simSquad_t s;
    int i;

    memset(&s, 0, sizeof(s));
    s.limit = VrSquad_Limit(0);                                     /* 2 */
    CHECK(sim_hire(&s) == VRSQ_OK);
    CHECK(sim_hire(&s) == VRSQ_OK);
    CHECK(sim_hire(&s) == VRSQ_FULL);                               /* reported, not silent */
    CHECK(sim_live(&s) == 2);
    s.limit = VrSquad_Limit(3);                                     /* owner raises it after the performance test */
    CHECK(sim_hire(&s) == VRSQ_OK);
    CHECK(sim_hire(&s) == VRSQ_FULL);
    s.inUse[0] = 0;                                                 /* soldier 1 dies */
    CHECK(sim_hire(&s) == VRSQ_OK);                                 /* its record and AI name are reused ... */
    CHECK(s.inUse[0] == 1);
    for (i = 0; i < 20; i++) {                                      /* ... and never beyond the limit */
        sim_hire(&s);
    }
    CHECK(sim_live(&s) == 3);
    s.pending = 1;
    s.inUse[1] = 0;
    CHECK(sim_hire(&s) == VRSQ_PENDING);                            /* one recruit at a time */
}

static void test_dismiss_all_then_rehire(void)
{
    simSquad_t s;
    int i, mask;

    memset(&s, 0, sizeof(s));
    s.limit = 3;
    sim_hire(&s); sim_hire(&s); sim_hire(&s);
    mask = 0;
    for (i = 0; i < VRMERC_MAX_SQUAD; i++) {
        mask |= s.inUse[i] << i;
    }
    {
        int sel;

        CHECK(VrSquad_ResolveSelection("all", 0, mask, &sel) == VRSQ_SEL_OK && VrSquad_CountBits(sel) == 3);
        for (i = 0; i < VRMERC_MAX_SQUAD; i++) {
            if (sel & (1 << i)) {
                s.inUse[i] = 0;
            }
        }
    }
    CHECK(sim_live(&s) == 0);
    CHECK(VrSquad_ResolveSelection("all", 0, 0, &mask) == VRSQ_SEL_NONE_ALIVE);
    CHECK(sim_hire(&s) == VRSQ_OK);                                 /* a fresh start works */
    CHECK(s.inUse[0] == 1);
}

int main(void)
{
    test_effective_distance();
    test_path_distance_from_travel_time();
    test_mode_near_stays_near_when_close();
    test_mode_run_thresholds_depend_on_player_motion();
    test_mode_catchup_thresholds();
    test_mode_unreachable_counts_as_catchup();
    test_corner_case_is_not_ignored();
    test_mode_hysteresis_and_dwell();
    test_mode_no_los_catchup_exit_is_stricter();
    test_mode_suppressed_drops_to_near_at_once();
    test_mode_does_not_flap_on_distance_jitter();
    test_speed_floor_per_mode();
    test_chase_flags();
    test_model_stock_speed_is_below_the_player();
    test_model_policy_keeps_up_where_stock_falls_behind();

    test_recovery_ladder_escalates_and_waits();
    test_recovery_step_timing();
    test_recovery_not_triggered_while_moving_or_idle();
    test_recovery_waits_then_starts_a_new_episode();
    test_recovery_forgets_old_failures_after_good_movement();

    test_trail_records_only_real_movement();
    test_trail_ring_keeps_newest();
    test_trail_pick();

    test_squad_limit();
    test_squad_can_add();
    test_squad_free_instance();
    test_squad_unique_identity();
    test_squad_selection();
    test_squad_rank_and_count();
    test_formation_roles_are_separate_and_behind();
    test_formation_left_right_roles();
    test_formation_narrow_is_single_file();
    test_formation_position_need();
    test_formation_settles_without_ping_pong();
    test_spawn_clear();
    test_instances_are_independent();
    test_squad_spreads_fire_over_similar_enemies();
    test_squad_concentrates_on_the_dangerous_enemy();
    test_squad_single_enemy_all_engage();
    test_squad_claim_penalty_is_bounded();
    test_wounded_and_no_ammo_seek_cover();
    test_repeated_recruitment_and_limits();
    test_dismiss_all_then_rehire();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
