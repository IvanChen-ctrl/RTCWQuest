/*
 * Host tests for g_vrlogic.h: single-player God Mode policy.
 *
 * Build and run: make -C tests/vr_gameplay test
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "g_vrlogic.h"

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

int main(void)
{
    test_god_default_off_never_protects();
    test_god_protects_local_player_only();
    test_god_single_player_only();
    test_god_does_not_stop_scripted_kills();
    test_god_toggle_off_restores_normal_damage();
    test_god_restore_health();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
