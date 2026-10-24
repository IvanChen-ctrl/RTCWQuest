/************************************************************************************

Filename	:	VrInputRight.c 
Content		:	Handles common controller input functionality
Created		:	September 2019
Authors		:	Simon Brown

*************************************************************************************/

#include "VrInput.h"
#include "VrInputLogic.h"
#include "VrInputDiag.h"
#include "VrPointerLogic.h"
#include "VrTransitionLogic.h"
#include "VrM2bDiag.h"

#include <string.h>

#include <src/qcommon/qcommon.h>

//keys.h
void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );

static int handIndexOf(const ovrInputStateTrackedRemote * trackedRemoteState)
{
    if (trackedRemoteState == &leftTrackedRemoteState_new) return VRI_HAND_LEFT;
    if (trackedRemoteState == &rightTrackedRemoteState_new) return VRI_HAND_RIGHT;
    return -1;
}

// Keys and "+action" commands this layer currently holds down (see VrTransitionLogic.h).
static vrHeld_t vrHeld;
static vrModeTracker_t vrModeTracker;

// Queues an engine key event and remembers whether the key is held, so a mode switch
// between the press and the release cannot leave it stuck.
void VrInput_QueueKey(int key, int down)
{
    VrHeld_Key(&vrHeld, key, down);
    Sys_QueEvent( 0, SE_KEY, key, down, 0, NULL );
}

// Releases only what this layer started and has not released yet.
void VrInput_ReleaseHeld(const char *reason)
{
    char name[VRT_ACTION_NAME_MAX + 2];
    int key;
    int actions = vrHeld.actionCount;
    int keys = vrHeld.keyCount;

    if (actions == 0 && keys == 0)
    {
        return;
    }
    while (VrHeld_TakeAction(&vrHeld, name + 1))
    {
        name[0] = '-';
        size_t len = strlen(name);
        name[len] = '\n';
        name[len + 1] = '\0';
        Cbuf_AddText( name );
    }
    while (VrHeld_TakeKey(&vrHeld, &key))
    {
        Sys_QueEvent( 0, SE_KEY, key, 0, 0, NULL );
        VrInputDiag_KeyGenerated( -1, key, 0 );
    }
    VrInputDiag_Event("release", "reason=%s actions=%d keys=%d", reason, actions, keys);
}

// Call once per input frame with the current screen-layer state.  When the mode
// changed since the previous frame (gameplay <-> menu/loading), anything still held
// is released, because the matching button edge would be handled by the other mode.
void VrInput_ModeTransition(int screen)
{
    if (VrMode_Changed(&vrModeTracker, screen))
    {
        VrInput_ReleaseHeld(screen ? "to_screen" : "to_game");
    }
}

void handleTrackedControllerButton(ovrInputStateTrackedRemote * trackedRemoteState, ovrInputStateTrackedRemote * prevTrackedRemoteState, uint32_t button, int key)
{
    int edge = VrInput_ButtonEdge(trackedRemoteState->Buttons, prevTrackedRemoteState->Buttons, button);
    if (edge >= 0)
    {
        VrInput_QueueKey( key, edge );
        VrInputDiag_KeyGenerated( handIndexOf(trackedRemoteState), key, edge );
//        Key_Event(key, (trackedRemoteState->Buttons & button) != 0, global_time);
    }
}

// Menu click for one hand: the confirm face button and the trigger both press
// the mouse key (same behaviour as two handleTrackedControllerButton calls).
void handleMenuClick(ovrInputStateTrackedRemote * trackedRemoteState, ovrInputStateTrackedRemote * prevTrackedRemoteState, uint32_t confirmButton, int mouseKey)
{
    vrKeyEvent_t events[2];
    int count = VrInput_MenuClickEvents(trackedRemoteState->Buttons, prevTrackedRemoteState->Buttons,
                                        confirmButton, ovrButton_Trigger, mouseKey, events);
    for (int i = 0; i < count; i++)
    {
        VrInput_QueueKey( events[i].key, events[i].down );
        VrInputDiag_KeyGenerated( handIndexOf(trackedRemoteState), events[i].key, events[i].down );
    }
}

void rotateAboutOrigin(float x, float y, float rotation, vec2_t out)
{
    out[0] = cosf(DEG2RAD(-rotation)) * x  +  sinf(DEG2RAD(-rotation)) * y;
    out[1] = cosf(DEG2RAD(-rotation)) * y  -  sinf(DEG2RAD(-rotation)) * x;
}

float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

#define NLF_DEADZONE 0.1
#define NLF_POWER 2.2

float nonLinearFilter(float in)
{
    float val = 0.0f;
    if (in > NLF_DEADZONE)
    {
        val = in;
        val -= NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = powf(val, NLF_POWER);
    }
    else if (in < -NLF_DEADZONE)
    {
        val = in;
        val += NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = -powf(fabsf(val), NLF_POWER);
    }

    return val;
}

void sendButtonActionSimple(const char* action)
{
    char command[256];
    snprintf( command, sizeof( command ), "%s\n", action );
    Cbuf_AddText( command );
}

qboolean between(float min, float val, float max)
{
    return (min < val) && (val < max);
}

void sendButtonAction(const char* action, long buttonDown)
{
    char command[256];
    snprintf( command, sizeof( command ), "%s\n", action );
    VrHeld_Action(&vrHeld, action, buttonDown != 0);
    if (!buttonDown)
    {
        command[0] = '-';
    }
    Cbuf_AddText( command );
}

void acquireTrackedRemotesData(double displayTime) {
    (void)displayTime;
    TBXR_UpdateControllers();
}


//YAW:  Left increase, Right decrease
void updateScopeAngles()
{
    vec3_t currentScopeAngles;
    if (vr.scopeengaged)
    {
        //Clear weapon offset
        VectorSet(vr.calculated_weaponoffset, 0, 0, 0);

        VectorSet(currentScopeAngles, vr.weaponangles[PITCH],
                  snapTurn + vr.hmdorientation_first[YAW] +
                  (vr.weaponangles[YAW] - vr.weaponangles_first[YAW]),
                  vr.hmdorientation[ROLL]);

        while (currentScopeAngles[YAW] > 180.0f) {
            currentScopeAngles[YAW] -= 360.0f;
        }
        while (currentScopeAngles[YAW] < -180.0f) {
            currentScopeAngles[YAW] += 360.0f;
        }
        VectorCopy(currentScopeAngles, vr.scopedviewangles);
    } else {
        VectorCopy(vr.hmdorientation, vr.scopedviewangles);
    }
}

void PortableMouseAbs(float x,float y);
inline float clamp(float _min, float _val, float _max)
{
    return max(min(_val, _max), _min);
}

// menuYaw must be the yaw the menu quad is placed at (vr.hmdorientation_snap[YAW]): the
// cursor is the controller direction relative to the panel, so both have to use the
// same reference.  Non-finite input leaves the cursor where it is.
void interactWithTouchScreen(float menuYaw, vec3_t controllerAngles) {
    float cursorX;
    float cursorY;

    if (!VrPointer_FromAngles(controllerAngles[YAW], controllerAngles[PITCH], menuYaw, &cursorX, &cursorY)) {
        VrPtrDiag_Rejected(controllerAngles[YAW], controllerAngles[PITCH], menuYaw);
        return;
    }
    VrPtrDiag_Sample(controllerAngles[YAW], controllerAngles[PITCH], menuYaw, vr.hmdorientation[YAW],
                     cursorX, cursorY);
    PortableMouseAbs(cursorX, cursorY);
}
