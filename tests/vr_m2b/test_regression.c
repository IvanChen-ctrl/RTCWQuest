/*
 * Regression tests: the M2A controller behaviour must survive M2B, and the new
 * held-input release logic must not disturb it.  The full M2A suite
 * (tests/vr_input) is run by `make test` as well; the checks here cover the
 * interaction between the old and the new logic.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "VrInputLogic.h"
#include "VrTransitionLogic.h"

#define T_K_ESCAPE 27
#define T_K_SPACE  32
#define T_K_MOUSE1 178

static vrRawHandInput_t rawTrigger(float v)
{
    vrRawHandInput_t r;

    memset(&r, 0, sizeof(r));
    r.trigger = v;
    return r;
}

static void testPicoTriggerAndHandIndependence(void)
{
    /* PICO 4 trigger value 0.59 (seen on hardware) presses; 0.45 releases. */
    vrRawHandInput_t pressed = rawTrigger(0.59f);
    vrRawHandInput_t released = rawTrigger(0.45f);

    CHECK(VrInput_ComposeButtons(VRI_HAND_RIGHT, &pressed) == VRI_BTN_TRIGGER);
    CHECK(VrInput_ComposeButtons(VRI_HAND_LEFT, &pressed) == VRI_BTN_TRIGGER);
    CHECK(VrInput_ComposeButtons(VRI_HAND_RIGHT, &released) == 0);

    /* Left and right masks are independent: a left press never shows up on the right. */
    uint32_t left = VrInput_ComposeButtons(VRI_HAND_LEFT, &pressed);
    uint32_t right = VrInput_ComposeButtons(VRI_HAND_RIGHT, &released);
    CHECK(left != 0 && right == 0);

    /* Face buttons keep their per-hand identity. */
    vrRawHandInput_t face;
    memset(&face, 0, sizeof(face));
    face.face1 = 1;
    face.face2 = 1;
    CHECK(VrInput_ComposeButtons(VRI_HAND_LEFT, &face) == (VRI_BTN_X | VRI_BTN_Y));
    CHECK(VrInput_ComposeButtons(VRI_HAND_RIGHT, &face) == (VRI_BTN_A | VRI_BTN_B));

    /* khr/simple_controller select stands in for the trigger. */
    vrRawHandInput_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.select = 1;
    CHECK(VrInput_ComposeButtons(VRI_HAND_RIGHT, &sel) == VRI_BTN_TRIGGER);
}

static void testMouse1PressAndReleaseThroughHeldTracking(void)
{
    vrHeld_t held;
    vrKeyEvent_t ev[2];
    int n;

    VrHeld_Init(&held);

    /* Press: one MOUSE1 down. */
    n = VrInput_MenuClickEvents(VRI_BTN_TRIGGER, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev);
    CHECK(n == 1 && ev[0].key == T_K_MOUSE1 && ev[0].down == 1);
    for (int i = 0; i < n; i++) {
        VrHeld_Key(&held, ev[i].key, ev[i].down);
    }
    CHECK(held.keyCount == 1);

    /* Release: one MOUSE1 up and nothing stays held. */
    n = VrInput_MenuClickEvents(0, VRI_BTN_TRIGGER, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev);
    CHECK(n == 1 && ev[0].key == T_K_MOUSE1 && ev[0].down == 0);
    for (int i = 0; i < n; i++) {
        VrHeld_Key(&held, ev[i].key, ev[i].down);
    }
    CHECK(held.keyCount == 0);

    /* A held button produces no events frame after frame (no repeat stream). */
    for (int f = 0; f < 1000; f++) {
        CHECK(VrInput_MenuClickEvents(VRI_BTN_TRIGGER, VRI_BTN_TRIGGER, VRI_BTN_A, VRI_BTN_TRIGGER,
                                      T_K_MOUSE1, ev) == 0);
    }
}

static void testMenuActivationHandSelection(void)
{
    /* Unchanged from M2A. */
    CHECK(VrInput_MenuCursorHand(0, 1, 1) == VRI_HAND_RIGHT);
    CHECK(VrInput_MenuCursorHand(0, 0, 1) == VRI_HAND_LEFT);
    CHECK(VrInput_MenuUsesOffhand(0, 0));
    CHECK(!VrInput_MenuUsesOffhand(0, 1));
}

static void testHeldActions(void)
{
    vrHeld_t h;
    char name[VRT_ACTION_NAME_MAX];

    VrHeld_Init(&h);
    VrHeld_Action(&h, "+attack", 1);
    VrHeld_Action(&h, "+attack", 1);             /* duplicate press is not stored twice */
    VrHeld_Action(&h, "+kick", 1);
    VrHeld_Action(&h, "weapalt", 1);             /* not holdable */
    VrHeld_Action(&h, "+", 1);
    CHECK(h.actionCount == 2);

    VrHeld_Action(&h, "+attack", 0);             /* released normally */
    CHECK(h.actionCount == 1);
    VrHeld_Action(&h, "+attack", 0);             /* release of something not held: no-op */
    CHECK(h.actionCount == 1);

    CHECK(VrHeld_TakeAction(&h, name) && strcmp(name, "kick") == 0);
    CHECK(!VrHeld_TakeAction(&h, name));

    /* Bounded: more distinct actions than slots never overflows. */
    VrHeld_Init(&h);
    for (int i = 0; i < 40; i++) {
        char a[16];
        snprintf(a, sizeof(a), "+act%d", i);
        VrHeld_Action(&h, a, 1);
    }
    CHECK(h.actionCount == VRT_MAX_ACTIONS);
    VrHeld_Action(&h, "+an_action_name_that_is_way_too_long", 1);
    CHECK(h.actionCount == VRT_MAX_ACTIONS);
}

static void testModeTransitionReleasesOnlyHeldInput(void)
{
    vrHeld_t held;
    vrModeTracker_t mode;
    char name[VRT_ACTION_NAME_MAX];
    int key;

    VrHeld_Init(&held);
    memset(&mode, 0, sizeof(mode));

    CHECK(!VrMode_Changed(&mode, 0));            /* first call only records */
    CHECK(!VrMode_Changed(&mode, 0));

    /* Gameplay: trigger fires (+attack), B jumps (SPACE). */
    VrHeld_Action(&held, "+attack", 1);
    VrHeld_Key(&held, T_K_SPACE, 1);

    /* A menu opens while both are held: exactly those two are released. */
    CHECK(VrMode_Changed(&mode, 1));
    CHECK(VrHeld_TakeAction(&held, name) && strcmp(name, "attack") == 0);
    CHECK(VrHeld_TakeKey(&held, &key) && key == T_K_SPACE);
    CHECK(!VrHeld_TakeAction(&held, name));
    CHECK(!VrHeld_TakeKey(&held, &key));

    /* No further change, no further release. */
    CHECK(!VrMode_Changed(&mode, 1));

    /* The physical release now arrives in the menu branch: harmless no-ops. */
    VrHeld_Action(&held, "+attack", 0);
    VrHeld_Key(&held, T_K_SPACE, 0);
    CHECK(held.actionCount == 0 && held.keyCount == 0);

    /* Menu -> gameplay while MOUSE1 is held (the New Game click). */
    VrHeld_Key(&held, T_K_MOUSE1, 1);
    CHECK(VrMode_Changed(&mode, 0));
    CHECK(VrHeld_TakeKey(&held, &key) && key == T_K_MOUSE1);

    /* Nothing held: a mode change releases nothing (movement etc. untouched). */
    CHECK(VrMode_Changed(&mode, 1));
    CHECK(!VrHeld_TakeAction(&held, name) && !VrHeld_TakeKey(&held, &key));
}

static void testEscapeDownOnlyThenRelease(void)
{
    /* The wheel-selector menu toggle queues ESC down without an up. */
    vrHeld_t held;
    int key;

    VrHeld_Init(&held);
    VrHeld_Key(&held, T_K_ESCAPE, 1);
    CHECK(held.keyCount == 1);
    VrHeld_Key(&held, T_K_ESCAPE, 0);
    CHECK(held.keyCount == 0);
    VrHeld_Key(&held, T_K_ESCAPE, 1);
    CHECK(VrHeld_TakeKey(&held, &key) && key == T_K_ESCAPE);
}

void RunRegressionTests(void)
{
    testPicoTriggerAndHandIndependence();
    testMouse1PressAndReleaseThroughHeldTracking();
    testMenuActivationHandSelection();
    testHeldActions();
    testModeTransitionReleasesOnlyHeldInput();
    testEscapeDownOnlyThenRelease();
}
