/*
===========================================================================
VrInputLogic.h -- pure controller input logic (no OpenXR, no engine types).

Everything here is a small static inline function so the exact code that runs
on the headset can also be compiled and unit tested on a host machine
(see tests/vr_input).  Keep this header free of engine and Android includes.
===========================================================================
*/

#ifndef VRINPUTLOGIC_H
#define VRINPUTLOGIC_H

#include <stdint.h>

#define VRI_HAND_LEFT   0
#define VRI_HAND_RIGHT  1

/* Button bits.  These MUST equal the xrButton_* / ovrButton_* values used by
 * the rest of the VR layer; OpenXrInput.c asserts that at compile time. */
#define VRI_BTN_A           0x00000001u
#define VRI_BTN_B           0x00000002u
#define VRI_BTN_RTHUMB      0x00000004u
#define VRI_BTN_X           0x00000100u
#define VRI_BTN_Y           0x00000200u
#define VRI_BTN_LTHUMB      0x00000400u
#define VRI_BTN_ENTER       0x00100000u
#define VRI_BTN_GRIP        0x04000000u
#define VRI_BTN_TRIGGER     0x20000000u
#define VRI_BTN_JOYSTICK    0x80000000u

/* Analog press threshold shared by the trigger and the grip. */
#define VRI_ANALOG_THRESHOLD 0.5f

/* Raw per-hand values as read from the OpenXR actions. */
typedef struct {
    int   menu;        /* menu click (left hand on most profiles)            */
    int   face1;       /* X on the left hand, A on the right hand            */
    int   face2;       /* Y on the left hand, B on the right hand            */
    int   stickClick;  /* thumbstick click                                   */
    float grip;        /* squeeze value (or 0/1 for click-only profiles)     */
    float trigger;     /* trigger value                                      */
    int   select;      /* khr/simple_controller select click                 */
} vrRawHandInput_t;

/* The khr/simple_controller profile has no analog trigger.  Its select click
 * stands in for a fully pulled trigger so the controller stays usable. */
static inline float VrInput_EffectiveTrigger(const vrRawHandInput_t *raw)
{
    if (raw->select && raw->trigger < 1.0f) {
        return 1.0f;
    }
    return raw->trigger;
}

static inline uint32_t VrInput_ComposeButtons(int hand, const vrRawHandInput_t *raw)
{
    uint32_t b = 0;

    if (raw->menu) {
        b |= VRI_BTN_ENTER;
    }
    if (raw->face1) {
        b |= (hand == VRI_HAND_LEFT) ? VRI_BTN_X : VRI_BTN_A;
    }
    if (raw->face2) {
        b |= (hand == VRI_HAND_LEFT) ? VRI_BTN_Y : VRI_BTN_B;
    }
    if (raw->stickClick) {
        b |= ((hand == VRI_HAND_LEFT) ? VRI_BTN_LTHUMB : VRI_BTN_RTHUMB) | VRI_BTN_JOYSTICK;
    }
    if (raw->grip > VRI_ANALOG_THRESHOLD) {
        b |= VRI_BTN_GRIP;
    }
    if (VrInput_EffectiveTrigger(raw) > VRI_ANALOG_THRESHOLD) {
        b |= VRI_BTN_TRIGGER;
    }
    return b;
}

/* Edge of one button between two button masks: 1 = pressed, 0 = released,
 * -1 = no change. */
static inline int VrInput_ButtonEdge(uint32_t newButtons, uint32_t oldButtons, uint32_t mask)
{
    uint32_t n = newButtons & mask;
    uint32_t o = oldButtons & mask;
    if (n == o) {
        return -1;
    }
    return n != 0;
}

/* Menu cursor hand selection.  Mirrors the original condition
 * "controlsLeftHanded == menu_right_handed": when true the OFF hand drives the
 * menu cursor, otherwise the DOMINANT hand does. */
static inline int VrInput_MenuUsesOffhand(int controlsLeftHanded, int menuRightHanded)
{
    return (controlsLeftHanded != 0) == (menuRightHanded != 0);
}

/* Physical hand (VRI_HAND_*) that drives the menu cursor and clicks. */
static inline int VrInput_MenuCursorHand(int controlsLeftHanded, int menuRightHanded, int playerRightHanded)
{
    int dominant = playerRightHanded ? VRI_HAND_RIGHT : VRI_HAND_LEFT;
    if (VrInput_MenuUsesOffhand(controlsLeftHanded, menuRightHanded)) {
        return (dominant == VRI_HAND_RIGHT) ? VRI_HAND_LEFT : VRI_HAND_RIGHT;
    }
    return dominant;
}

typedef struct {
    int key;
    int down;
} vrKeyEvent_t;

/* Menu click generation for one hand: the confirm face button and the trigger
 * both produce the mouse key.  Returns the number of events written to out[]
 * (at most 2), in the same order the engine receives them. */
static inline int VrInput_MenuClickEvents(uint32_t newButtons, uint32_t oldButtons,
                                          uint32_t confirmMask, uint32_t triggerMask,
                                          int mouseKey, vrKeyEvent_t out[2])
{
    int count = 0;
    int edge = VrInput_ButtonEdge(newButtons, oldButtons, confirmMask);
    if (edge >= 0) {
        out[count].key = mouseKey;
        out[count].down = edge;
        count++;
    }
    edge = VrInput_ButtonEdge(newButtons, oldButtons, triggerMask);
    if (edge >= 0) {
        out[count].key = mouseKey;
        out[count].down = edge;
        count++;
    }
    return count;
}

/* ---- Jump (B button on the dominant hand, Y when sticks are switched) ----
 *
 * The jump used to be a raw K_SPACE key event, which only works when the
 * player's config binds SPACE to +moveup.  Every other gameplay action of the
 * VR layer (crouch, use, kick, attack) issues its +/- console command directly,
 * so the jump now does too.  The route chosen at press time is latched so the
 * release always matches the press, and a held jump is released whenever the
 * jump becomes blocked (menu opened, hand moved into the backpack zone) so a
 * +moveup can never stay stuck. */
#define VRI_JUMP_ROUTE_NONE      0
#define VRI_JUMP_ROUTE_MOVEUP    1   /* "+moveup" / "-moveup" console command      */
#define VRI_JUMP_ROUTE_KEY       2   /* K_SPACE key event (cinematic interrupt)    */
#define VRI_JUMP_ROUTE_ACTIVATE  3   /* "+activate" / "-activate" (mounted gun)    */

typedef struct {
    int heldRoute;   /* route used by the press that is still active          */
    int prevDown;    /* button state seen by the previous step                */
} vrJumpState_t;

typedef struct {
    int route;       /* VRI_JUMP_ROUTE_*                                      */
    int down;        /* 1 = press, 0 = release                                */
} vrJumpAction_t;

/* One step per frame.  Writes at most one action to *out and returns 1 if it
 * did.  `blocked` must be true whenever the jump must not act (menus, backpack
 * zone, backpack weapon selection). */
static inline int VrInput_JumpStep(vrJumpState_t *st, int buttonDown, int blocked,
                                   int mountedGun, int cameraMode, vrJumpAction_t *out)
{
    int pressEdge = buttonDown && !st->prevDown;

    st->prevDown = buttonDown ? 1 : 0;

    if (!buttonDown) {
        if (st->heldRoute != VRI_JUMP_ROUTE_NONE) {
            out->route = st->heldRoute;
            out->down = 0;
            st->heldRoute = VRI_JUMP_ROUTE_NONE;
            return 1;
        }
        return 0;
    }

    /* A press that starts while blocked is never replayed: only a press edge can
     * start a jump, and prevDown stays set for as long as the button is held. */
    if (blocked) {
        if (st->heldRoute != VRI_JUMP_ROUTE_NONE) {
            out->route = st->heldRoute;
            out->down = 0;
            st->heldRoute = VRI_JUMP_ROUTE_NONE;
            return 1;
        }
        return 0;
    }

    if (pressEdge) {
        st->heldRoute = mountedGun ? VRI_JUMP_ROUTE_ACTIVATE :
                        (cameraMode ? VRI_JUMP_ROUTE_KEY : VRI_JUMP_ROUTE_MOVEUP);
        out->route = st->heldRoute;
        out->down = 1;
        return 1;
    }
    return 0;
}

/* ---- Crouch (A button on the dominant hand, X when sticks are switched) ----
 *
 * Hold semantics (the engine's +movedown / -movedown, as before M4).  The old code sent both
 * edges only while the backpack zone was inactive, so a press made while the hand was in the
 * zone was lost and a release made after the hand entered it was never sent (stuck crouch).
 * Same rules as the jump: the release always matches the press and is sent even when the
 * crouch becomes blocked; a press that starts while blocked is never replayed. */
#define VRI_DUCK_NONE            0
#define VRI_DUCK_PRESS           1   /* send +movedown                                    */
#define VRI_DUCK_RELEASE         2   /* send -movedown (button let go)                    */
#define VRI_DUCK_FORCED_RELEASE  3   /* send -movedown (menu / backpack took over)        */
#define VRI_DUCK_IGNORED         4   /* press edge while blocked: nothing sent (log only) */

typedef struct {
    int held;        /* +movedown has been sent and not yet released */
    int prevDown;
} vrDuckState_t;

static inline int VrInput_DuckStep(vrDuckState_t *st, int buttonDown, int blocked)
{
    int pressEdge = buttonDown && !st->prevDown;

    st->prevDown = buttonDown ? 1 : 0;

    if (!buttonDown) {
        if (st->held) {
            st->held = 0;
            return VRI_DUCK_RELEASE;
        }
        return VRI_DUCK_NONE;
    }
    if (blocked) {
        if (st->held) {
            st->held = 0;
            return VRI_DUCK_FORCED_RELEASE;
        }
        return pressEdge ? VRI_DUCK_IGNORED : VRI_DUCK_NONE;
    }
    if (pressEdge) {
        st->held = 1;
        return VRI_DUCK_PRESS;
    }
    return VRI_DUCK_NONE;
}

/* Name of the physical button that drives the crouch, for diagnostics.
 * The crouch uses the "primary" button 1: the dominant hand's, or the other hand's when sticks
 * are switched.  Right hand = A, left hand = X. */
static inline const char *VrInput_DuckButtonName(int switchSticks, int rightHandedDominant)
{
    int primaryIsRight = switchSticks ? !rightHandedDominant : rightHandedDominant;

    return primaryIsRight ? "A" : "X";
}

#endif /* VRINPUTLOGIC_H */
