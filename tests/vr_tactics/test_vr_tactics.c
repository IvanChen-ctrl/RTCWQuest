/*
 * Host tests for g_vrtactics.h: the tactical decision layer of the AI mercenary companion
 * (command validation, friend-or-foe classification, threat scoring, target selection with
 * hysteresis, friendly-fire line checks, formation spacing, cover, flanking, regroup/retreat
 * and the maneuver entry/exit rules).
 *
 * These test the exact functions that run inside the game module.  They do NOT exercise the
 * engine glue in g_merc.c (traces, AAS, the stock AI) and prove nothing about behaviour on a
 * headset.
 *
 * Build and run: make -C tests/vr_tactics test
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "g_vrtactics.h"

#define T_NAZI      0
#define T_ALLIES    1
#define T_MONSTER   2
#define T_NEUTRAL   7

static int failures;
static int checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* ===================== reason codes ===================== */

static void test_reason_names_are_complete_and_unique(void)
{
    int i, j;

    for (i = 0; i < VRTAC_R_COUNT; i++) {
        const char *a = VrTac_ReasonName(i);

        CHECK(a != NULL && a[0] != 0 && strcmp(a, "unknown") != 0);
        for (j = i + 1; j < VRTAC_R_COUNT; j++) {
            CHECK(strcmp(a, VrTac_ReasonName(j)) != 0);
        }
    }
    CHECK(strcmp(VrTac_ReasonName(-1), "unknown") == 0);
    CHECK(strcmp(VrTac_ReasonName(VRTAC_R_COUNT), "unknown") == 0);
    CHECK(strcmp(VrTac_ReasonName(VRTAC_OK), "ok") == 0);
    CHECK(strcmp(VrTac_CommandName(VRTAC_CMD_COVER_ME), "cover_me") == 0);
    CHECK(strcmp(VrTac_CommandName(99), "unknown") == 0);
}

/* ===================== command validation ===================== */

static vrTacOrderCtx_t good_ctx(void)
{
    vrTacOrderCtx_t c;

    memset(&c, 0, sizeof(c));
    c.companionPresent = 1;
    c.companionAlive = 1;
    c.state = VRMERC_FOLLOW;
    c.playerOk = 1;
    c.assist = 1;
    return c;
}

static void test_commands_valid_when_everything_is_fine(void)
{
    vrTacOrderCtx_t c = good_ctx();
    int cmd;

    for (cmd = 0; cmd < VRTAC_CMD_COUNT; cmd++) {
        CHECK(VrTac_ValidateCommand(cmd, &c) == VRTAC_OK);
    }
}

static void test_commands_refused_without_companion(void)
{
    vrTacOrderCtx_t c = good_ctx();

    c.companionPresent = 0;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ATTACK, &c) == VRTAC_R_NO_COMPANION);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_COVER_ME, &c) == VRTAC_R_NO_COMPANION);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_R_NO_COMPANION);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_DISMISS, &c) == VRTAC_R_NO_COMPANION);
    /* settings that need nobody still work, so the player can pre-set them */
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_STANCE, &c) == VRTAC_OK);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ASSIST, &c) == VRTAC_OK);
}

static void test_commands_refused_when_dead_or_terminal(void)
{
    vrTacOrderCtx_t c = good_ctx();
    int s;

    c.companionAlive = 0;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_R_COMPANION_DEAD);
    c.companionAlive = 1;
    c.state = VRMERC_DEAD;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_R_NOT_ACTIVE);
    c.state = VRMERC_DISMISSED;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ATTACK, &c) == VRTAC_R_NOT_ACTIVE);
    c.state = VRMERC_NONE;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_HOLD, &c) == VRTAC_R_NOT_ACTIVE);

    /* every maneuver and base state counts as active */
    for (s = VRMERC_FOLLOW; s <= VRMERC_RETURN; s++) {
        c.state = s;
        CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_OK);
    }
    c.state = VRMERC_COVER;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_OK);
    c.state = VRMERC_FLANK;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_OK);
    c.state = VRMERC_REGROUP;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_OK);
}

static void test_tactical_commands_need_a_sane_game_state(void)
{
    vrTacOrderCtx_t c = good_ctx();

    c.cinematic = 1;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ATTACK, &c) == VRTAC_R_CINEMATIC);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_R_CINEMATIC);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_FOLLOW, &c) == VRTAC_OK);    /* plain orders still allowed */
    c.cinematic = 0;
    c.loading = 1;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_COVER_ME, &c) == VRTAC_R_LOADING);
    c.loading = 0;
    c.playerOk = 0;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ATTACK, &c) == VRTAC_R_PLAYER_UNAVAILABLE);
    c.playerOk = 1;
    CHECK(VrTac_ValidateCommand(-1, &c) == VRTAC_R_UNKNOWN_COMMAND);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_COUNT, &c) == VRTAC_R_UNKNOWN_COMMAND);
}

static void test_attack_and_cover_need_assist(void)
{
    vrTacOrderCtx_t c = good_ctx();

    c.assist = 0;
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_ATTACK, &c) == VRTAC_R_ASSIST_OFF);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_COVER_ME, &c) == VRTAC_R_ASSIST_OFF);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_REGROUP, &c) == VRTAC_OK);
    CHECK(VrTac_ValidateCommand(VRTAC_CMD_HOLD, &c) == VRTAC_OK);
}

/* ===================== friend or foe ===================== */

static int classify(int exists, int alive, int cast, int player, int self, int comp, int team)
{
    return VrTac_ClassifyTarget(exists, alive, cast, player, self, comp, T_ALLIES, team, T_NEUTRAL);
}

static void test_classification(void)
{
    CHECK(classify(1, 1, 1, 0, 0, 0, T_NAZI) == VRTAC_OK);
    CHECK(classify(1, 1, 1, 0, 0, 0, T_MONSTER) == VRTAC_OK);      /* zombies are hostile */
    CHECK(classify(0, 1, 1, 0, 0, 0, T_NAZI) == VRTAC_R_NO_TARGET);
    CHECK(classify(1, 1, 1, 1, 0, 0, T_NAZI) == VRTAC_R_TARGET_PLAYER);
    CHECK(classify(1, 1, 1, 0, 1, 0, T_NAZI) == VRTAC_R_TARGET_SELF);
    CHECK(classify(1, 1, 1, 0, 0, 1, T_ALLIES) == VRTAC_R_TARGET_FRIENDLY);
    CHECK(classify(1, 1, 1, 0, 0, 0, T_ALLIES) == VRTAC_R_TARGET_FRIENDLY);     /* story ally */
    CHECK(classify(1, 1, 1, 0, 0, 0, T_NEUTRAL) == VRTAC_R_TARGET_NEUTRAL);     /* civilian */
    CHECK(classify(1, 0, 1, 0, 0, 0, T_NAZI) == VRTAC_R_TARGET_DEAD);
    CHECK(classify(1, 1, 0, 0, 0, 0, T_NAZI) == VRTAC_R_TARGET_NOT_AI);         /* a door or crate */
}

static void test_classification_order_is_safe(void)
{
    /* a dead friend or the player is reported as what it is, never as a valid hostile */
    CHECK(classify(1, 0, 1, 1, 0, 0, T_NAZI) == VRTAC_R_TARGET_PLAYER);
    CHECK(classify(1, 0, 1, 0, 0, 1, T_ALLIES) == VRTAC_R_TARGET_FRIENDLY);
    /* nothing but a live, cast, enemy-team entity is ever OK */
    {
        int exists, alive, cast, player, self, comp, team, ok = 0, bad = 0;

        for (exists = 0; exists < 2; exists++)
        for (alive = 0; alive < 2; alive++)
        for (cast = 0; cast < 2; cast++)
        for (player = 0; player < 2; player++)
        for (self = 0; self < 2; self++)
        for (comp = 0; comp < 2; comp++)
        for (team = 0; team < 8; team++) {
            int r = classify(exists, alive, cast, player, self, comp, team);

            if (r == VRTAC_OK) {
                ok++;
                if (!exists || !alive || !cast || player || self || comp || team == T_ALLIES || team == T_NEUTRAL) {
                    bad++;
                }
            }
        }
        CHECK(ok > 0);
        CHECK(bad == 0);
    }
}

static void test_designation_range(void)
{
    CHECK(VrTac_ValidateDesignation(VRTAC_OK, 500.0f) == VRTAC_OK);
    CHECK(VrTac_ValidateDesignation(VRTAC_OK, VRTAC_DESIGNATE_RANGE) == VRTAC_OK);
    CHECK(VrTac_ValidateDesignation(VRTAC_OK, VRTAC_DESIGNATE_RANGE + 1.0f) == VRTAC_R_TARGET_RANGE);
    CHECK(VrTac_ValidateDesignation(VRTAC_OK, -1.0f) == VRTAC_R_TARGET_RANGE);
    /* a rejected classification is passed through untouched, whatever the range */
    CHECK(VrTac_ValidateDesignation(VRTAC_R_TARGET_FRIENDLY, 10.0f) == VRTAC_R_TARGET_FRIENDLY);
    CHECK(VrTac_ValidateDesignation(VRTAC_R_TARGET_PLAYER, 10.0f) == VRTAC_R_TARGET_PLAYER);
}

/* ===================== damage window ===================== */

static void test_damage_window(void)
{
    vrTacDmgLog_t l;
    int hits, total;

    VrTac_DmgReset(&l);
    VrTac_DmgWindow(&l, 5000, 4000, &hits, &total);
    CHECK(hits == 0 && total == 0);

    VrTac_DmgAdd(&l, 1000, 10);
    VrTac_DmgAdd(&l, 2000, 20);
    VrTac_DmgAdd(&l, 3000, 30);
    VrTac_DmgWindow(&l, 3500, 4000, &hits, &total);
    CHECK(hits == 3 && total == 60);
    VrTac_DmgWindow(&l, 5500, 4000, &hits, &total);     /* the 1000 ms hit has aged out */
    CHECK(hits == 2 && total == 50);
    VrTac_DmgWindow(&l, 9000, 4000, &hits, &total);
    CHECK(hits == 0 && total == 0);

    VrTac_DmgAdd(&l, 9500, 0);                          /* zero or negative damage is ignored */
    VrTac_DmgAdd(&l, 9500, -5);
    VrTac_DmgWindow(&l, 9600, 4000, &hits, &total);
    CHECK(hits == 0);
}

static void test_damage_ring_overwrites_oldest(void)
{
    vrTacDmgLog_t l;
    int hits, total, i;

    VrTac_DmgReset(&l);
    for (i = 0; i < VRTAC_DMG_SLOTS + 3; i++) {
        VrTac_DmgAdd(&l, 1000 + i * 10, 1);
    }
    VrTac_DmgWindow(&l, 1200, 4000, &hits, &total);
    CHECK(hits == VRTAC_DMG_SLOTS);                     /* bounded memory, newest kept */
    /* a log in the future of "now" (level restart) never reports negative ages as hits */
    VrTac_DmgWindow(&l, 0, 4000, &hits, &total);
    CHECK(hits == 0);
}

/* ===================== threat scoring ===================== */

static vrTacCand_t base_cand(void)
{
    vrTacCand_t c;

    memset(&c, 0, sizeof(c));
    c.entNum = 5;
    c.dist = 600;
    c.distToPlayer = 700;
    c.visibleNow = 1;
    c.msSinceSeen = 100;
    c.msSinceHurtPlayer = -1;
    c.msSinceHurtSelf = -1;
    c.inWeaponRange = 1;
    return c;
}

static vrTacScoreParams_t defensive(void)
{
    vrTacScoreParams_t p;

    p.stance = VRTAC_STANCE_DEFENSIVE;
    p.coverMeActive = 0;
    return p;
}

static void test_score_stale_target_is_dropped(void)
{
    vrTacCand_t c = base_cand();
    vrTacScoreParams_t p = defensive();

    c.visibleNow = 0;
    c.msSinceSeen = VRTAC_STALE_MS + 1;
    CHECK(VrTac_ThreatScore(&c, &p) < 0.0f);
    c.msSinceSeen = VRTAC_STALE_MS - 1;
    CHECK(VrTac_ThreatScore(&c, &p) >= 0.0f);
    /* a designated target is never stale: the player pointed at it */
    c.msSinceSeen = VRTAC_STALE_MS * 3;
    c.designated = 1;
    CHECK(VrTac_ThreatScore(&c, &p) > 0.0f);
}

static void test_score_priorities(void)
{
    vrTacCand_t plain = base_cand(), t;
    vrTacScoreParams_t p = defensive();
    float base = VrTac_ThreatScore(&plain, &p);

    t = plain; t.dist = 200;
    CHECK(VrTac_ThreatScore(&t, &p) > base);                    /* nearer is worse for us */
    t = plain; t.targetsPlayer = 1;
    CHECK(VrTac_ThreatScore(&t, &p) > base + 40.0f);            /* threat to the player */
    t = plain; t.targetsSelf = 1;
    CHECK(VrTac_ThreatScore(&t, &p) > base + 30.0f);
    t = plain; t.msSinceHurtPlayer = 500;
    CHECK(VrTac_ThreatScore(&t, &p) > base);
    t = plain; t.msSinceHurtPlayer = 5000;                      /* too old to matter */
    CHECK(VrTac_ThreatScore(&t, &p) == base);
    t = plain; t.msSinceHurtSelf = 500;
    CHECK(VrTac_ThreatScore(&t, &p) > base);
    t = plain; t.visibleNow = 0; t.msSinceSeen = 3000;
    CHECK(VrTac_ThreatScore(&t, &p) < base);                    /* remembered, not seen */
    t = plain; t.inWeaponRange = 0;
    CHECK(VrTac_ThreatScore(&t, &p) < base);
    t = plain; t.designated = 1;
    CHECK(VrTac_ThreatScore(&t, &p) > base + 150.0f);
}

static void test_score_stance_and_cover_me(void)
{
    vrTacCand_t threat = base_cand(), near_enemy = base_cand();
    vrTacScoreParams_t def = defensive(), agg = defensive(), cov = defensive();

    threat.targetsPlayer = 1;
    threat.dist = 800;
    near_enemy.dist = 150;
    agg.stance = VRTAC_STANCE_AGGRESSIVE;
    cov.coverMeActive = 1;

    /* defensive: the one shooting at the player outranks a nearer idle enemy */
    CHECK(VrTac_ThreatScore(&threat, &def) > VrTac_ThreatScore(&near_enemy, &def));
    /* aggressive: nearby enemies gain, the player-threat bonus is smaller than in defensive */
    CHECK(VrTac_ThreatScore(&near_enemy, &agg) > VrTac_ThreatScore(&near_enemy, &def));
    CHECK(VrTac_ThreatScore(&threat, &agg) < VrTac_ThreatScore(&threat, &def) + 40.0f);
    /* cover me: threats close to the player gain */
    near_enemy.distToPlayer = 150;
    threat.distToPlayer = 1500;
    CHECK(VrTac_ThreatScore(&near_enemy, &cov) > VrTac_ThreatScore(&near_enemy, &def));
    CHECK(VrTac_ThreatScore(&threat, &cov) - VrTac_ThreatScore(&threat, &def) <
          VrTac_ThreatScore(&near_enemy, &cov) - VrTac_ThreatScore(&near_enemy, &def));
}

static void test_score_never_negative_zero_means_usable(void)
{
    vrTacCand_t c = base_cand();
    vrTacScoreParams_t p = defensive();
    float s;

    c.dist = 99999;
    c.visibleNow = 0;
    c.msSinceSeen = VRTAC_STALE_MS - 1;
    c.inWeaponRange = 0;
    s = VrTac_ThreatScore(&c, &p);
    CHECK(s >= 0.0f);       /* clamped: only staleness returns -1 */
}

/* ===================== target selection / hysteresis ===================== */

static void test_select_nothing_usable(void)
{
    float sc[3] = { -1, -1, -1 };
    int sw = 9;

    CHECK(VrTac_SelectTarget(sc, 3, -1, 0, &sw) == -1);
    CHECK(sw == 0);
    CHECK(VrTac_SelectTarget(sc, 3, 1, 5000, &sw) == -1);      /* the current one went stale */
    CHECK(sw == 1);
    CHECK(VrTac_SelectTarget(sc, 0, -1, 0, &sw) == -1);
}

static void test_select_picks_best_when_no_current(void)
{
    float sc[4] = { 120, 200, -1, 180 };
    int sw = 0;

    CHECK(VrTac_SelectTarget(sc, 4, -1, 0, &sw) == 1);
    CHECK(sw == 1);
}

static void test_select_commitment_blocks_small_changes(void)
{
    float sc[2] = { 200, 230 };     /* 15% better: below the ratio */
    int sw = 0;

    CHECK(VrTac_SelectTarget(sc, 2, 0, 100, &sw) == 0 && sw == 0);          /* committed */
    CHECK(VrTac_SelectTarget(sc, 2, 0, 10000, &sw) == 0 && sw == 0);        /* long enough, still not 30% */
    sc[1] = 280;                                                            /* 40% better */
    CHECK(VrTac_SelectTarget(sc, 2, 0, 100, &sw) == 0 && sw == 0);          /* too early */
    CHECK(VrTac_SelectTarget(sc, 2, 0, VRTAC_TARGET_COMMIT_MS - 1, &sw) == 0 && sw == 0);
    CHECK(VrTac_SelectTarget(sc, 2, 0, VRTAC_TARGET_COMMIT_MS, &sw) == 1 && sw == 1);
}

static void test_select_emergency_overrides_commitment(void)
{
    float sc[2] = { 200, 200 + VRTAC_EMERGENCY_MARGIN };
    int sw = 0;

    CHECK(VrTac_SelectTarget(sc, 2, 0, 0, &sw) == 1 && sw == 1);
    sc[1] = 200 + VRTAC_EMERGENCY_MARGIN - 1;
    CHECK(VrTac_SelectTarget(sc, 2, 0, 0, &sw) == 0 && sw == 0);
}

static void test_select_abs_margin_for_tiny_scores(void)
{
    float sc[2] = { 10, 14 };       /* 40% better but only 4 points */
    int sw = 0;

    CHECK(VrTac_SelectTarget(sc, 2, 0, 9999, &sw) == 0 && sw == 0);
}

static void test_select_invalid_current_index_is_safe(void)
{
    float sc[2] = { 100, 150 };
    int sw = 0;

    CHECK(VrTac_SelectTarget(sc, 2, 7, 0, &sw) == 1);       /* out of range current */
    CHECK(VrTac_SelectTarget(sc, 2, -3, 0, &sw) == 1);
}

/* Two enemies whose scores wobble +-8% every scan must not cause target flip-flopping. */
static void test_select_no_flip_flop_under_noise(void)
{
    float sc[2];
    int cur = -1, onTarget = 0, switches = 0, i, sw;

    for (i = 0; i < 400; i++) {                 /* 400 scans = 100 s */
        sc[0] = 200 * (1.0f + ((i % 2) ? 0.08f : -0.08f));
        sc[1] = 200 * (1.0f + ((i % 2) ? -0.08f : 0.08f));
        {
            int sel = VrTac_SelectTarget(sc, 2, cur, onTarget, &sw);

            if (sel != cur) {
                switches++;
                cur = sel;
                onTarget = 0;
            }
        }
        onTarget += VRTAC_PERCEPTION_MS;
    }
    CHECK(switches == 1);       /* only the initial acquisition */
}

/* A clearly better target takes over after the commitment time, and only once. */
static void test_select_switches_once_to_a_clearly_better_target(void)
{
    float sc[2] = { 200, 150 };
    int cur = -1, onTarget = 0, switches = 0, i, sw;

    for (i = 0; i < 40; i++) {
        if (i == 8) {
            sc[1] = 320;
        }
        {
            int sel = VrTac_SelectTarget(sc, 2, cur, onTarget, &sw);

            if (sel != cur) {
                switches++;
                cur = sel;
                onTarget = 0;
            }
        }
        onTarget += VRTAC_PERCEPTION_MS;
    }
    CHECK(switches == 2 && cur == 1);
}

/* ===================== friendly fire ===================== */

static void test_ff_friend_between_is_blocked(void)
{
    float shooter[3] = { 0, 0, 0 }, target[3] = { 1000, 0, 0 };
    float player[3] = { 500, 10, 0 };

    CHECK(VrTac_FriendInLineOfFire(shooter, target, player, VRTAC_FF_RADIUS_PLAYER));
}

static void test_ff_friend_off_the_line_is_clear(void)
{
    float shooter[3] = { 0, 0, 0 }, target[3] = { 1000, 0, 0 };
    float side[3] = { 500, 100, 0 };

    CHECK(!VrTac_FriendInLineOfFire(shooter, target, side, VRTAC_FF_RADIUS_PLAYER));
}

static void test_ff_friend_behind_shooter_or_beyond_target(void)
{
    float shooter[3] = { 0, 0, 0 }, target[3] = { 1000, 0, 0 };
    float behind[3] = { -200, 0, 0 }, beyond[3] = { 1500, 0, 0 }, atTarget[3] = { 1000, 0, 0 };

    CHECK(!VrTac_FriendInLineOfFire(shooter, target, behind, VRTAC_FF_RADIUS_PLAYER));
    CHECK(!VrTac_FriendInLineOfFire(shooter, target, beyond, VRTAC_FF_RADIUS_PLAYER));
    CHECK(!VrTac_FriendInLineOfFire(shooter, target, atTarget, VRTAC_FF_RADIUS_PLAYER));
}

static void test_ff_radius_edges_and_vertical(void)
{
    float shooter[3] = { 0, 0, 0 }, target[3] = { 1000, 0, 0 };
    float justIn[3] = { 500, 43, 0 }, justOut[3] = { 500, 45, 0 }, high[3] = { 500, 0, 200 };

    CHECK(VrTac_FriendInLineOfFire(shooter, target, justIn, VRTAC_FF_RADIUS_PLAYER));
    CHECK(!VrTac_FriendInLineOfFire(shooter, target, justOut, VRTAC_FF_RADIUS_PLAYER));
    CHECK(!VrTac_FriendInLineOfFire(shooter, target, high, VRTAC_FF_RADIUS_PLAYER));
}

static void test_ff_degenerate_segment(void)
{
    float p[3] = { 10, 0, 0 }, friendNear[3] = { 20, 0, 0 };

    /* shooter == target: nothing in between can be hit */
    CHECK(!VrTac_FriendInLineOfFire(p, p, friendNear, VRTAC_FF_RADIUS_PLAYER));
}

/* ===================== formation ===================== */

static void test_formation_slot_is_behind_and_to_the_side(void)
{
    float player[3] = { 100, 200, 30 }, slot[3], d;
    float fx, fy, rx, ry;

    VrTac_YawBasis(0.0f, &fx, &fy, &rx, &ry);
    CHECK(fabsf(fx - 1.0f) < 1e-4f && fabsf(fy) < 1e-4f);
    VrTac_FormationSlot(player, 0.0f, 1, VRTAC_FORM_BACK, VRTAC_FORM_SIDE, slot);
    CHECK(fabsf(slot[0] - (100 - VRTAC_FORM_BACK)) < 0.01f);     /* behind a player facing +x */
    CHECK(fabsf(slot[1] - (200 - VRTAC_FORM_SIDE)) < 0.01f);     /* right of +x is -y */
    CHECK(slot[2] == 30);
    VrTac_FormationSlot(player, 0.0f, -1, VRTAC_FORM_BACK, VRTAC_FORM_SIDE, slot);
    CHECK(fabsf(slot[1] - (200 + VRTAC_FORM_SIDE)) < 0.01f);

    /* rotating the player rotates the slot with the same distance */
    VrTac_FormationSlot(player, 90.0f, 1, VRTAC_FORM_BACK, VRTAC_FORM_SIDE, slot);
    d = sqrtf((slot[0] - 100) * (slot[0] - 100) + (slot[1] - 200) * (slot[1] - 200));
    CHECK(fabsf(d - sqrtf(VRTAC_FORM_BACK * VRTAC_FORM_BACK + VRTAC_FORM_SIDE * VRTAC_FORM_SIDE)) < 0.01f);
    CHECK(fabsf(slot[1] - (200 - VRTAC_FORM_BACK)) < 0.01f);     /* facing +y, behind is -y */
}

static void test_formation_slot_is_never_in_front(void)
{
    float player[3] = { 0, 0, 0 }, slot[3];
    int yaw, side;

    for (yaw = 0; yaw < 360; yaw += 15) {
        for (side = -1; side <= 1; side += 2) {
            float fx, fy, rx, ry, dot;

            VrTac_YawBasis((float)yaw, &fx, &fy, &rx, &ry);
            VrTac_FormationSlot(player, (float)yaw, side, VRTAC_FORM_BACK, VRTAC_FORM_SIDE, slot);
            dot = slot[0] * fx + slot[1] * fy;
            CHECK(dot < 0.0f);          /* behind the player's view direction */
            /* and the slot is not itself a "reposition" spot */
            CHECK(VrTac_RepositionReason(slot, player, fx, fy) == VRTAC_REPOS_NONE);
        }
    }
}

static void test_reposition_reasons(void)
{
    float player[3] = { 0, 0, 0 };
    float onTop[3] = { 20, 0, 0 }, inFront[3] = { 200, 20, 0 }, side[3] = { 150, 200, 0 };
    float behind[3] = { -150, 30, 0 }, farFront[3] = { 900, 0, 0 };

    CHECK(VrTac_RepositionReason(onTop, player, 1, 0) == VRTAC_REPOS_TOO_CLOSE);
    CHECK(VrTac_RepositionReason(onTop, player, 0, 0) == VRTAC_REPOS_TOO_CLOSE);     /* unknown aim */
    CHECK(VrTac_RepositionReason(inFront, player, 1, 0) == VRTAC_REPOS_IN_FRONT);
    CHECK(VrTac_RepositionReason(side, player, 1, 0) == VRTAC_REPOS_NONE);
    CHECK(VrTac_RepositionReason(behind, player, 1, 0) == VRTAC_REPOS_NONE);
    CHECK(VrTac_RepositionReason(farFront, player, 1, 0) == VRTAC_REPOS_NONE);       /* beyond the rule's range */
    CHECK(VrTac_RepositionReason(inFront, player, 0, 0) == VRTAC_REPOS_NONE);        /* no aim data: only distance */
    CHECK(VrTac_RepositionReason(inFront, player, 100, 0) == VRTAC_REPOS_IN_FRONT);  /* aim need not be unit length */
}

static void test_pick_side_keeps_the_companion_on_its_side(void)
{
    float player[3] = { 0, 0, 0 };
    float onRight[3] = { 0, -100, 0 }, onLeft[3] = { 0, 100, 0 };

    /* yaw 0: right is -y */
    CHECK(VrTac_PickSide(onRight, player, 0.0f) == 1);
    CHECK(VrTac_PickSide(onLeft, player, 0.0f) == -1);
}

/* Formation moves must settle: after repositioning to its slot the companion is no longer "in the way". */
static void test_formation_settles(void)
{
    float player[3] = { 0, 0, 0 }, self[3] = { 150, 10, 0 }, slot[3];
    int i, moves = 0;

    for (i = 0; i < 10; i++) {
        int why = VrTac_RepositionReason(self, player, 1, 0);

        if (why == VRTAC_REPOS_NONE) {
            break;
        }
        VrTac_FormationSlot(player, 0.0f, VrTac_PickSide(self, player, 0.0f), VRTAC_FORM_BACK, VRTAC_FORM_SIDE, slot);
        self[0] = slot[0]; self[1] = slot[1]; self[2] = slot[2];
        moves++;
    }
    CHECK(moves == 1);
    CHECK(VrTac_RepositionReason(self, player, 1, 0) == VRTAC_REPOS_NONE);
}

/* ===================== base state machine with maneuver states ===================== */

static vrMercInput_t calm_input(void)
{
    vrMercInput_t in;

    memset(&in, 0, sizeof(in));
    in.alive = 1;
    in.order = VRMERC_ORDER_FOLLOW;
    in.assist = 1;
    in.returnAllowed = 1;
    in.distToPlayer = 150;
    in.distToHold = 0;
    return in;
}

static void test_maneuver_states_are_active_and_distinct(void)
{
    CHECK(VrMerc_StateIsActive(VRMERC_COVER));
    CHECK(VrMerc_StateIsActive(VRMERC_FLANK));
    CHECK(VrMerc_StateIsActive(VRMERC_REGROUP));
    CHECK(VrMerc_StateIsManeuver(VRMERC_COVER));
    CHECK(VrMerc_StateIsManeuver(VRMERC_FLANK));
    CHECK(VrMerc_StateIsManeuver(VRMERC_REGROUP));
    CHECK(!VrMerc_StateIsManeuver(VRMERC_FOLLOW));
    CHECK(!VrMerc_StateIsManeuver(VRMERC_ENGAGE));
    CHECK(!VrMerc_StateIsManeuver(VRMERC_DEAD));
    CHECK(VRMERC_COVER > VRMERC_DISMISSED);     /* M3 enum values are unchanged */
    CHECK(VRMERC_FOLLOW == 1 && VRMERC_DISMISSED == 6);
}

static void test_maneuver_state_is_left_alone_by_the_base_machine(void)
{
    vrMercInput_t in = calm_input();
    int s;

    for (s = VRMERC_COVER; s <= VRMERC_REGROUP; s++) {
        vrMercResult_t r;

        in.haveTarget = 1;
        r = VrMerc_NextState(s, &in, 0);
        CHECK(r.state == s);            /* combat does not pull a maneuver back to ENGAGE */
        in.haveTarget = 0;
        in.distToPlayer = 5000;         /* far away does not trigger RETURN mid-maneuver */
        r = VrMerc_NextState(s, &in, 0);
        CHECK(r.state == s);
        in.distToPlayer = 150;
        in.order = VRMERC_ORDER_HOLD;
        r = VrMerc_NextState(s, &in, 0);
        CHECK(r.state == s);
        in.order = VRMERC_ORDER_FOLLOW;
    }
}

static void test_maneuver_state_still_dies_and_is_dismissed(void)
{
    vrMercInput_t in = calm_input();
    int s;

    for (s = VRMERC_COVER; s <= VRMERC_REGROUP; s++) {
        vrMercResult_t r;

        in.alive = 0;
        r = VrMerc_NextState(s, &in, 0);
        CHECK(r.state == VRMERC_DEAD);
        in.alive = 1;
        in.dismissRequested = 1;
        r = VrMerc_NextState(s, &in, 0);
        CHECK(r.state == VRMERC_DISMISSED);
        in.dismissRequested = 0;
    }
}

static void test_terminal_states_never_revive(void)
{
    vrMercInput_t in = calm_input();
    int s;

    in.haveTarget = 1;
    for (s = VRMERC_DEAD; s <= VRMERC_DISMISSED; s++) {
        CHECK(VrMerc_NextState(s, &in, 0).state == s);
    }
    CHECK(VrMerc_NextState(VRMERC_NONE, &in, 0).state == VRMERC_NONE);
}

int main(void)
{
    test_reason_names_are_complete_and_unique();

    test_commands_valid_when_everything_is_fine();
    test_commands_refused_without_companion();
    test_commands_refused_when_dead_or_terminal();
    test_tactical_commands_need_a_sane_game_state();
    test_attack_and_cover_need_assist();

    test_classification();
    test_classification_order_is_safe();
    test_designation_range();

    test_damage_window();
    test_damage_ring_overwrites_oldest();

    test_score_stale_target_is_dropped();
    test_score_priorities();
    test_score_stance_and_cover_me();
    test_score_never_negative_zero_means_usable();

    test_select_nothing_usable();
    test_select_picks_best_when_no_current();
    test_select_commitment_blocks_small_changes();
    test_select_emergency_overrides_commitment();
    test_select_abs_margin_for_tiny_scores();
    test_select_invalid_current_index_is_safe();
    test_select_no_flip_flop_under_noise();
    test_select_switches_once_to_a_clearly_better_target();

    test_ff_friend_between_is_blocked();
    test_ff_friend_off_the_line_is_clear();
    test_ff_friend_behind_shooter_or_beyond_target();
    test_ff_radius_edges_and_vertical();
    test_ff_degenerate_segment();

    test_formation_slot_is_behind_and_to_the_side();
    test_formation_slot_is_never_in_front();
    test_reposition_reasons();
    test_pick_side_keeps_the_companion_on_its_side();
    test_formation_settles();

    test_maneuver_states_are_active_and_distinct();
    test_maneuver_state_is_left_alone_by_the_base_machine();
    test_maneuver_state_still_dies_and_is_dismissed();
    test_terminal_states_never_revive();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
