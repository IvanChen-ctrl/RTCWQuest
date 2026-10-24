/************************************************************************************

Filename	:	VrInputRight.c 
Content		:	Handles common controller input functionality
Created		:	September 2019
Authors		:	Simon Brown

*************************************************************************************/

#include "VrInput.h"
#include "VrInputLogic.h"
#include "VrInputDiag.h"

#include <src/qcommon/qcommon.h>

//keys.h
void Sys_QueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );

static int handIndexOf(const ovrInputStateTrackedRemote * trackedRemoteState)
{
    if (trackedRemoteState == &leftTrackedRemoteState_new) return VRI_HAND_LEFT;
    if (trackedRemoteState == &rightTrackedRemoteState_new) return VRI_HAND_RIGHT;
    return -1;
}

void handleTrackedControllerButton(ovrInputStateTrackedRemote * trackedRemoteState, ovrInputStateTrackedRemote * prevTrackedRemoteState, uint32_t button, int key)
{
    int edge = VrInput_ButtonEdge(trackedRemoteState->Buttons, prevTrackedRemoteState->Buttons, button);
    if (edge >= 0)
    {
        Sys_QueEvent( 0, SE_KEY, key, edge, 0, NULL );
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
        Sys_QueEvent( 0, SE_KEY, events[i].key, events[i].down, 0, NULL );
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

void interactWithTouchScreen(float menuYaw, vec3_t controllerAngles) {
    float cursorX = -sinf(DEG2RAD(controllerAngles[YAW] - menuYaw)) + 0.5f;
    float cursorY = (float)((controllerAngles[PITCH] - 15) / 90.0) + 0.5f;

    PortableMouseAbs(cursorX, cursorY);
}
