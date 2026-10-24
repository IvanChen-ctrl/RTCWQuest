#if !defined(vrcommon_h)
#define vrcommon_h

#include <android/log.h>
#include <stdbool.h>

#include "mathlib.h"
#include "VrClientInfo.h"
#include "openxr.h"

#define LOG_TAG "RTCWVR"

#ifndef NDEBUG
#define DEBUG 1
#endif

#define ALOGE(...) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )
#define ALOGI(...) __android_log_print( ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__ )

#if DEBUG
#define ALOGV(...) __android_log_print( ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__ )
#else
#define ALOGV(...)
#endif

qboolean rtcw_initialised;

long long global_time;

float playerHeight;
float playerYaw;

qboolean showingScreenLayer;

#define DUCK_NOTDUCKED 0
#define DUCK_BUTTON 1
#define DUCK_CROUCHED 2
int ducked;

int resyncClientYawWithGameYaw;

vr_client_info_t vr;

#define RTCW_OPENXR_COMPAT_TYPES

#define ovrButton_A 0x00000001
#define ovrButton_B 0x00000002
#define ovrButton_RThumb 0x00000004
#define ovrButton_RShoulder 0x00000008
#define ovrButton_X 0x00000100
#define ovrButton_Y 0x00000200
#define ovrButton_LThumb 0x00000400
#define ovrButton_LShoulder 0x00000800
#define ovrButton_Enter 0x00100000
#define ovrButton_Back 0x00200000
#define ovrButton_GripTrigger 0x04000000
#define ovrButton_Trigger 0x20000000
#define ovrButton_Joystick 0x80000000

#define xrButton_A ovrButton_A
#define xrButton_B ovrButton_B
#define xrButton_RThumb ovrButton_RThumb
#define xrButton_RShoulder ovrButton_RShoulder
#define xrButton_X ovrButton_X
#define xrButton_Y ovrButton_Y
#define xrButton_LThumb ovrButton_LThumb
#define xrButton_LShoulder ovrButton_LShoulder
#define xrButton_Enter ovrButton_Enter
#define xrButton_Back ovrButton_Back
#define xrButton_GripTrigger ovrButton_GripTrigger
#define xrButton_Trigger ovrButton_Trigger
#define xrButton_Joystick ovrButton_Joystick

enum {
    TOUCH_CONTROLLERS = 1,
    VIVE_CONTROLLERS,
    INDEX_CONTROLLERS,
    PICO_CONTROLLERS
};

#define VRAPI_TRACKING_STATUS_POSITION_TRACKED 0x0001
#define VRAPI_TRACKING_STATUS_POSITION_VALID 0x0002
#define VRAPI_TRACKING_STATUS_ORIENTATION_TRACKED 0x0004
#define VRAPI_TRACKING_STATUS_ORIENTATION_VALID 0x0008

typedef XrVector2f ovrVector2f;
typedef XrVector3f ovrVector3f;
typedef XrQuaternionf ovrQuatf;
typedef uint64_t ovrDeviceID;

typedef struct {
    ovrQuatf Orientation;
    ovrVector3f Position;
} ovrPosef;

typedef struct {
    ovrPosef Pose;
    ovrVector3f LinearVelocity;
} ovrPoseStatef;

typedef struct {
    ovrPoseStatef HeadPose;
    uint32_t Status;
} ovrTracking;

typedef ovrTracking ovrTracking2;

ovrTracking2 tracking;

typedef struct {
    XrVector2f LeftJoystick;
} ovrInputStateGamepad;

typedef struct {
    uint32_t Buttons;
    uint32_t Touches;
    float IndexTrigger;
    float GripTrigger;
    XrVector2f Joystick;
} ovrInputStateTrackedRemote;

double TBXR_GetTimeInMilliSeconds(void);
int TBXR_GetRefresh(void);
void TBXR_InitialiseOpenXR(void);
void TBXR_EnterVR(void);
void TBXR_InitRenderer(void);
void TBXR_InitActions(void);
void TBXR_UpdateControllers(void);
void TBXR_FrameSetup(void);
void TBXR_ProcessHaptics(void);
void TBXR_prepareEyeBuffer(int eye);
void TBXR_finishEyeBuffer(int eye);
void TBXR_submitFrame(void);
qboolean VR_GetFovTangents(float *tanLeft, float *tanRight, float *tanUp, float *tanDown);
qboolean VR_GetFovTangentsForEye(int eye, float *tanLeft, float *tanRight, float *tanUp, float *tanDown);
float VR_GetEyeStereoSeparation(int eye);
void TBXR_GetScreenRes(int *width, int *height);
void TBXR_Vibrate(int duration, int channel, float intensity);


float radians(float deg);
float degrees(float rad);
qboolean isMultiplayer();
double GetTimeInMilliSeconds();
float length(float x, float y);
float nonLinearFilter(float in);
qboolean between(float min, float val, float max);
void rotateAboutOrigin(float v1, float v2, float rotation, vec2_t out);
void QuatToYawPitchRoll(ovrQuatf q, vec3_t rotation, vec3_t out);
void handleTrackedControllerButton(ovrInputStateTrackedRemote * trackedRemoteState, ovrInputStateTrackedRemote * prevTrackedRemoteState, uint32_t button, int key);
void interactWithTouchScreen(float menuYaw, vec3_t controllerAngles);
void VrInput_QueueKey(int key, int down);
void VrInput_ReleaseHeld(const char *reason);
void VrInput_ModeTransition(int screen);
int GetRefresh();
int GetRequestedRefresh();

//Called from engine code
qboolean RTCWVR_useScreenLayer();
qboolean RTCWVR_PreRendererInit(void);
void RTCWVR_InitOnce(void);
void RTCWVR_Init(void);
void RTCWVR_GetScreenRes(int *width, int *height);
void RTCWVR_Vibrate(int duration, int channel, float intensity );
void RTCWVR_Haptic(int duration, int channel, float intensity, char *description, float yaw, float height);
void RTCWVR_HapticEvent(const char* event, int position, int flags, int intensity, float angle, float yHeight );
void RTCWVR_HapticUpdateEvent(const char* event, int intensity, float angle );
void RTCWVR_HapticEndFrame();
void RTCWVR_HapticStopEvent(const char* event);
void RTCWVR_HapticEnable();
void RTCWVR_HapticDisable();
qboolean RTCWVR_processMessageQueue();
void RTCWVR_FrameSetup();
void RTCWVR_setUseScreenLayer(qboolean use);
void RTCWVR_processHaptics();
void RTCWVR_getHMDOrientation();
void RTCWVR_getTrackedRemotesOrientation();
void RTCWVR_ResyncClientYawWithGameYaw();
void RTCWVR_incrementFrameIndex();

void RTCWVR_prepareEyeBuffer(int eye );
void RTCWVR_finishEyeBuffer(int eye );
void RTCWVR_submitFrame();

void GPUDropSync();
void GPUWaitSync();

#endif //vrcommon_h
