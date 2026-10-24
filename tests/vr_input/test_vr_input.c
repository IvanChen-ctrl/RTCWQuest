/*
 * Host tests for the controller input logic (VrInputLogic.h) and the input
 * diagnostics (VrInputDiag.c).  No headset, OpenXR runtime or engine needed.
 *
 * Build and run: make -C tests/vr_input test
 */

#include <stdio.h>
#include <string.h>

#include "VrInputLogic.h"
#include "VrInputDiag.h"

/* Engine-side key codes as the diagnostics module sees them in host mode. */
#define T_K_ESCAPE 27
#define T_K_SPACE  32
#define T_K_MOUSE1 178

void VrInputDiag_ResetForTest(void);

/* ---- host shims required by VrInputDiag.c in VRINPUT_HOST_TEST mode ---- */

static int hostNow;
static int hostCvar = 1;
#define MAX_LINES 512
static char lines[MAX_LINES][320];
static int lineCount;

int VrInputDiagHost_Now(void)      { return hostNow; }
int VrInputDiagHost_CvarValue(void) { return hostCvar; }
void VrInputDiagHost_Sink(const char *line)
{
    if (lineCount < MAX_LINES) {
        snprintf(lines[lineCount], sizeof(lines[lineCount]), "%s", line);
    }
    lineCount++;
}

/* ---- tiny harness ---- */

static int failures;
static int checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static int linesContain(const char *needle)
{
    for (int i = 0; i < lineCount && i < MAX_LINES; i++) {
        if (strstr(lines[i], needle)) {
            return 1;
        }
    }
    return 0;
}

static void resetDiag(void)
{
    VrInputDiag_ResetForTest();
    hostNow = 1000;
    hostCvar = 1;
    lineCount = 0;
}

static vrDiagHandSample_t idleSample(void)
{
    vrDiagHandSample_t s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < VRDIAG_ACT_COUNT; i++) {
        s.isActive[i] = 1;
    }
    return s;
}

/* ===================== VrInputLogic.h ===================== */

static void test_button_bits_match_quest_abi(void)
{
    /* These values are shared with the rest of the VR layer (xrButton_*).
     * OpenXrInput.c asserts the same equalities at compile time. */
    CHECK(VRI_BTN_A == 0x00000001u);
    CHECK(VRI_BTN_B == 0x00000002u);
    CHECK(VRI_BTN_RTHUMB == 0x00000004u);
    CHECK(VRI_BTN_X == 0x00000100u);
    CHECK(VRI_BTN_Y == 0x00000200u);
    CHECK(VRI_BTN_LTHUMB == 0x00000400u);
    CHECK(VRI_BTN_ENTER == 0x00100000u);
    CHECK(VRI_BTN_GRIP == 0x04000000u);
    CHECK(VRI_BTN_TRIGGER == 0x20000000u);
    CHECK(VRI_BTN_JOYSTICK == 0x80000000u);
}

static void test_button_edge(void)
{
    CHECK(VrInput_ButtonEdge(0, 0, VRI_BTN_TRIGGER) == -1);
    CHECK(VrInput_ButtonEdge(VRI_BTN_TRIGGER, 0, VRI_BTN_TRIGGER) == 1);
    CHECK(VrInput_ButtonEdge(0, VRI_BTN_TRIGGER, VRI_BTN_TRIGGER) == 0);
    CHECK(VrInput_ButtonEdge(VRI_BTN_TRIGGER, VRI_BTN_TRIGGER, VRI_BTN_TRIGGER) == -1);
    /* Other buttons changing must not report an edge for the masked button. */
    CHECK(VrInput_ButtonEdge(VRI_BTN_A, 0, VRI_BTN_TRIGGER) == -1);
    CHECK(VrInput_ButtonEdge(VRI_BTN_A | VRI_BTN_TRIGGER, VRI_BTN_TRIGGER, VRI_BTN_A) == 1);
}

static void test_trigger_threshold(void)
{
    vrRawHandInput_t raw;
    memset(&raw, 0, sizeof(raw));

    raw.trigger = 0.50f;
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_TRIGGER) == 0);
    raw.trigger = 0.51f;
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_TRIGGER) != 0);
    raw.trigger = 1.0f;
    CHECK((VrInput_ComposeButtons(VRI_HAND_LEFT, &raw) & VRI_BTN_TRIGGER) != 0);

    raw.trigger = 0.0f;
    raw.grip = 0.50f;
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_GRIP) == 0);
    raw.grip = 0.75f;
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_GRIP) != 0);
}

static void test_compose_buttons_per_hand(void)
{
    vrRawHandInput_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.menu = raw.face1 = raw.face2 = raw.stickClick = 1;

    uint32_t left = VrInput_ComposeButtons(VRI_HAND_LEFT, &raw);
    CHECK(left == (VRI_BTN_ENTER | VRI_BTN_X | VRI_BTN_Y | VRI_BTN_LTHUMB | VRI_BTN_JOYSTICK));

    uint32_t right = VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw);
    CHECK(right == (VRI_BTN_ENTER | VRI_BTN_A | VRI_BTN_B | VRI_BTN_RTHUMB | VRI_BTN_JOYSTICK));

    memset(&raw, 0, sizeof(raw));
    CHECK(VrInput_ComposeButtons(VRI_HAND_LEFT, &raw) == 0);
    CHECK(VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) == 0);
}

static void test_simple_controller_select_acts_as_trigger(void)
{
    /* khr/simple_controller has no analog trigger.  If the runtime falls back
     * to it, select/click is the only button, and it must still act as the
     * trigger so the menu can be clicked. */
    vrRawHandInput_t raw;
    memset(&raw, 0, sizeof(raw));

    raw.select = 1;
    CHECK(VrInput_EffectiveTrigger(&raw) == 1.0f);
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_TRIGGER) != 0);

    raw.select = 0;
    CHECK(VrInput_EffectiveTrigger(&raw) == 0.0f);
    CHECK((VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw) & VRI_BTN_TRIGGER) == 0);

    /* An analog trigger value is never reduced by the select fold. */
    raw.select = 0;
    raw.trigger = 0.8f;
    CHECK(VrInput_EffectiveTrigger(&raw) == 0.8f);
}

static void test_menu_cursor_hand_selection(void)
{
    /* Defaults: right-handed scheme, menu_right_handed set -> right hand. */
    CHECK(VrInput_MenuCursorHand(0, 1, 1) == VRI_HAND_RIGHT);
    CHECK(VrInput_MenuUsesOffhand(0, 1) == 0);

    /* The off-hand trigger toggles menu_right_handed -> cursor moves to left. */
    CHECK(VrInput_MenuCursorHand(0, 0, 1) == VRI_HAND_LEFT);
    CHECK(VrInput_MenuUsesOffhand(0, 0) == 1);

    /* Left-handed scheme: dominant is the left hand. */
    CHECK(VrInput_MenuCursorHand(1, 0, 0) == VRI_HAND_LEFT);
    CHECK(VrInput_MenuCursorHand(1, 1, 0) == VRI_HAND_RIGHT);

    /* The original condition was "controlsLeftHanded == menu_right_handed". */
    for (int c = 0; c <= 1; c++) {
        for (int m = 0; m <= 1; m++) {
            CHECK(VrInput_MenuUsesOffhand(c, m) == (c == m));
        }
    }
}

static void test_menu_click_events(void)
{
    vrKeyEvent_t ev[2];

    /* Nothing changed. */
    CHECK(VrInput_MenuClickEvents(0, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 0);
    CHECK(VrInput_MenuClickEvents(VRI_BTN_TRIGGER, VRI_BTN_TRIGGER, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 0);

    /* Trigger press, then release. */
    CHECK(VrInput_MenuClickEvents(VRI_BTN_TRIGGER, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 1);
    CHECK(ev[0].key == T_K_MOUSE1 && ev[0].down == 1);
    CHECK(VrInput_MenuClickEvents(0, VRI_BTN_TRIGGER, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 1);
    CHECK(ev[0].key == T_K_MOUSE1 && ev[0].down == 0);

    /* Confirm face button press. */
    CHECK(VrInput_MenuClickEvents(VRI_BTN_A, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 1);
    CHECK(ev[0].key == T_K_MOUSE1 && ev[0].down == 1);

    /* Both pressed in the same frame: two events, confirm button first. */
    CHECK(VrInput_MenuClickEvents(VRI_BTN_A | VRI_BTN_TRIGGER, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 2);
    CHECK(ev[0].down == 1 && ev[1].down == 1);

    /* An unrelated button never produces a click. */
    CHECK(VrInput_MenuClickEvents(VRI_BTN_B | VRI_BTN_GRIP, 0, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev) == 0);
}

static void test_menu_click_from_trigger_values(void)
{
    /* Full path as on the headset: trigger values -> Buttons -> key events. */
    const float values[] = {0.0f, 0.2f, 0.45f, 0.55f, 0.9f, 1.0f, 0.7f, 0.4f, 0.0f};
    uint32_t prev = 0;
    int downs = 0;
    int ups = 0;

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        vrRawHandInput_t raw;
        vrKeyEvent_t ev[2];
        memset(&raw, 0, sizeof(raw));
        raw.trigger = values[i];

        uint32_t cur = VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw);
        int n = VrInput_MenuClickEvents(cur, prev, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev);
        for (int e = 0; e < n; e++) {
            if (ev[e].down) downs++; else ups++;
        }
        prev = cur;
    }
    CHECK(downs == 1);
    CHECK(ups == 1);
}

static void test_menu_click_from_simple_controller_select(void)
{
    uint32_t prev = 0;
    int downs = 0;
    int ups = 0;

    for (int frame = 0; frame < 6; frame++) {
        vrRawHandInput_t raw;
        vrKeyEvent_t ev[2];
        memset(&raw, 0, sizeof(raw));
        raw.select = (frame >= 2 && frame <= 3);

        uint32_t cur = VrInput_ComposeButtons(VRI_HAND_RIGHT, &raw);
        int n = VrInput_MenuClickEvents(cur, prev, VRI_BTN_A, VRI_BTN_TRIGGER, T_K_MOUSE1, ev);
        for (int e = 0; e < n; e++) {
            if (ev[e].down) downs++; else ups++;
        }
        prev = cur;
    }
    CHECK(downs == 1);
    CHECK(ups == 1);
}

/* ===================== VrInputDiag.c ===================== */

static void test_diag_line_format(void)
{
    resetDiag();
    VrInputDiag_Event("sample", "a=%d b=%s", 7, "x");
    CHECK(lineCount == 1);
    CHECK(strcmp(lines[0], "v1 t=1000 ev=sample a=7 b=x") == 0);

    lineCount = 0;
    VrInputDiag_Event("bare", "%s", "");
    CHECK(strcmp(lines[0], "v1 t=1000 ev=bare") == 0);
}

static void test_diag_disabled_is_silent(void)
{
    resetDiag();
    hostCvar = 0;
    vrDiagHandSample_t s = idleSample();
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    s.value[VRDIAG_ACT_TRIGGER] = 1.0f;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    VrInputDiag_KeyGenerated(VRI_HAND_RIGHT, T_K_MOUSE1, 1);
    VrInputDiag_SyncResult(0, "XR_SUCCESS");
    CHECK(lineCount == 0);
}

static void test_diag_logs_only_changes(void)
{
    resetDiag();
    vrDiagHandSample_t s = idleSample();

    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=active hand=R"));

    /* 300 unchanged frames produce nothing. */
    for (int i = 0; i < 300; i++) {
        hostNow += 11;
        VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    }
    CHECK(lineCount == 1);
}

static void test_diag_trigger_transitions(void)
{
    resetDiag();
    vrDiagHandSample_t s = idleSample();
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    lineCount = 0;

    s.value[VRDIAG_ACT_TRIGGER] = 0.3f;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(lineCount == 0);                      /* analog change below threshold: silent */

    s.value[VRDIAG_ACT_TRIGGER] = 0.8f;
    s.buttons = VRI_BTN_TRIGGER;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=btn hand=R name=trigger down=1 val=0.80 active=1 buttons=0x20000000"));

    s.value[VRDIAG_ACT_TRIGGER] = 0.0f;
    s.buttons = 0;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(linesContain("ev=btn hand=R name=trigger down=0 val=0.00 active=1 buttons=0x00000000"));
    CHECK(!linesContain("trigpeak"));           /* it did cross the threshold */
}

static void test_diag_face_buttons_use_hand_names(void)
{
    resetDiag();
    vrDiagHandSample_t s = idleSample();
    VrInputDiag_HandSample(VRI_HAND_LEFT, &s);
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    lineCount = 0;

    s.value[VRDIAG_ACT_FACE1] = 1.0f;
    VrInputDiag_HandSample(VRI_HAND_LEFT, &s);
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(linesContain("hand=L name=X down=1"));
    CHECK(linesContain("hand=R name=A down=1"));

    lineCount = 0;
    s.value[VRDIAG_ACT_FACE2] = 1.0f;
    VrInputDiag_HandSample(VRI_HAND_LEFT, &s);
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(linesContain("hand=L name=Y down=1"));
    CHECK(linesContain("hand=R name=B down=1"));
}

static void test_diag_reports_inactive_actions(void)
{
    resetDiag();
    vrDiagHandSample_t s = idleSample();
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    lineCount = 0;

    s.isActive[VRDIAG_ACT_TRIGGER] = 0;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=active hand=R mask=0xfe actions=grip|A|B|menu|stickclick|select|stick"));
}

static void test_diag_all_inactive_is_visible(void)
{
    resetDiag();
    vrDiagHandSample_t s;
    memset(&s, 0, sizeof(s));                   /* nothing active at all */
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(linesContain("ev=active hand=R mask=0x00 actions=none"));
}

static void test_diag_trigger_peak_below_threshold(void)
{
    resetDiag();
    vrDiagHandSample_t s = idleSample();
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    lineCount = 0;

    s.value[VRDIAG_ACT_TRIGGER] = 0.35f;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    s.value[VRDIAG_ACT_TRIGGER] = 0.0f;
    VrInputDiag_HandSample(VRI_HAND_RIGHT, &s);
    CHECK(linesContain("ev=trigpeak hand=R max=0.35 note=below_press_threshold"));
}

static void test_diag_result_events_are_deduplicated(void)
{
    resetDiag();
    VrInputDiag_SyncResult(0, "XR_SUCCESS");
    VrInputDiag_SyncResult(0, "XR_SUCCESS");
    CHECK(lineCount == 1);
    VrInputDiag_SyncResult(8, "XR_SESSION_NOT_FOCUSED");
    CHECK(lineCount == 2);
    CHECK(linesContain("ev=sync result=8 name=XR_SESSION_NOT_FOCUSED"));

    lineCount = 0;
    VrInputDiag_Profile(VRI_HAND_RIGHT, "/interaction_profiles/bytedance/pico4_controller");
    VrInputDiag_Profile(VRI_HAND_RIGHT, "/interaction_profiles/bytedance/pico4_controller");
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=profile hand=R path=/interaction_profiles/bytedance/pico4_controller"));
    VrInputDiag_Profile(VRI_HAND_LEFT, "");
    CHECK(lineCount == 1);                      /* "no profile yet" on first look is not news */
}

static void test_diag_setup_events(void)
{
    resetDiag();
    VrInputDiag_Extension("XR_BD_controller_interaction", 1);
    VrInputDiag_SuggestResult("/interaction_profiles/bytedance/pico4_controller", -22, "XR_ERROR_PATH_UNSUPPORTED", 21, 0);
    VrInputDiag_AttachResult(0, "XR_SUCCESS");
    CHECK(linesContain("ev=ext name=XR_BD_controller_interaction enabled=1"));
    CHECK(linesContain("ev=suggest profile=bytedance/pico4_controller result=-22 name=XR_ERROR_PATH_UNSUPPORTED bindings=21 retry=0"));
    CHECK(linesContain("ev=attach result=0 name=XR_SUCCESS"));
}

static void test_diag_menu_hand_and_keys(void)
{
    resetDiag();
    VrInputDiag_MenuHand(VRI_HAND_RIGHT, 1, 0, 1);
    VrInputDiag_MenuHand(VRI_HAND_RIGHT, 1, 0, 1);   /* unchanged */
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=menuhand cursor=R menu_right_handed=1 controls_left=0 screen=1"));

    lineCount = 0;
    VrInputDiag_KeyGenerated(VRI_HAND_RIGHT, T_K_MOUSE1, 1);
    VrInputDiag_KeyGenerated(VRI_HAND_RIGHT, T_K_MOUSE1, 0);
    CHECK(linesContain("ev=key stage=gen hand=R name=MOUSE1 code=178 down=1"));
    CHECK(linesContain("ev=key stage=gen hand=R name=MOUSE1 code=178 down=0"));

    lineCount = 0;
    VrInputDiag_KeyDelivered(T_K_MOUSE1, 1, 0x2);
    CHECK(linesContain("ev=key stage=deliver name=MOUSE1 code=178 down=1 catchers=0x2 route=ui"));
    VrInputDiag_KeyDelivered(T_K_MOUSE1, 1, 0x0);
    CHECK(linesContain("route=game"));
    VrInputDiag_KeyDelivered(T_K_MOUSE1, 1, 0x1);
    CHECK(linesContain("route=console"));

    lineCount = 0;
    VrInputDiag_KeyDelivered('a', 1, 0x2);           /* unrelated keys are ignored */
    CHECK(lineCount == 0);
}

static void test_diag_heartbeat(void)
{
    resetDiag();
    VrInputDiag_Profile(VRI_HAND_RIGHT, "/interaction_profiles/khr/simple_controller");
    VrInputDiag_KeyGenerated(VRI_HAND_RIGHT, T_K_MOUSE1, 1);
    VrInputDiag_KeyDelivered(T_K_MOUSE1, 1, 0x2);
    lineCount = 0;

    VrInputDiag_Heartbeat();                          /* starts the timer */
    hostNow += 4000;
    VrInputDiag_Heartbeat();
    CHECK(lineCount == 0);                            /* 5 s not reached */

    hostNow += 1500;
    VrInputDiag_Heartbeat();
    CHECK(lineCount == 1);
    CHECK(linesContain("ev=hb frames=3 profileL=none profileR=khr/simple_controller sync=0 mouse1_gen=1 mouse1_deliver=1 dropped_total=0"));
}

static void test_diag_rate_limit(void)
{
    resetDiag();

    /* 200 events in the same millisecond: only the bucket capacity gets out. */
    for (int i = 0; i < 200; i++) {
        VrInputDiag_Event("flood", "i=%d", i);
    }
    CHECK(lineCount == 40);

    /* One second later the bucket has refilled; the loss is reported once. */
    hostNow += 1000;
    VrInputDiag_Event("after", "x=1");
    CHECK(linesContain("ev=drop n=160"));
    CHECK(linesContain("ev=after x=1"));
}

int main(void)
{
    test_button_bits_match_quest_abi();
    test_button_edge();
    test_trigger_threshold();
    test_compose_buttons_per_hand();
    test_simple_controller_select_acts_as_trigger();
    test_menu_cursor_hand_selection();
    test_menu_click_events();
    test_menu_click_from_trigger_values();
    test_menu_click_from_simple_controller_select();

    test_diag_line_format();
    test_diag_disabled_is_silent();
    test_diag_logs_only_changes();
    test_diag_trigger_transitions();
    test_diag_face_buttons_use_hand_names();
    test_diag_reports_inactive_actions();
    test_diag_all_inactive_is_visible();
    test_diag_trigger_peak_below_threshold();
    test_diag_result_events_are_deduplicated();
    test_diag_setup_events();
    test_diag_menu_hand_and_keys();
    test_diag_heartbeat();
    test_diag_rate_limit();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
