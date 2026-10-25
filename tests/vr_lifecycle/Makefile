# Host tests for the OpenXR session / Android lifecycle decision logic.  No SDK/NDK needed.
#   make -C tests/vr_lifecycle test

JNI_VR := ../../Projects/Android/jni/RTCWVR
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -I$(JNI_VR)

BIN := build/test_vr_lifecycle

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): test_vr_lifecycle.c $(JNI_VR)/VrLifecycleLogic.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_vr_lifecycle.c -o $(BIN)

clean:
	rm -rf build
