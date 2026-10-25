# Host tests for controller input logic and diagnostics.  No Android SDK/NDK needed.
#   make -C tests/vr_input test

JNI_VR := ../../Projects/Android/jni/RTCWVR
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -DVRINPUT_HOST_TEST -I$(JNI_VR)

BIN := build/test_vr_input

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): test_vr_input.c $(JNI_VR)/VrInputDiag.c $(JNI_VR)/VrInputDiag.h $(JNI_VR)/VrInputLogic.h $(JNI_VR)/VrCrouchLogic.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_vr_input.c $(JNI_VR)/VrInputDiag.c -o $(BIN)

clean:
	rm -rf build
