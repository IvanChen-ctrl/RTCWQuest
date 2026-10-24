# Host tests for the M2B cinematic / pointer / transition logic.  No Android SDK/NDK needed.
#   make -C tests/vr_m2b test      (also runs the M2A input tests in tests/vr_input)

JNI_VR := ../../Projects/Android/jni/RTCWVR
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -DVRM2B_HOST_TEST -I$(JNI_VR) -I.

BIN  := build/test_vr_m2b
SRCS := test_main.c test_cinematic.c test_pointer.c test_regression.c $(JNI_VR)/VrM2bDiag.c
HDRS := harness.h $(JNI_VR)/VrCinematicLogic.h $(JNI_VR)/VrPointerLogic.h $(JNI_VR)/VrTransitionLogic.h \
        $(JNI_VR)/VrInputLogic.h $(JNI_VR)/VrM2bDiag.h

.PHONY: test clean

test: $(BIN)
	./$(BIN)
	$(MAKE) -C ../vr_input test

$(BIN): $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN) -lm

clean:
	rm -rf build
