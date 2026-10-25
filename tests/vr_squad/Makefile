# Host tests for the mercenary squad logic (g_vrmove.h, g_vrsquad.h).
# No Android SDK/NDK, engine or headset needed.
#   make -C tests/vr_squad test

GAME_SRC := ../../Projects/Android/jni/rtcw/src/game
CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -I$(GAME_SRC)

BIN := build/test_vr_squad

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): test_vr_squad.c $(wildcard $(GAME_SRC)/g_vrmove.h) $(wildcard $(GAME_SRC)/g_vrsquad.h) $(GAME_SRC)/g_vrtactics.h $(GAME_SRC)/g_vrlogic.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_vr_squad.c -o $(BIN) -lm

clean:
	rm -rf build
