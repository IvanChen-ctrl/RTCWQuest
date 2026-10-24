/*
 * Host tests for the M2B cinematic / pointer / transition logic.
 *   make -C tests/vr_m2b test
 * Compiles the production headers (VrCinematicLogic.h, VrPointerLogic.h,
 * VrTransitionLogic.h, VrInputLogic.h) and the production diagnostics
 * (VrM2bDiag.c).  No headset, OpenXR runtime or engine needed.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "VrM2bDiag.h"

int m2bChecks;
int m2bFailures;

/* ---- host shims required by VrM2bDiag.c in VRM2B_HOST_TEST mode ---- */

void VrM2bDiag_ResetForTest(void);

int hostNow;
int hostLevel[2] = { 1, 1 };
#define MAX_LINES 256
char hostLines[2][MAX_LINES][320];
int  hostLineCount[2];
int  hostLastPrio[2];

int VrM2bDiagHost_Now(void) { return hostNow; }
int VrM2bDiagHost_CvarValue(int channel) { return hostLevel[channel]; }
void VrM2bDiagHost_Sink(int channel, int priority, const char *line)
{
    if (hostLineCount[channel] < MAX_LINES) {
        snprintf(hostLines[channel][hostLineCount[channel]], sizeof(hostLines[channel][0]), "%s", line);
    }
    hostLineCount[channel]++;
    hostLastPrio[channel] = priority;
}

void HostDiagReset(void)
{
    VrM2bDiag_ResetForTest();
    hostNow = 1000;
    hostLevel[0] = hostLevel[1] = 1;
    hostLineCount[0] = hostLineCount[1] = 0;
}

int HostLinesContain(int channel, const char *needle)
{
    for (int i = 0; i < hostLineCount[channel] && i < MAX_LINES; i++) {
        if (strstr(hostLines[channel][i], needle)) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    printf("-- cinematic\n");
    RunCinematicTests();
    printf("-- pointer\n");
    RunPointerTests();
    printf("-- regression\n");
    RunRegressionTests();

    printf("%d checks, %d failures\n", m2bChecks, m2bFailures);
    return m2bFailures ? 1 : 0;
}
