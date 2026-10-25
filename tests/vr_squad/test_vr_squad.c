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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
