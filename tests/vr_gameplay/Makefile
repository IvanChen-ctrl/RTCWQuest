# Host tests for the single-player God Mode and AI mercenary decision logic.
# No Android SDK/NDK, engine or headset needed.
#   make -C tests/vr_gameplay test

GAME_SRC := ../../Projects/Android/jni/rtcw/src/game
CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -I$(GAME_SRC)

BIN := build/test_vr_gameplay

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): test_vr_gameplay.c $(GAME_SRC)/g_vrlogic.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_vr_gameplay.c -o $(BIN) -lm

clean:
	rm -rf build
