/*
 * Host tests for g_vrlogic.h: single-player God Mode policy and the AI
 * mercenary companion decision logic (state machine, hire rules, spawn
 * candidates, friend-or-foe rules, stall watchdog, auto re-hire policy).
 *
 * Build and run: make -C tests/vr_gameplay test
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "g_vrlogic.h"

/* Team numbers as in AITeam_t (ai_cast.h); the logic only compares them. */
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

/* ===================== God Mode ===================== */

static void test_god_default_off_never_protects(void)
{
    CHECK(!VrGod_Protects(1, 0, 1, 0, 0, 0));
}

static void test_god_protects_local_player_only(void)
{
    CHECK(VrGod_Protects(1, 1, 1, 0, 0, 0));       /* single player human, option on */
    CHECK(!VrGod_Protects(1, 1, 1, 1, 0, 0));      /* an AI character in slot 0 is never protected */
    CHECK(!VrGod_Protects(1, 1, 1, 0, 5, 0));      /* another client number */
    CHECK(!VrGod_Protects(1, 1, 0, 0, 0, 0));      /* not a client at all (door, prop, ...) */
    CHECK(!VrGod_Protects(1, 1, 1, 1, 7, 0));      /* enemy soldier */
}

static void test_god_single_player_only(void)
{
    /* any non single-player game type: the option is ignored even if the cvar is set */
    CHECK(!VrGod_Protects(0, 1, 1, 0, 0, 0));
    CHECK(!VrGod_Protects(0, 1, 1, 0, 3, 0));
}

static void test_god_does_not_stop_scripted_kills(void)
{
    /* telefrag / kill trigger carry DAMAGE_NO_PROTECTION, same as the stock FL_GODMODE */
    CHECK(!VrGod_Protects(1, 1, 1, 0, 0, 1));
    CHECK(VrGod_Protects(1, 1, 1, 0, 0, 0));
}

static void test_god_toggle_off_restores_normal_damage(void)
{
    int on = VrGod_Protects(1, 1, 1, 0, 0, 0);
    int off = VrGod_Protects(1, 0, 1, 0, 0, 0);
    CHECK(on == 1 && off == 0);
}

static void test_god_restore_health(void)
{
    CHECK(VrGod_RestoreHealth(37, 100) == 100);
    CHECK(VrGod_RestoreHealth(100, 100) == 100);
    CHECK(VrGod_RestoreHealth(120, 100) == 120);   /* never lowers health */
    CHECK(VrGod_RestoreHealth(1, 0) == 100);       /* bad max falls back to 100 */
    CHECK(VrGod_RestoreHealth(50, 150) == 150);
}

/* ===================== hire rules ===================== */

static vrMercHireContext_t okContext(void)
{
    vrMercHireContext_t c;
    memset(&c, 0, sizeof(c));
    c.singlePlayerGame = 1;
    c.playerPresent = 1;
    c.playerAlive = 1;
    c.aiEnabled = 1;
    c.freeClientSlots = 8;
    c.navigationLoaded = 1;
    c.modelUsable = 1;
    return c;
}

static void test_hire_ok(void)
{
    vrMercHireContext_t c = okContext();
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_OK);
}

static void test_hire_refusals(void)
{
    vrMercHireContext_t c;

    c = okContext(); c.singlePlayerGame = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_NOT_SINGLEPLAYER);
    c = okContext(); c.playerPresent = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_NO_PLAYER);
    c = okContext(); c.playerAlive = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_PLAYER_DEAD);
    c = okContext(); c.cameraActive = 1;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_CINEMATIC);
    c = okContext(); c.loading = 1;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_LOADING);
    c = okContext(); c.aiEnabled = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_AI_DISABLED);
    c = okContext(); c.freeClientSlots = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_NO_CLIENT_SLOT);
    c = okContext(); c.navigationLoaded = 0;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_NO_NAVIGATION);
    c = okContext(); c.modelUsable = 0;                     /* regression: "Failed to load animation scripts" */
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_NO_MODEL);
}

static void test_hire_duplicate_prevention(void)
{
    vrMercHireContext_t c = okContext();

    c.companionActive = 1;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_ALREADY_ACTIVE);

    /* repeated commands while the spawn request is in flight */
    c = okContext();
    c.spawnPending = 1;
    CHECK(VrMerc_CanHire(&c) == VRMERC_HIRE_SPAWN_PENDING);

    /* simulate five rapid "hire" commands: exactly one request gets through */
    c = okContext();
    int accepted = 0;
    for (int i = 0; i < 5; i++) {
        if (VrMerc_CanHire(&c) == VRMERC_HIRE_OK) {
            accepted++;
            c.spawnPending = 1;         /* the engine side sets this when it queues the spawn */
        }
    }
    CHECK(accepted == 1);
    CHECK(VRMERC_MAX_COMPANIONS == 1);
}

/* ===================== spawn candidates ===================== */

static void test_spawn_candidates(void)
{
    float dx = 0.0f, dy = 0.0f;
    int ok = 0;

    CHECK(!VrMerc_SpawnOffset(-1, 0, &dx, &dy));
    CHECK(!VrMerc_SpawnOffset(VRMERC_SPAWN_CANDIDATES, 0, &dx, &dy));

    for (int i = 0; i < VRMERC_SPAWN_CANDIDATES; i++) {
        float d;
        CHECK(VrMerc_SpawnOffset(i, 0.0f, &dx, &dy));
        d = sqrtf(dx * dx + dy * dy);
        CHECK(d >= 100.0f && d <= 180.0f);          /* never inside the player, never far away */
        ok++;
    }
    CHECK(ok == VRMERC_SPAWN_CANDIDATES);
}

static void test_spawn_candidates_start_behind_player(void)
{
    /* player faces +x (yaw 0): the first candidates are behind (negative x) */
    float dx = 0.0f, dy = 0.0f;
    for (int i = 0; i < 4; i++) {
        CHECK(VrMerc_SpawnOffset(i, 0.0f, &dx, &dy));
        CHECK(dx < 0.0f);
    }
    /* rotating the player rotates the candidates: yaw 90 faces +y, so behind is -y */
    for (int i = 0; i < 4; i++) {
        CHECK(VrMerc_SpawnOffset(i, 90.0f, &dx, &dy));
        CHECK(dy < 0.0f);
    }
}

static void test_spawn_candidates_deterministic_and_distinct(void)
{
    float ax[VRMERC_SPAWN_CANDIDATES], ay[VRMERC_SPAWN_CANDIDATES];
    float bx = 0.0f, by = 0.0f;
    for (int i = 0; i < VRMERC_SPAWN_CANDIDATES; i++) {
        VrMerc_SpawnOffset(i, 33.0f, &ax[i], &ay[i]);
        VrMerc_SpawnOffset(i, 33.0f, &bx, &by);
        CHECK(ax[i] == bx && ay[i] == by);          /* same input, same output */
    }
    for (int i = 0; i < VRMERC_SPAWN_CANDIDATES; i++) {
        for (int j = i + 1; j < VRMERC_SPAWN_CANDIDATES; j++) {
            float dd = hypotf(ax[i] - ax[j], ay[i] - ay[j]);
            CHECK(dd > 20.0f);                      /* no duplicate candidate positions */
        }
    }
}

static void test_ground_walkable(void)
{
    CHECK(VrMerc_GroundWalkable(1.0f));
    CHECK(VrMerc_GroundWalkable(0.71f));
    CHECK(!VrMerc_GroundWalkable(0.69f));           /* steep slope */
    CHECK(!VrMerc_GroundWalkable(0.0f));            /* wall */
    CHECK(!VrMerc_GroundWalkable(-1.0f));           /* ceiling */
}

/* ===================== friend or foe ===================== */

static void test_valid_hostile_selection(void)
{
    /* args: mercTeam, targetTeam, neutralTeam, isPlayer, isCompanion, alive */
    CHECK(VrMerc_IsValidHostile(T_ALLIES, T_NAZI, T_NEUTRAL, 0, 0, 1));         /* nazi soldier */
    CHECK(VrMerc_IsValidHostile(T_ALLIES, T_MONSTER, T_NEUTRAL, 0, 0, 1));      /* zombie */
    CHECK(!VrMerc_IsValidHostile(T_ALLIES, T_NAZI, T_NEUTRAL, 1, 0, 1));        /* the player, whatever its team */
    CHECK(!VrMerc_IsValidHostile(T_ALLIES, T_ALLIES, T_NEUTRAL, 0, 0, 1));      /* friendly story NPC */
    CHECK(!VrMerc_IsValidHostile(T_ALLIES, T_NEUTRAL, T_NEUTRAL, 0, 0, 1));     /* civilian */
    CHECK(!VrMerc_IsValidHostile(T_ALLIES, T_NAZI, T_NEUTRAL, 0, 1, 1));        /* another companion */
    CHECK(!VrMerc_IsValidHostile(T_ALLIES, T_NAZI, T_NEUTRAL, 0, 0, 0));        /* already dead */
}

static void test_friendly_fire_policy(void)
{
    /* args: attackerCompanion, attackerPlayer, targetCompanion, targetPlayer,
     *       targetTeam, mercTeam, neutralTeam, targetIsCast */
    CHECK(VrMerc_DamagePolicy(1, 0, 0, 1, T_ALLIES, T_ALLIES, T_NEUTRAL, 0));      /* companion -> player */
    CHECK(VrMerc_DamagePolicy(1, 0, 0, 0, T_ALLIES, T_ALLIES, T_NEUTRAL, 1));      /* companion -> story ally */
    CHECK(VrMerc_DamagePolicy(1, 0, 0, 0, T_NEUTRAL, T_ALLIES, T_NEUTRAL, 1));     /* companion -> civilian */
    CHECK(!VrMerc_DamagePolicy(1, 0, 0, 0, T_NAZI, T_ALLIES, T_NEUTRAL, 1));       /* companion -> nazi: allowed */
    CHECK(!VrMerc_DamagePolicy(1, 0, 0, 0, T_MONSTER, T_ALLIES, T_NEUTRAL, 1));    /* companion -> zombie: allowed */
    CHECK(!VrMerc_DamagePolicy(1, 0, 0, 0, 0, T_ALLIES, T_NEUTRAL, 0));            /* companion -> crate/door */
    CHECK(VrMerc_DamagePolicy(0, 1, 1, 0, T_ALLIES, T_ALLIES, T_NEUTRAL, 1));      /* player -> companion */
    CHECK(!VrMerc_DamagePolicy(0, 1, 0, 0, T_NAZI, T_ALLIES, T_NEUTRAL, 1));       /* player -> nazi: unchanged */
    CHECK(!VrMerc_DamagePolicy(0, 0, 1, 0, T_ALLIES, T_ALLIES, T_NEUTRAL, 1));     /* nazi/world -> companion: allowed */
    CHECK(!VrMerc_DamagePolicy(0, 0, 0, 1, T_ALLIES, T_ALLIES, T_NEUTRAL, 0));     /* world/nazi -> player: unchanged */
}

/* ===================== state machine ===================== */

static vrMercInput_t baseInput(void)
{
    vrMercInput_t in;
    memset(&in, 0, sizeof(in));
    in.alive = 1;
    in.order = VRMERC_ORDER_FOLLOW;
    in.assist = 1;
    in.returnAllowed = 1;
    in.distToPlayer = 100.0f;
    in.distToHold = 0.0f;
    return in;
}

static void test_state_idle_states_are_terminal(void)
{
    vrMercInput_t in = baseInput();
    CHECK(VrMerc_NextState(VRMERC_NONE, &in, 0).state == VRMERC_NONE);
    CHECK(VrMerc_NextState(VRMERC_DEAD, &in, 0).state == VRMERC_DEAD);
    CHECK(VrMerc_NextState(VRMERC_DISMISSED, &in, 0).state == VRMERC_DISMISSED);
    in.haveTarget = 1;
    CHECK(VrMerc_NextState(VRMERC_DEAD, &in, 0).state == VRMERC_DEAD);          /* corpses do not fight */
}

static void test_state_death_and_dismiss_win(void)
{
    vrMercInput_t in = baseInput();
    in.alive = 0;
    in.haveTarget = 1;
    CHECK(VrMerc_NextState(VRMERC_ENGAGE, &in, 0).state == VRMERC_DEAD);
    CHECK(VrMerc_NextState(VRMERC_FOLLOW, &in, 0).state == VRMERC_DEAD);

    in = baseInput();
    in.dismissRequested = 1;
    in.haveTarget = 1;
    CHECK(VrMerc_NextState(VRMERC_ENGAGE, &in, 0).state == VRMERC_DISMISSED);
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_DISMISSED);
    CHECK(VrMerc_NextState(VRMERC_RETURN, &in, 0).state == VRMERC_DISMISSED);
}

static void test_state_follow_engage_and_back(void)
{
    vrMercInput_t in = baseInput();
    int s = VRMERC_FOLLOW;

    CHECK(VrMerc_NextState(s, &in, 0).state == VRMERC_FOLLOW);                  /* calm, close */

    in.haveTarget = 1;
    s = VrMerc_NextState(s, &in, 0).state;
    CHECK(s == VRMERC_ENGAGE);

    /* target lost: linger in ENGAGE, then leave */
    in.haveTarget = 0;
    in.msSinceTarget = 500;
    CHECK(VrMerc_NextState(s, &in, 0).state == VRMERC_ENGAGE);
    in.msSinceTarget = VRMERC_ENGAGE_LINGER_MS + 1;
    CHECK(VrMerc_NextState(s, &in, 0).state == VRMERC_FOLLOW);                  /* still near the player */

    /* far from the player after the fight: go back */
    in.distToPlayer = VRMERC_FOLLOW_RETURN_DIST + 50.0f;
    CHECK(VrMerc_NextState(s, &in, 0).state == VRMERC_RETURN);
}

static void test_state_assist_off_never_engages(void)
{
    vrMercInput_t in = baseInput();
    in.assist = 0;
    in.haveTarget = 1;
    CHECK(VrMerc_NextState(VRMERC_FOLLOW, &in, 0).state == VRMERC_FOLLOW);
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_FOLLOW);       /* order is FOLLOW */
    /* switching assist off mid-fight leaves ENGAGE immediately */
    in.msSinceTarget = 0;
    CHECK(VrMerc_NextState(VRMERC_ENGAGE, &in, 0).state != VRMERC_ENGAGE);
}

static void test_state_hold_orders(void)
{
    vrMercInput_t in = baseInput();

    /* HOLD order while following: switch to HOLD */
    in.order = VRMERC_ORDER_HOLD;
    CHECK(VrMerc_NextState(VRMERC_FOLLOW, &in, 0).state == VRMERC_HOLD);

    /* holding: the player walking away does not make the companion leave */
    in.distToPlayer = 5000.0f;
    in.distToHold = 20.0f;
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_HOLD);

    /* pushed away from the post beyond the leash: return to the post */
    in.distToHold = VRMERC_HOLD_LEASH + 10.0f;
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_RETURN);

    /* back at the post */
    in.distToHold = 40.0f;
    CHECK(VrMerc_NextState(VRMERC_RETURN, &in, 0).state == VRMERC_HOLD);

    /* HOLD while hostile: engages, then returns to HOLD, not FOLLOW */
    in.haveTarget = 1;
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_ENGAGE);
    in.haveTarget = 0;
    in.msSinceTarget = VRMERC_ENGAGE_LINGER_MS + 1;
    CHECK(VrMerc_NextState(VRMERC_ENGAGE, &in, 0).state == VRMERC_HOLD);

    /* FOLLOW order again */
    in = baseInput();
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_FOLLOW);
}

static void test_state_return_arrives(void)
{
    vrMercInput_t in = baseInput();
    in.distToPlayer = VRMERC_FOLLOW_ARRIVE_DIST + 100.0f;
    CHECK(VrMerc_NextState(VRMERC_RETURN, &in, 0).state == VRMERC_RETURN);      /* still walking */
    in.distToPlayer = VRMERC_FOLLOW_ARRIVE_DIST - 10.0f;
    CHECK(VrMerc_NextState(VRMERC_RETURN, &in, 0).state == VRMERC_FOLLOW);
}

/* ===================== navigation failure handling ===================== */

static void test_progress_watchdog(void)
{
    vrMercProgress_t p;
    int now = 1000;

    VrMerc_ProgressReset(&p, 1000.0f, now);
    CHECK(!VrMerc_ProgressStalled(&p, 1000.0f, now + 5000, VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS));
    /* real progress resets the clock */
    CHECK(!VrMerc_ProgressStalled(&p, 900.0f, now + 9000, VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS));
    CHECK(!VrMerc_ProgressStalled(&p, 900.0f, now + 18000, VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS));
    /* jitter below the minimum gain is not progress */
    CHECK(VrMerc_ProgressStalled(&p, 880.0f, now + 19001, VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS));
}

static void test_unreachable_return_gives_up_and_cools_down(void)
{
    /* Simulation: the player is 2000 units away behind a locked door.  The
     * companion stays at the same distance.  It must give up after the stall
     * time, hold, and not start another RETURN until the cooldown ends. */
    vrMercInput_t in = baseInput();
    vrMercProgress_t prog;
    int state = VRMERC_FOLLOW;
    int now = 0, returnAllowedAt = 0;
    int returns = 0, giveUps = 0;
    int firstGiveUpTime = -1, nextReturnTime = -1;

    in.distToPlayer = 2000.0f;

    for (now = 0; now < 120000; now += VRMERC_THINK_MS) {
        vrMercResult_t r;
        int stalled = 0;

        in.returnAllowed = now >= returnAllowedAt;
        if (state == VRMERC_RETURN) {
            stalled = VrMerc_ProgressStalled(&prog, in.distToPlayer, now,
                                             VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS);
        }
        r = VrMerc_NextState(state, &in, stalled);
        if (r.state != state) {
            if (r.state == VRMERC_RETURN) {
                returns++;
                VrMerc_ProgressReset(&prog, in.distToPlayer, now);
                if (firstGiveUpTime >= 0 && nextReturnTime < 0) {
                    nextReturnTime = now;
                }
            }
            if (r.gaveUp) {
                giveUps++;
                returnAllowedAt = now + VRMERC_RETURN_COOLDOWN_MS;
                if (firstGiveUpTime < 0) {
                    firstGiveUpTime = now;
                }
            }
            state = r.state;
        }
    }

    CHECK(giveUps >= 1);
    CHECK(firstGiveUpTime >= VRMERC_RETURN_STALL_MS);
    CHECK(firstGiveUpTime <= VRMERC_RETURN_STALL_MS + 2 * VRMERC_THINK_MS);
    CHECK(nextReturnTime >= firstGiveUpTime + VRMERC_RETURN_COOLDOWN_MS);
    /* bounded effort: over two minutes at most a handful of attempts */
    CHECK(returns <= 120000 / (VRMERC_RETURN_STALL_MS + VRMERC_RETURN_COOLDOWN_MS) + 2);
}

static void test_cooldown_blocks_follow_flip_flop(void)
{
    /* after giving up the state is HOLD with order FOLLOW; it must stay HOLD during the cooldown */
    vrMercInput_t in = baseInput();
    in.distToPlayer = 2000.0f;
    in.returnAllowed = 0;
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_HOLD);
    CHECK(VrMerc_NextState(VRMERC_FOLLOW, &in, 0).state == VRMERC_FOLLOW);       /* and does not start RETURN */
    in.returnAllowed = 1;
    CHECK(VrMerc_NextState(VRMERC_HOLD, &in, 0).state == VRMERC_FOLLOW);         /* cooldown over: follow again */
}

static void test_following_companion_returning_on_time(void)
{
    /* the player walks away at 200 u/s, the companion runs at 220 u/s: it catches up */
    vrMercInput_t in = baseInput();
    vrMercProgress_t prog;
    int state = VRMERC_FOLLOW;
    float gap = VRMERC_FOLLOW_RETURN_DIST + 200.0f;
    int arrived = -1;

    in.distToPlayer = gap;
    for (int now = 0; now < 60000; now += VRMERC_THINK_MS) {
        vrMercResult_t r;
        int stalled = 0;
        if (state == VRMERC_RETURN) {
            stalled = VrMerc_ProgressStalled(&prog, in.distToPlayer, now,
                                             VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS);
            in.distToPlayer -= (220.0f - 200.0f) * (VRMERC_THINK_MS / 1000.0f);
        }
        r = VrMerc_NextState(state, &in, stalled);
        if (r.state != state) {
            if (r.state == VRMERC_RETURN) {
                VrMerc_ProgressReset(&prog, in.distToPlayer, now);
            }
            if (r.state == VRMERC_FOLLOW && state == VRMERC_RETURN) {
                arrived = now;
            }
            state = r.state;
        }
        if (arrived >= 0) {
            break;
        }
    }
    /* closing at 20 u/s gains 200 units per stall window, far more than the 48 unit minimum,
     * so the watchdog never calls it stalled. */
    CHECK(arrived >= 0);
}

/* ===================== save / load / level change ===================== */

static void test_auto_spawn_policy(void)
{
    int now = 10000;

    CHECK(VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_OK, 0, now, 0, VRMERC_AUTOSPAWN_DELAY_MS));
    CHECK(!VrMerc_ShouldAutoSpawn(0, VRMERC_HIRE_OK, 0, now, 0, 99999));                        /* not wanted */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_LOADING, 0, now, 0, 99999));                   /* loading */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_CINEMATIC, 0, now, 0, 99999));                 /* cutscene */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_ALREADY_ACTIVE, 0, now, 0, 99999));            /* no duplicate after load */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_OK, 0, now, 0, VRMERC_AUTOSPAWN_DELAY_MS - 1)); /* level just started */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_OK, 0, now, now + 1, 99999));                  /* retry delay */
    CHECK(!VrMerc_ShouldAutoSpawn(1, VRMERC_HIRE_OK, VRMERC_SPAWN_MAX_ATTEMPTS, now, 0, 99999)); /* gives up */
}

static void test_session_reset_after_load_has_no_duplicates(void)
{
    /* Level load: the game module state is wiped, the wish (cvar) survives.
     * Run the policy over simulated frames: exactly one spawn happens. */
    vrMercHireContext_t c = okContext();
    int spawns = 0;
    int attempts = 0, next = 0;

    c.loading = 1;                                  /* savegame still settling */
    for (int now = 0; now < 30000; now += VRMERC_THINK_MS) {
        int levelAge = now;
        if (now == 6000) {
            c.loading = 0;                          /* load finished */
        }
        if (VrMerc_ShouldAutoSpawn(1, VrMerc_CanHire(&c), attempts, now, next, levelAge)) {
            spawns++;
            attempts++;
            next = now + VRMERC_SPAWN_RETRY_MS;
            c.spawnPending = 1;                     /* request queued ... */
        }
        if (now == 6600) {
            c.spawnPending = 0;                     /* ... character appeared */
            c.companionActive = 1;
        }
    }
    CHECK(spawns == 1);
}

/* ===================== model discovery and completeness ===================== */
/* Regression: "Failed to load animation scripts for model american" and the follow-up
 * "no allied soldier model found". The engine reads these files through its virtual file system
 * (all pk3 files + mod directories); the logic below decides from what was found. */

static vrMercModelFiles_t completeSkeletal(void)
{
    vrMercModelFiles_t f;
    memset(&f, 0, sizeof(f));
    f.hasAnimCfg = 1;
    f.cfgVersion = 2;
    f.hasAnimScript = 1;
    f.scriptStateMask = VRMERC_STATE_RELAXED | VRMERC_STATE_QUERY | VRMERC_STATE_ALERT | VRMERC_STATE_COMBAT;
    f.hasBodySkin = 1;
    f.hasBodyMesh = 1;
    f.hasHeadSkin = 1;
    return f;
}

static void test_model_complete_is_ok(void)
{
    vrMercModelFiles_t f = completeSkeletal();
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_OK);

    /* legacy md3 character: lower/upper parts instead of a skeletal body */
    f = completeSkeletal();
    f.hasBodySkin = f.hasBodyMesh = 0;
    f.hasLowerSkin = f.hasUpperSkin = f.hasLowerMesh = f.hasUpperMesh = 1;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_OK);

    /* QUERY is optional for a fighter */
    f = completeSkeletal();
    f.scriptStateMask = VRMERC_STATES_REQUIRED;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_OK);
}

static void test_model_missing_animation_files_are_rejected(void)
{
    /* The reported fatal error: the server cannot open wolfanim.cfg / wolfanim.script. */
    vrMercModelFiles_t f = completeSkeletal();
    f.hasAnimCfg = 0;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_ANIM_CFG);

    f = completeSkeletal();
    f.hasAnimScript = 0;                                    /* version 2 cfg: no fallback script allowed */
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_ANIM_SCRIPT);

    memset(&f, 0, sizeof(f));
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_ANIM_CFG);
}

static void test_model_legacy_default_script_rule(void)
{
    /* The loader falls back to models/players/default.script only for cfg version <= 1. */
    vrMercModelFiles_t f = completeSkeletal();
    f.hasAnimScript = 0;
    f.hasDefaultScript = 1;
    f.cfgVersion = 1;
    CHECK(VrMerc_AnimScriptAvailable(&f));
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_OK);

    f.cfgVersion = 0;                                       /* no "version" token at all */
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_OK);

    f.cfgVersion = 2;                                       /* v2 needs its own script */
    CHECK(!VrMerc_AnimScriptAvailable(&f));
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_ANIM_SCRIPT);
}

static void test_model_script_must_have_ai_states(void)
{
    /* e.g. a player-style script: files load, but the AI would stay in its bind pose */
    vrMercModelFiles_t f = completeSkeletal();
    f.scriptStateMask = VRMERC_STATE_RELAXED;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_AI_STATES);
    f.scriptStateMask = VRMERC_STATE_RELAXED | VRMERC_STATE_ALERT;      /* no COMBAT: cannot fight */
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_AI_STATES);
    f.scriptStateMask = 0;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_AI_STATES);
}

static void test_model_invalid_skin_combinations(void)
{
    vrMercModelFiles_t f = completeSkeletal();
    f.hasBodyMesh = 0;                                      /* skin without mesh: invisible character */
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_BODY);

    f = completeSkeletal();
    f.hasBodySkin = 0;                                      /* mesh without skin */
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_BODY);

    /* half a legacy body is not a body */
    f = completeSkeletal();
    f.hasBodySkin = f.hasBodyMesh = 0;
    f.hasLowerSkin = f.hasLowerMesh = f.hasUpperMesh = 1;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_BODY);

    f = completeSkeletal();
    f.hasHeadSkin = 0;
    CHECK(VrMerc_ModelProblem(&f) == VRMERC_MODEL_NO_HEAD_SKIN);
}

static void test_model_files_merged_across_pk3_sources(void)
{
    /* The engine's file system presents the union of base pk3 files and mod/mission-pack pk3
     * files (later ones override). A model whose animation files are in one pk3 and whose
     * skins come from another is complete; a model split so that something is in no pk3 is not. */
    vrMercModelFiles_t base, mod, merged;
    memset(&base, 0, sizeof(base));
    memset(&mod, 0, sizeof(mod));

    base.hasAnimCfg = 1; base.cfgVersion = 2; base.hasAnimScript = 1;
    base.scriptStateMask = VRMERC_STATES_REQUIRED;
    base.hasBodyMesh = 1;                                   /* base game: no skins for this model */
    CHECK(VrMerc_ModelProblem(&base) != VRMERC_MODEL_OK);

    mod.hasBodySkin = 1; mod.hasHeadSkin = 1;               /* mission pack adds the skins */
    merged = base;
    merged.hasBodySkin |= mod.hasBodySkin;
    merged.hasHeadSkin |= mod.hasHeadSkin;
    CHECK(VrMerc_ModelProblem(&merged) == VRMERC_MODEL_OK);

    merged = base;                                          /* mod adds only a head */
    merged.hasHeadSkin |= mod.hasHeadSkin;
    CHECK(VrMerc_ModelProblem(&merged) == VRMERC_MODEL_NO_BODY);
}

static void test_pick_first_viable(void)
{
    int p[4];

    p[0] = VRMERC_MODEL_NO_ANIM_CFG; p[1] = VRMERC_MODEL_OK; p[2] = VRMERC_MODEL_OK; p[3] = VRMERC_MODEL_NO_BODY;
    CHECK(VrMerc_PickFirstViable(p, 4, -1) == 1);           /* first usable in preference order */
    CHECK(VrMerc_PickFirstViable(p, 4, 2) == 2);            /* explicit choice wins when usable */
    CHECK(VrMerc_PickFirstViable(p, 4, 3) == 1);            /* unusable choice falls back, never used */
    CHECK(VrMerc_PickFirstViable(p, 4, 0) == 1);
    CHECK(VrMerc_PickFirstViable(p, 4, 99) == 1);           /* out of range ignored */
    CHECK(VrMerc_PickFirstViable(p, 4, -5) == 1);

    p[1] = VRMERC_MODEL_NO_ANIM_SCRIPT; p[2] = VRMERC_MODEL_NO_AI_STATES;
    CHECK(VrMerc_PickFirstViable(p, 4, -1) == -1);          /* nothing usable: no spawn at all */
    CHECK(VrMerc_PickFirstViable(p, 0, -1) == -1);
}

static void test_no_model_blocks_hire_and_auto_spawn(void)
{
    vrMercHireContext_t c = okContext();
    int result;

    c.modelUsable = 0;
    result = VrMerc_CanHire(&c);
    CHECK(result == VRMERC_HIRE_NO_MODEL);
    CHECK(!VrMerc_ShouldAutoSpawn(1, result, 0, 10000, 0, 99999));
}

static void test_skin_discovery_from_file_list(void)
{
    /* what trap_FS_GetFileList returns: consecutive NUL-terminated names */
    static const char list[] =
        "body_default.skin\0body_blue.skin\0head_default.skin\0lower_x.skin\0body_notes.skin.bak\0"
        "BODY_Red.SKIN\0body_.skin\0wolfanim.cfg";
    char names[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
    int n;

    n = VrMerc_SkinNamesFromList(list, 8, "body_", names, VRMERC_MAX_SKINS);
    CHECK(n == 3);                                          /* default, blue, Red (any case); not .bak, not empty */
    CHECK(strcmp(names[0], "default") == 0);
    CHECK(strcmp(names[1], "blue") == 0);
    CHECK(strcmp(names[2], "Red") == 0);
    CHECK(VrMerc_PickSkinIndex(names, n) == 0);

    n = VrMerc_SkinNamesFromList(list, 8, "head_", names, VRMERC_MAX_SKINS);
    CHECK(n == 1 && strcmp(names[0], "default") == 0);

    n = VrMerc_SkinNamesFromList(list, 8, "upper_", names, VRMERC_MAX_SKINS);
    CHECK(n == 0);
    CHECK(VrMerc_PickSkinIndex(names, n) == -1);

    n = VrMerc_SkinNamesFromList(list, 8, "body_", names, 2);   /* output limit respected */
    CHECK(n == 2);
}

static void test_skin_not_named_default(void)
{
    /* models whose skins are not called "default" (mission packs) are still found */
    static const char list[] = "body_trooper.skin\0body_officer.skin\0head_trooper.skin";
    char body[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
    char head[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
    int nb = VrMerc_SkinNamesFromList(list, 3, "body_", body, VRMERC_MAX_SKINS);
    int nh = VrMerc_SkinNamesFromList(list, 3, "head_", head, VRMERC_MAX_SKINS);

    CHECK(nb == 2 && nh == 1);
    CHECK(VrMerc_PickSkinIndex(body, nb) == 0);
    CHECK(strcmp(head[VrMerc_PickSkinIndex(head, nh)], "trooper") == 0);
}

static void test_legacy_common_skin(void)
{
    char lower[VRMERC_MAX_SKINS][VRMERC_NAME_LEN] = { "red", "default" };
    char upper[VRMERC_MAX_SKINS][VRMERC_NAME_LEN] = { "default", "blue" };
    char upperOther[VRMERC_MAX_SKINS][VRMERC_NAME_LEN] = { "green" };

    CHECK(VrMerc_PickCommonSkin(lower, 2, upper, 2) == 1);         /* default preferred */
    CHECK(VrMerc_PickCommonSkin(lower, 1, upperOther, 1) == -1);   /* lower and upper must share a skin */
    CHECK(VrMerc_PickCommonSkin(lower, 0, upper, 2) == -1);
    {
        char l2[VRMERC_MAX_SKINS][VRMERC_NAME_LEN] = { "a", "b" };
        char u2[VRMERC_MAX_SKINS][VRMERC_NAME_LEN] = { "b" };
        CHECK(VrMerc_PickCommonSkin(l2, 2, u2, 1) == 1);           /* otherwise the first common one */
    }
}

static void test_cfg_version_parse(void)
{
    CHECK(VrMerc_CfgVersion("version 2\nskeletal\n") == 2);
    CHECK(VrMerc_CfgVersion("// comment\nfootsteps boot\nVERSION 1\n") == 1);
    CHECK(VrMerc_CfgVersion("/* version 9 */ version 2") == 2);                   /* comments ignored */
    CHECK(VrMerc_CfgVersion("// version 9\nfootsteps boot") == 0);
    CHECK(VrMerc_CfgVersion("") == 0);
    CHECK(VrMerc_CfgVersion("version") == 0);
    CHECK(VrMerc_CfgVersion("sex male\nversion   3  \n") == 3);
}

static void test_script_state_mask(void)
{
    const char *full =
        "// animations\nSTATE RELAXED { WALK { } }\nstate query\n{ }\nState ALERT {}\nSTATE COMBAT\n{\n}\n";
    const char *playerStyle = "STATE RELAXED\n{\n}\n";
    const char *commented = "// STATE COMBAT\n/* STATE ALERT */\nSTATE RELAXED { }";
    const char *wordOnly = "WALK { animation combat 0 12 }";                          /* not a STATE block */

    CHECK(VrMerc_ScriptStateMask(full) == (VRMERC_STATE_RELAXED | VRMERC_STATE_QUERY | VRMERC_STATE_ALERT | VRMERC_STATE_COMBAT));
    CHECK(VrMerc_ScriptStateMask(playerStyle) == VRMERC_STATE_RELAXED);
    CHECK(VrMerc_ScriptStateMask(commented) == VRMERC_STATE_RELAXED);
    CHECK(VrMerc_ScriptStateMask(wordOnly) == 0);
    CHECK(VrMerc_ScriptStateMask("") == 0);
}

static void test_missing_resource_report(void)
{
    char out[512];
    vrMercModelFiles_t f;
    int n;

    memset(&f, 0, sizeof(f));
    f.cfgVersion = 2;
    n = VrMerc_DescribeMissing("american", "default", "default", &f, out, sizeof(out));
    CHECK(n == 5);
    CHECK(strstr(out, "models/players/american/wolfanim.cfg"));
    CHECK(strstr(out, "models/players/american/wolfanim.script"));
    CHECK(strstr(out, "models/players/american/body_default.skin"));
    CHECK(strstr(out, "models/players/american/body.mds"));
    CHECK(strstr(out, "models/players/american/head_default.skin"));

    f = completeSkeletal();
    CHECK(VrMerc_DescribeMissing("partisan", "default", "default", &f, out, sizeof(out)) == 0);
    CHECK(out[0] == 0);

    f = completeSkeletal();
    f.hasHeadSkin = 0;
    n = VrMerc_DescribeMissing("partisan", "default", "trooper", &f, out, sizeof(out));
    CHECK(n == 1 && strcmp(out, "models/players/partisan/head_trooper.skin") == 0);

    f = completeSkeletal();
    f.scriptStateMask = VRMERC_STATE_RELAXED;
    n = VrMerc_DescribeMissing("bj2", "default", "default", &f, out, sizeof(out));
    CHECK(n == 1 && strstr(out, "AI states"));

    /* tiny buffers never overflow */
    memset(&f, 0, sizeof(f));
    n = VrMerc_DescribeMissing("american", "default", "default", &f, out, 20);
    CHECK(strlen(out) < 20);
}

static void test_state_helpers(void)
{
    CHECK(VrMerc_StateIsActive(VRMERC_FOLLOW));
    CHECK(VrMerc_StateIsActive(VRMERC_HOLD));
    CHECK(VrMerc_StateIsActive(VRMERC_ENGAGE));
    CHECK(VrMerc_StateIsActive(VRMERC_RETURN));
    CHECK(!VrMerc_StateIsActive(VRMERC_NONE));
    CHECK(!VrMerc_StateIsActive(VRMERC_DEAD));
    CHECK(!VrMerc_StateIsActive(VRMERC_DISMISSED));
}

int main(void)
{
    test_god_default_off_never_protects();
    test_god_protects_local_player_only();
    test_god_single_player_only();
    test_god_does_not_stop_scripted_kills();
    test_god_toggle_off_restores_normal_damage();
    test_god_restore_health();

    test_hire_ok();
    test_hire_refusals();
    test_hire_duplicate_prevention();

    test_spawn_candidates();
    test_spawn_candidates_start_behind_player();
    test_spawn_candidates_deterministic_and_distinct();
    test_ground_walkable();

    test_valid_hostile_selection();
    test_friendly_fire_policy();

    test_state_idle_states_are_terminal();
    test_state_death_and_dismiss_win();
    test_state_follow_engage_and_back();
    test_state_assist_off_never_engages();
    test_state_hold_orders();
    test_state_return_arrives();
    test_state_helpers();

    test_model_complete_is_ok();
    test_model_missing_animation_files_are_rejected();
    test_model_legacy_default_script_rule();
    test_model_script_must_have_ai_states();
    test_model_invalid_skin_combinations();
    test_model_files_merged_across_pk3_sources();
    test_pick_first_viable();
    test_no_model_blocks_hire_and_auto_spawn();
    test_skin_discovery_from_file_list();
    test_skin_not_named_default();
    test_legacy_common_skin();
    test_cfg_version_parse();
    test_script_state_mask();
    test_missing_resource_report();

    test_progress_watchdog();
    test_unreachable_return_gives_up_and_cools_down();
    test_cooldown_blocks_follow_flip_flop();
    test_following_companion_returning_on_time();

    test_auto_spawn_policy();
    test_session_reset_after_load_has_no_duplicates();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
