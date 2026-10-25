#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <android/native_window_jni.h>

#include "VrCommon.h"
#include "TBXR_Common.h"
#include "VrCvars.h"
#include "VrInput.h"
#include "VrLifecycleLogic.h"
#include "VrLifecycleLog.h"

#include "../rtcw/src/client/client.h"
#include "../rtcw/src/qcommon/qcommon.h"

extern int VR_main(int argc, char *argv[]);
extern void initialize_gl4es(void);
qboolean VR_GetFovTangentsForEye(int eye, float *tanLeft, float *tanRight, float *tanUp, float *tanDown);

ovrAppThread gAppThread;

cvar_t	*vr_turn_mode;
cvar_t	*vr_turn_angle;
cvar_t	*vr_reloadtimeoutms;
cvar_t	*vr_positional_factor;
cvar_t	*vr_walkdirection;
cvar_t	*vr_movement_multiplier;
cvar_t	*vr_weapon_pitchadjust;
cvar_t	*vr_lasersight;
cvar_t	*vr_control_scheme;
cvar_t	*vr_teleport;
cvar_t	*vr_virtual_stock;
cvar_t	*vr_comfort_vignette;
cvar_t	*vr_switch_sticks;
cvar_t	*vr_cinematic_stereo;
cvar_t	*vr_screen_dist;
cvar_t	*vr_gesture_triggered_use;
cvar_t	*vr_use_gesture_boundary;
cvar_t	*vr_draw_hud;
cvar_t	*vr_irl_crouch_enabled;
cvar_t	*vr_irl_crouch_to_stand_ratio;
cvar_t	*vr_haptic_intensity;
cvar_t	*vr_menu_item_touched;
cvar_t	*vr_refresh;
cvar_t	*vr_spread_reduce;

ovrInputStateTrackedRemote leftTrackedRemoteState_old;
ovrInputStateTrackedRemote leftTrackedRemoteState_new;
ovrTracking leftRemoteTracking_new;

ovrInputStateTrackedRemote rightTrackedRemoteState_old;
ovrInputStateTrackedRemote rightTrackedRemoteState_new;
ovrTracking rightRemoteTracking_new;

ovrInputStateGamepad footTrackedRemoteState_old;
ovrInputStateGamepad footTrackedRemoteState_new;

ovrDeviceID controllerIDs[2];

float remote_movementSideways;
float remote_movementForward;
float remote_movementUp;
float positional_movementSideways;
float positional_movementForward;
float snapTurn;

static JavaVM *jVM;
static jobject jniCallbackObj;
static jmethodID android_shutdown;
static jmethodID android_haptic_event;
static jmethodID android_haptic_updateevent;
static jmethodID android_haptic_stopevent;
static jmethodID android_haptic_endframe;
static jmethodID android_haptic_enable;
static jmethodID android_haptic_disable;

static void RTCWVR_JniShutdown(void);
static pthread_mutex_t gWindowLock = PTHREAD_MUTEX_INITIALIZER;
static volatile int gAppThreadStarted;
static volatile int gExitRequested;
extern vrLifecycle_t gLife;

static char **argv;
static int argc;
static long long frameIndex;

static qboolean vrInitialised;
static qboolean vrPreInitialised;
static qboolean vrCvarsInitialised;
static void *vrGlContext;

static void UnEscapeQuotes(char *arg)
{
	char *last = NULL;
	while ((last = strchr(arg, '\\')) != NULL && last[1] == '"') {
		memmove(last, last + 1, strlen(last));
	}
}

static int ParseCommandLine(char *cmdline, char **outArgv)
{
	char *bufp = cmdline;
	int outArgc = 0;
	int lastArgc = 0;

	while (*bufp) {
		while (*bufp && ((*bufp == ' ') || (*bufp == '\t'))) {
			*bufp++ = 0;
		}
		if (!*bufp) {
			break;
		}
		if (*bufp == '"') {
			bufp++;
			if (outArgv) {
				outArgv[outArgc] = bufp;
			}
			outArgc++;
			while (*bufp && ((*bufp != '"') || (bufp[-1] == '\\'))) {
				bufp++;
			}
		} else {
			if (outArgv) {
				outArgv[outArgc] = bufp;
			}
			outArgc++;
			while (*bufp && ((*bufp != ' ') && (*bufp != '\t'))) {
				bufp++;
			}
		}
		if (*bufp) {
			*bufp++ = 0;
		}
		if (outArgv && lastArgc != outArgc) {
			UnEscapeQuotes(outArgv[lastArgc]);
		}
		lastArgc = outArgc;
	}
	if (outArgv) {
		outArgv[outArgc] = NULL;
	}
	return outArgc;
}

static void *RTCWVR_AppThread(void *parm)
{
    (void)parm;

    VR_main(argc, argv);

    RTCWVR_JniShutdown();

    return NULL;
}

static void RTCWVR_InitCvars(void)
{
	if (vrCvarsInitialised) {
		return;
	}

	vr_turn_mode = Cvar_Get("vr_turn_mode", "0", CVAR_ARCHIVE);
	vr_turn_angle = Cvar_Get("vr_turn_angle", "45", CVAR_ARCHIVE);
	vr_reloadtimeoutms = Cvar_Get("vr_reloadtimeoutms", "500", CVAR_ARCHIVE);
	vr_positional_factor = Cvar_Get("vr_positional_factor", "1.0", CVAR_ARCHIVE);
	vr_walkdirection = Cvar_Get("vr_walkdirection", "0", CVAR_ARCHIVE);
	vr_movement_multiplier = Cvar_Get("vr_movement_multiplier", "1.0", CVAR_ARCHIVE);
	vr_weapon_pitchadjust = Cvar_Get("vr_weapon_pitchadjust", "0", CVAR_ARCHIVE);
	vr_lasersight = Cvar_Get("vr_lasersight", "0", CVAR_ARCHIVE);
	Cvar_Set("vr_lasersight", "0");
	vr_control_scheme = Cvar_Get("vr_control_scheme", "0", CVAR_ARCHIVE);
	vr_teleport = Cvar_Get("vr_teleport", "0", CVAR_ARCHIVE);
	vr_virtual_stock = Cvar_Get("vr_virtual_stock", "1", CVAR_ARCHIVE);
	vr_comfort_vignette = Cvar_Get("vr_comfort_vignette", "0", CVAR_ARCHIVE);
	vr_switch_sticks = Cvar_Get("vr_switch_sticks", "0", CVAR_ARCHIVE);
	vr_cinematic_stereo = Cvar_Get("vr_cinematic_stereo", "0", CVAR_ARCHIVE);
	vr_screen_dist = Cvar_Get("vr_screen_dist", "0", CVAR_ARCHIVE);
	vr_gesture_triggered_use = Cvar_Get("vr_gesture_triggered_use", "0", CVAR_ARCHIVE);
	vr_use_gesture_boundary = Cvar_Get("vr_use_gesture_boundary", "0.35", CVAR_ARCHIVE);
	vr_draw_hud = Cvar_Get("vr_draw_hud", "1", CVAR_ARCHIVE);
	vr_irl_crouch_enabled = Cvar_Get("vr_irl_crouch_enabled", "1", CVAR_ARCHIVE);
	vr_irl_crouch_to_stand_ratio = Cvar_Get("vr_irl_crouch_to_stand_ratio", "0.65", CVAR_ARCHIVE);
	vr_haptic_intensity = Cvar_Get("vr_haptic_intensity", "1.0", CVAR_ARCHIVE);
	vr_menu_item_touched = Cvar_Get("vr_menu_item_touched", "0", 0);
	vr_refresh = Cvar_Get("vr_refresh", "90", CVAR_ARCHIVE);
	vr_spread_reduce = Cvar_Get("vr_spread_reduce", "1", CVAR_ARCHIVE);

	vrCvarsInitialised = qtrue;
}

static JNIEnv *GetJniEnv(void)
{
	JNIEnv *env = NULL;
	if (jVM != NULL && (*jVM)->GetEnv(jVM, (void **)&env, JNI_VERSION_1_4) < 0) {
		(*jVM)->AttachCurrentThread(jVM, &env, NULL);
	}
	return env;
}

static void RTCWVR_JniShutdown(void)
{
    JNIEnv *env = GetJniEnv();

    if (!env || !jniCallbackObj || !android_shutdown) {
        return;
    }

    (*env)->CallVoidMethod(env, jniCallbackObj, android_shutdown);
}

static qboolean RTCWVR_WaitForAndroidSurface(void)
{
	for (int i = 0; i < 200 && (gAppThread.NativeWindow == NULL || !gAppThread.Resumed); ++i) {
		usleep(10000);
	}
	if (gAppThread.NativeWindow == NULL || !gAppThread.Resumed) {
		// not fatal: the OpenXR session does not need the Android window; startup goes on
		RTCWLife_Log("startup: no resumed Activity/Surface after 2 s (window=%p resumed=%d); continuing",
					 (void*)gAppThread.NativeWindow, (int)gAppThread.Resumed);
	}
	return (gAppThread.NativeWindow != NULL && gAppThread.Resumed);
}

// Clean application exit requested by the lifecycle logic (session EXITING, instance loss,
// unrecoverable failures).  The Java side ends the process; this is NOT a seamless resume.
void RTCWVR_RequestAppExit(const char *reason)
{
	if (gExitRequested) {
		return;
	}
	gExitRequested = 1;
	RTCWLife_Log("application exit requested (%s)", reason ? reason : "?");
	if (jniCallbackObj != NULL && android_shutdown != NULL) {
		RTCWVR_JniShutdown();
	} else {
		RTCWLife_Log("no Activity callback available: exiting the process directly");
		exit(0);
	}
}

static void RTCWVR_InitSharedState(void)
{
	memset(&vr, 0, sizeof(vr));
	playerHeight = 1.8f;
	vr.right_handed = qtrue;
	vr.menu_right_handed = qtrue;
	vr.visible_hud = qtrue;
}

qboolean RTCWVR_PreRendererInit(void)
{
	if (vrPreInitialised) {
		if (gAppState.Instance != XR_NULL_HANDLE) {
			Cvar_Set("r_customwidth", va("%d", (int)gAppState.Width));
			Cvar_Set("r_customheight", va("%d", (int)gAppState.Height));
			Cvar_Set("r_mode", "-1");
		}
		return (qboolean)(gAppState.Instance != XR_NULL_HANDLE);
	}

	RTCWVR_InitCvars();
	RTCWVR_InitSharedState();
	vrPreInitialised = qtrue;

	RTCWVR_WaitForAndroidSurface();

	TBXR_InitialiseOpenXR();
	if (gAppState.Instance == XR_NULL_HANDLE) {
		return qfalse;
	}

	Cvar_Set("r_customwidth", va("%d", (int)gAppState.Width));
	Cvar_Set("r_customheight", va("%d", (int)gAppState.Height));
	Cvar_Set("r_mode", "-1");
	Cvar_Set("com_maxfps", "0");

	return qtrue;
}

void RTCWVR_InitOnce(void)
{
	void *ctx;

	if (!vrPreInitialised || gAppState.Instance == XR_NULL_HANDLE) {
		return;
	}

	ctx = TBXR_GetCurrentGLContext();
	if (vrInitialised) {
		if (ctx == vrGlContext) {
			return;
		}
		RTCWLife_Log("GL context changed (%p -> %p): session objects are rebuilt", vrGlContext, ctx);
		TBXR_DestroySessionForReinit();
		vrInitialised = qfalse;
	} else if (gAppState.Session != XR_NULL_HANDLE) {
		// an earlier attempt stopped half way (session lost while waiting): start from a clean state
		TBXR_DestroySessionForReinit();
	}

	GlInitExtensions();
	if (!TBXR_EnterVR()) {
		TBXR_OnSessionCreateFailed();
		return;
	}
	TBXR_InitRenderer();
	TBXR_InitActions();
	TBXR_OnSessionCreated();
	if (!TBXR_WaitForSessionActive()) {
		return;     // lost or shutting down; the next frame rebuilds (or the process ends)
	}

	vrInitialised = qtrue;
	vrGlContext = ctx;
}

void RTCWVR_Init(void)
{
	if (!vrPreInitialised) {
		RTCWVR_PreRendererInit();
	}
	RTCWVR_InitOnce();
}

static qboolean VR_CalculateScreenLayer(void)
{
	static int frame = 0;
	return
		(frame++ < 100) ||
		(clc.demoplaying) ||
		(cls.state == CA_DISCONNECTED) ||
		(cls.state == CA_CHALLENGING) ||
		(cls.state == CA_CONNECTING) ||
		(cls.state == CA_CINEMATIC) ||
		(cls.state == CA_LOADING) ||
		(cls.state == CA_PRIMED) ||
		(cls.keyCatchers & KEYCATCH_UI) ||
		(cls.keyCatchers & KEYCATCH_CONSOLE);
}

qboolean VR_UseScreenLayer(void)
{
	return vr.screen;
}

qboolean RTCWVR_useScreenLayer(void)
{
	return VR_UseScreenLayer();
}

void RTCWVR_setUseScreenLayer(qboolean use)
{
	vr.screen = use;
	showingScreenLayer = use;
}

static void RTCWVR_UpdateScreenLayer(void)
{
	static qboolean lastUse = qfalse;
	qboolean use = VR_CalculateScreenLayer();

	RTCWVR_setUseScreenLayer(use);
	if (use != lastUse) {
		lastUse = use;
	}
}

float VR_GetScreenLayerDistance(void)
{
	return 2.0f + vr_screen_dist->value;
}

void VR_SetHMDOrientation(float pitch, float yaw, float roll)
{
	VectorSet(vr.hmdorientation, pitch, yaw, roll);
	VectorSubtract(vr.hmdorientation_last, vr.hmdorientation, vr.hmdorientation_delta);
	VectorCopy(vr.hmdorientation, vr.hmdorientation_last);

	if (!vr.scopeengaged && !vr.screen && !vr.cin_camera) {
		VectorCopy(vr.hmdorientation, vr.hmdorientation_first);
		VectorCopy(vr.weaponangles, vr.weaponangles_first);
	}

	float clientview_yaw = vr.clientviewangles[YAW] - vr.hmdorientation[YAW];
	vr.clientview_yaw_delta = vr.clientview_yaw_last - clientview_yaw;
	vr.clientview_yaw_last = clientview_yaw;
}

void VR_SetHMDPosition(float x, float y, float z)
{
	static qboolean s_useScreen = qfalse;
	static int frame = 0;

	VectorSet(vr.hmdposition, x, y, z);

	qboolean useScreen = VR_UseScreenLayer();
	if ((s_useScreen != useScreen) || (frame++ < 100)) {
		s_useScreen = useScreen;
		VectorSet(vr.hmdposition_snap, x, y, z);
		VectorCopy(vr.hmdorientation, vr.hmdorientation_snap);
	}

	VectorSubtract(vr.hmdposition, vr.hmdposition_snap, vr.hmdposition_offset);
	VectorSubtract(vr.hmdposition_last, vr.hmdposition, vr.hmdposition_delta);
	VectorCopy(vr.hmdposition, vr.hmdposition_last);

	if (!vr.maxHeight || vr.maxHeight < 1.0f) {
		vr.maxHeight = vr.hmdposition[1];
	}
	vr.curHeight = vr.hmdposition[1];
}

qboolean VR_GetFovTangents(float *tanLeft, float *tanRight, float *tanUp, float *tanDown)
{
	return VR_GetFovTangentsForEye(vr.eye, tanLeft, tanRight, tanUp, tanDown);
}

qboolean VR_GetFovTangentsForEye(int eye, float *tanLeft, float *tanRight, float *tanUp, float *tanDown)
{
	if (!gAppState.SessionActive || gAppState.Views == NULL) {
		return qfalse;
	}
	if (eye < 0) {
		XrFovf left = gAppState.Views[0].fov;
		XrFovf right = gAppState.Views[1].fov;
		*tanLeft = fminf(tanf(left.angleLeft), tanf(right.angleLeft));
		*tanRight = fmaxf(tanf(left.angleRight), tanf(right.angleRight));
		*tanUp = fmaxf(tanf(left.angleUp), tanf(right.angleUp));
		*tanDown = fminf(tanf(left.angleDown), tanf(right.angleDown));
		return qtrue;
	}
	if (eye < 0 || eye > 1) {
		eye = vr.eye;
	}
	XrFovf fov = gAppState.Views[eye].fov;
	*tanLeft = tanf(fov.angleLeft);
	*tanRight = tanf(fov.angleRight);
	*tanUp = tanf(fov.angleUp);
	*tanDown = tanf(fov.angleDown);
	return qtrue;
}

qboolean VR_GetProjectionZoomFactors(float refFovX, float refFovY, qboolean overrideFov, float *zoomX, float *zoomY)
{
	*zoomX = 1.0f;
	*zoomY = 1.0f;

	if (!overrideFov && !vr.cgzoommode) {
		return qfalse;
	}
	if (refFovX <= 1.0f || refFovY <= 1.0f) {
		return qfalse;
	}
	if (vr.fov_x > 1.0f) {
		*zoomX = vr.fov_x / refFovX;
	}
	if (vr.fov_y > 1.0f) {
		*zoomY = vr.fov_y / refFovY;
	}
	return qtrue;
}

float VR_GetEyeStereoSeparation(int eye)
{
	XrVector3f *l;
	XrVector3f *r;
	float dx, dy, dz;
	float ipd;

	if (!gAppState.SessionActive || gAppState.Views == NULL) {
		return 0.0f;
	}

	l = &gAppState.Views[0].pose.position;
	r = &gAppState.Views[1].pose.position;
	dx = r->x - l->x;
	dy = r->y - l->y;
	dz = r->z - l->z;
	ipd = sqrtf(dx * dx + dy * dy + dz * dz);

	return (eye == 0) ? (ipd * 0.5f) : -(ipd * 0.5f);
}

void VR_FrameSetup(void)
{
	if (gAppState.Views) {
		vr.fov = (fabsf(gAppState.Views[0].fov.angleUp) + fabsf(gAppState.Views[0].fov.angleDown)) * 180.0f / (float)M_PI;
		vr.fov_x = (fabsf(gAppState.Views[0].fov.angleLeft) + fabsf(gAppState.Views[1].fov.angleRight)) * 180.0f / (float)M_PI;
		vr.fov_y = (fabsf(gAppState.Views[0].fov.angleUp) + fabsf(gAppState.Views[0].fov.angleDown)) * 180.0f / (float)M_PI;
	} else {
		vr.fov = 90.0f;
		vr.fov_x = 90.0f;
		vr.fov_y = 90.0f;
	}
}

void VR_HandleControllerInput(void)
{
	acquireTrackedRemotesData(TBXR_GetTimeInMilliSeconds());
	HandleInput_Default(&footTrackedRemoteState_new, &footTrackedRemoteState_old,
		vr.right_handed ? &rightTrackedRemoteState_new : &leftTrackedRemoteState_new,
		vr.right_handed ? &rightTrackedRemoteState_old : &leftTrackedRemoteState_old,
		vr.right_handed ? &rightRemoteTracking_new : &leftRemoteTracking_new,
		vr.right_handed ? &leftTrackedRemoteState_new : &rightTrackedRemoteState_new,
		vr.right_handed ? &leftTrackedRemoteState_old : &rightTrackedRemoteState_old,
		vr.right_handed ? &leftRemoteTracking_new : &rightRemoteTracking_new,
		ovrButton_A, ovrButton_B, ovrButton_X, ovrButton_Y);

	leftTrackedRemoteState_old = leftTrackedRemoteState_new;
	rightTrackedRemoteState_old = rightTrackedRemoteState_new;
	footTrackedRemoteState_old = footTrackedRemoteState_new;
}

void RTCWVR_FrameSetup(void)
{
	if (TBXR_RecreatePending()) {
		// the session was lost (or its swapchains became unusable): rebuild on this thread, which owns the GL context
		TBXR_ClearRecreatePending();
		if (vrPreInitialised && gAppState.Instance != XR_NULL_HANDLE) {
			RTCWLife_Log("rebuilding the OpenXR session (recovery)");
			TBXR_DestroySessionForReinit();
			vrInitialised = qfalse;
		}
	}
	RTCWVR_InitOnce();
	RTCWVR_UpdateScreenLayer();
	TBXR_FrameSetup();
}

qboolean RTCWVR_processMessageQueue(void)
{
	return qtrue;
}

void RTCWVR_getHMDOrientation(void)
{
}

void RTCWVR_getTrackedRemotesOrientation(void)
{
	acquireTrackedRemotesData(TBXR_GetTimeInMilliSeconds());
}

void RTCWVR_processHaptics(void)
{
	TBXR_ProcessHaptics();
}

void RTCWVR_prepareEyeBuffer(int eye)
{
	TBXR_prepareEyeBuffer(eye);
}

void RTCWVR_finishEyeBuffer(int eye)
{
	TBXR_finishEyeBuffer(eye);
}

void RTCWVR_submitFrame(void)
{
	TBXR_submitFrame();
	frameIndex++;
}

long long RTCWVR_getFrameIndex(void)
{
	return frameIndex;
}

void RTCWVR_incrementFrameIndex(void)
{
	frameIndex++;
}

void RTCWVR_GetScreenRes(int *width, int *height)
{
	TBXR_GetScreenRes(width, height);
}

int GetRefresh(void)
{
	return TBXR_GetRefresh();
}

int GetRequestedRefresh(void)
{
	return TBXR_GetRefresh();
}

void RTCWVR_ResyncClientYawWithGameYaw(void)
{
	resyncClientYawWithGameYaw = 1;
}

void RTCWVR_GetMove(float *forward, float *side, float *pos_forward, float *pos_side, float *up,
					float *yaw, float *pitch, float *roll)
{
	qboolean weaponScopeView = vr.scopeengaged && vr.backpackitemactive != 3 && !vr.binocularsActive;

	if (forward) {
		*forward = weaponScopeView ? remote_movementForward / 3.0f : remote_movementForward;
	}
	if (side) {
		*side = weaponScopeView ? remote_movementSideways / 3.0f : remote_movementSideways;
	}
	if (pos_forward) {
		*pos_forward = weaponScopeView ? 0.0f : positional_movementForward;
	}
	if (pos_side) {
		*pos_side = weaponScopeView ? 0.0f : positional_movementSideways;
	}
	if (up) {
		*up = remote_movementUp;
	}
	if (yaw) {
		*yaw = weaponScopeView ? vr.scopedviewangles[YAW] : snapTurn;
	}
	if (pitch) {
		*pitch = weaponScopeView ? vr.scopedviewangles[PITCH] : vr.hmdorientation[PITCH];
	}
	if (roll) {
		*roll = weaponScopeView ? vr.scopedviewangles[ROLL] : vr.hmdorientation[ROLL];
	}
}

void RTCWVR_Vibrate(int duration, int channel, float intensity)
{
	TBXR_Vibrate(duration, channel, intensity);
}

void RTCWVR_Haptic(int duration, int channel, float intensity, char *description, float yaw, float height)
{
	RTCWVR_HapticEvent(description, channel, 0, (int)(intensity * 100.0f), yaw, height);
	RTCWVR_Vibrate(duration, channel, intensity);
}

void RTCWVR_HapticEvent(const char *event, int position, int flags, int intensity, float angle, float yHeight)
{
	JNIEnv *env = GetJniEnv();
	if (!env || !jniCallbackObj || !android_haptic_event) {
		return;
	}
	jstring eventString = (*env)->NewStringUTF(env, event ? event : "");
	(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_event, eventString, position, flags, intensity, angle, yHeight);
	(*env)->DeleteLocalRef(env, eventString);
}

void RTCWVR_HapticUpdateEvent(const char *event, int intensity, float angle)
{
	JNIEnv *env = GetJniEnv();
	if (!env || !jniCallbackObj || !android_haptic_updateevent) {
		return;
	}
	jstring eventString = (*env)->NewStringUTF(env, event ? event : "");
	(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_updateevent, eventString, intensity, angle);
	(*env)->DeleteLocalRef(env, eventString);
}

void RTCWVR_HapticEndFrame(void)
{
	JNIEnv *env = GetJniEnv();
	if (env && jniCallbackObj && android_haptic_endframe) {
		(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_endframe);
	}
}

void RTCWVR_HapticStopEvent(const char *event)
{
	JNIEnv *env = GetJniEnv();
	if (!env || !jniCallbackObj || !android_haptic_stopevent) {
		return;
	}
	jstring eventString = (*env)->NewStringUTF(env, event ? event : "");
	(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_stopevent, eventString);
	(*env)->DeleteLocalRef(env, eventString);
}

void RTCWVR_HapticEnable(void)
{
	JNIEnv *env = GetJniEnv();
	if (env && jniCallbackObj && android_haptic_enable) {
		(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_enable);
	}
}

void RTCWVR_HapticDisable(void)
{
	JNIEnv *env = GetJniEnv();
	if (env && jniCallbackObj && android_haptic_disable) {
		(*env)->CallVoidMethod(env, jniCallbackObj, android_haptic_disable);
	}
}

float radians(float deg)
{
	return DEG2RAD(deg);
}

float degrees(float rad)
{
	return RAD2DEG(rad);
}

void EFXR_SwapWindow(void)
{
	glFlush();
}

double GetTimeInMilliSeconds(void)
{
	return TBXR_GetTimeInMilliSeconds();
}

void GPUDropSync(void) {}
void GPUWaitSync(void) {}

int JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	jVM = vm;
	return JNI_VERSION_1_4;
}

JNIEXPORT jlong JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onCreate(JNIEnv *env, jclass activityClass, jobject activity,
																	   jstring commandLineParams)
{
	RTCWLife_Log("jni onCreate (app thread already started: %d)", (int)gAppThreadStarted);
	if (gAppThreadStarted) {
		// A second Activity instance in a process whose engine thread is still alive. Starting
		// another engine thread would run two engines on the same globals, so reuse the first one.
		RTCWLife_Log("onCreate ignored: the engine thread is already running");
		return (jlong)(size_t)&gAppThread;
	}

	(*env)->GetJavaVM(env, &gAppThread.JavaVm);
	gAppThread.ActivityObject = (*env)->NewGlobalRef(env, activity);
	gAppThread.ActivityClass = (*env)->NewGlobalRef(env, activityClass);

	jboolean iscopy;
	const char *arg = (*env)->GetStringUTFChars(env, commandLineParams, &iscopy);
	char *cmdLine = strdup(arg && *arg ? arg : "rtcw");
	(*env)->ReleaseStringUTFChars(env, commandLineParams, arg);

	argv = calloc(255, sizeof(char *));
	argc = ParseCommandLine(cmdLine, argv);
	if (argc == 0) {
		argc = 1;
		argv[0] = "rtcw";
	}

	initialize_gl4es();
	gAppThreadStarted = 1;
	pthread_create(&gAppThread.Thread, NULL, RTCWVR_AppThread, NULL);
	return (jlong)(size_t)&gAppThread;
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onStart(JNIEnv *env, jobject obj, jlong handle, jobject activity)
{
	(void)handle;
	RTCWLife_Log("jni onStart");
	// onStart runs again after every onStop. Keep one global reference per Activity instance; the old
	// code created a new one each time (a leak) while the engine thread was using the previous one.
	if (jniCallbackObj != NULL && (*env)->IsSameObject(env, jniCallbackObj, activity)) {
		return;
	}
	jobject newRef = (*env)->NewGlobalRef(env, activity);
	jclass callbackClass = (*env)->GetObjectClass(env, newRef);
	android_shutdown = (*env)->GetMethodID(env, callbackClass, "shutdown", "()V");
	android_haptic_event = (*env)->GetMethodID(env, callbackClass, "haptic_event", "(Ljava/lang/String;IIIFF)V");
	android_haptic_updateevent = (*env)->GetMethodID(env, callbackClass, "haptic_updateevent", "(Ljava/lang/String;IF)V");
	android_haptic_stopevent = (*env)->GetMethodID(env, callbackClass, "haptic_stopevent", "(Ljava/lang/String;)V");
	android_haptic_endframe = (*env)->GetMethodID(env, callbackClass, "haptic_endframe", "()V");
	android_haptic_enable = (*env)->GetMethodID(env, callbackClass, "haptic_enable", "()V");
	android_haptic_disable = (*env)->GetMethodID(env, callbackClass, "haptic_disable", "()V");
	(*env)->DeleteLocalRef(env, callbackClass);
	{
		jobject old = jniCallbackObj;

		jniCallbackObj = newRef;
		if (old != NULL) {
			(*env)->DeleteGlobalRef(env, old);
		}
	}
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onResume(JNIEnv *env, jobject obj, jlong handle)
{
	(void)env; (void)obj; (void)handle;
	gAppThread.Resumed = true;
	TBXR_NoteAndroidLifecycle(1);
	RTCWLife_Log("jni onResume: xr=%s running=%d surface=%d", VrLife_StateName(gLife.xrState), gLife.running, gLife.surfaceId);
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onPause(JNIEnv *env, jobject obj, jlong handle)
{
	(void)env; (void)obj; (void)handle;
	gAppThread.Resumed = false;
	TBXR_NoteAndroidLifecycle(0);
	RTCWLife_Log("jni onPause: xr=%s running=%d surface=%d", VrLife_StateName(gLife.xrState), gLife.running, gLife.surfaceId);
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onStop(JNIEnv *env, jobject obj, jlong handle)
{
	(void)env; (void)obj; (void)handle;
	RTCWLife_Log("jni onStop: xr=%s running=%d", VrLife_StateName(gLife.xrState), gLife.running);
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onDestroy(JNIEnv *env, jobject obj, jlong handle)
{
	(void)obj; (void)handle;
	RTCWLife_Log("jni onDestroy: xr=%s running=%d", VrLife_StateName(gLife.xrState), gLife.running);
	// Tell the engine thread: it leaves its wait loops and shuts the process down (it used to keep
	// running without an Activity, and a relaunch then started a second engine thread).
	TBXR_NotifyActivityDestroyed();

	pthread_mutex_lock(&gWindowLock);
	if (gAppThread.NativeWindow) {
		ANativeWindow_release(gAppThread.NativeWindow);
		gAppThread.NativeWindow = NULL;
	}
	gAppState.NativeWindow = NULL;
	pthread_mutex_unlock(&gWindowLock);
	// jniCallbackObj stays valid on purpose: the engine thread may still call haptics/shutdown
	// while it exits; the process ends right after.
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onSurfaceCreated(JNIEnv *env, jobject obj, jlong handle, jobject surface)
{
	(void)obj; (void)handle;
	ANativeWindow *window = ANativeWindow_fromSurface(env, surface);

	pthread_mutex_lock(&gWindowLock);
	if (window != NULL && window == gAppThread.NativeWindow) {
		// surfaceChanged for the surface we already hold: nothing to release or re-acquire
		pthread_mutex_unlock(&gWindowLock);
		ANativeWindow_release(window);       // drop the extra reference from ANativeWindow_fromSurface
		RTCWLife_Log("jni onSurfaceChanged: same window %p (no change)", (void*)window);
		return;
	}
	if (gAppThread.NativeWindow) {
		RTCWLife_Log("jni surface: replacing window %p with %p", (void*)gAppThread.NativeWindow, (void*)window);
		ANativeWindow_release(gAppThread.NativeWindow);
	}
	gAppThread.NativeWindow = window;
	gAppState.NativeWindow = window;
	gAppState.NativeWindowChanged = true;
	pthread_mutex_unlock(&gWindowLock);

	TBXR_NoteSurface((int)(size_t)window);
	RTCWLife_Log("jni onSurfaceCreated: window=%p", (void*)window);
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onSurfaceChanged(JNIEnv *env, jobject obj, jlong handle, jobject surface)
{
	Java_com_drbeef_rtcwquest_GLES3JNILib_onSurfaceCreated(env, obj, handle, surface);
}

JNIEXPORT void JNICALL Java_com_drbeef_rtcwquest_GLES3JNILib_onSurfaceDestroyed(JNIEnv *env, jobject obj, jlong handle)
{
	(void)env; (void)obj; (void)handle;
	pthread_mutex_lock(&gWindowLock);
	if (gAppThread.NativeWindow) {
		ANativeWindow_release(gAppThread.NativeWindow);
		gAppThread.NativeWindow = NULL;
	}
	gAppState.NativeWindow = NULL;
	gAppState.NativeWindowChanged = true;
	pthread_mutex_unlock(&gWindowLock);
	TBXR_NoteSurface(0);
	RTCWLife_Log("jni onSurfaceDestroyed: window released; XR session state=%s running=%d (rendering does not depend on it)",
				 VrLife_StateName(gLife.xrState), gLife.running);
}
