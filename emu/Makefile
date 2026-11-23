# MPC2500 emulator build. `make` builds the windowed emulator when SDL2 is installed (otherwise the headless one);
# `make hl` always builds the headless binary used by the test suite; `make asan` builds it with sanitizers.
CC      ?= gcc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
SRC     := sh3.c dsp.c board.c flash.c ata.c lcd.c soc.c audio.c panel.c fault.c dbg.c fwload.c script.c cli.c
SDL_CF  := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LD  := $(shell sdl2-config --libs 2>/dev/null)

ifeq ($(strip $(SDL_CF)),)
all: hl
	cp mpc2500emu_hl mpc2500emu
else
all: mpc2500emu
endif

mpc2500emu: $(SRC) ui_sdl.c board.h sh3.h cgrom.inc
	$(CC) $(CFLAGS) $(WARN) -DUSE_SDL $(SDL_CF) -o $@ $(SRC) ui_sdl.c $(SDL_LD) -lm

hl: mpc2500emu_hl
mpc2500emu_hl: $(SRC) board.h sh3.h cgrom.inc
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRC) -lm

asan: $(SRC) board.h sh3.h cgrom.inc
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) -o mpc2500emu_asan $(SRC) -lm

clean:
	rm -f mpc2500emu mpc2500emu_hl mpc2500emu_asan
.PHONY: all hl asan clean
