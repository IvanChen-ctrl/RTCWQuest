# Host tests for the M4 tactical mercenary logic (g_vrtactics.h).
# No Android SDK/NDK, engine or headset needed.
#   make -C tests/vr_tactics test

GAME_SRC := ../../Projects/Android/jni/rtcw/src/game
CC       ?= cc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O1 -I$(GAME_SRC)

BIN := build/test_vr_tactics

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): test_vr_tactics.c $(GAME_SRC)/g_vrtactics.h $(GAME_SRC)/g_vrlogic.h
	@mkdir -p build
	$(CC) $(CFLAGS) test_vr_tactics.c -o $(BIN) -lm

clean:
	rm -rf build
