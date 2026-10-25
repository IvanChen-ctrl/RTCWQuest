/*
===========================================================================
VrLifecycleLog.c -- see VrLifecycleLog.h.
===========================================================================
*/

#include "VrLifecycleLog.h"

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void RTCWLife_Log( const char *fmt, ... ) {
	char msg[384];
	struct timespec now;
	long ms;
	va_list ap;

	clock_gettime( CLOCK_MONOTONIC, &now );
	ms = (long)( now.tv_sec * 1000L + now.tv_nsec / 1000000L );

	va_start( ap, fmt );
	vsnprintf( msg, sizeof( msg ), fmt, ap );
	va_end( ap );

	__android_log_print( ANDROID_LOG_INFO, VRLIFE_LOG_TAG, "v1 t=%ld %s", ms, msg );
}
