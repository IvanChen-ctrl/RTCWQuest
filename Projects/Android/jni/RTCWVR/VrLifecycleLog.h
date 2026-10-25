/*
===========================================================================
VrLifecycleLog.h -- event-based lifecycle logging, logcat tag "RTCWLifecycle".

One line per lifecycle event (Android callbacks, native message handling,
Surface identity changes, OpenXR session transitions, begin/end results, frame
failures, recovery attempts).  Never called per rendered frame.  Format:
    v1 t=<ms> <message>
Collect with:  adb logcat -s RTCWLifecycle:I
===========================================================================
*/

#ifndef VRLIFECYCLELOG_H
#define VRLIFECYCLELOG_H

#define VRLIFE_LOG_TAG "RTCWLifecycle"

void RTCWLife_Log( const char *fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );

#endif /* VRLIFECYCLELOG_H */
