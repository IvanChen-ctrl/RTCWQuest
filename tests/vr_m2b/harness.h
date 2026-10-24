/* Tiny shared test harness for tests/vr_m2b. */
#ifndef M2B_HARNESS_H
#define M2B_HARNESS_H

#include <stdio.h>
#include <string.h>

extern int m2bChecks;
extern int m2bFailures;

#define CHECK(cond) do { \
        m2bChecks++; \
        if (!(cond)) { \
            m2bFailures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

void RunCinematicTests(void);
void RunPointerTests(void);
void RunRegressionTests(void);

#endif
