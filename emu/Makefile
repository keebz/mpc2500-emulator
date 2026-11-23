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
	rm -rf mpc2500emu mpc2500emu_hl mpc2500emu_asan build
# Profile-guided build (~10 % faster): builds an instrumented headless binary, trains it by booting FIRMWARE for 400M instructions with a few
# key presses, then rebuilds both binaries with the profile. FIRMWARE=path/to/firmware (default: the first file in ../firmware).
FIRMWARE ?= $(firstword $(wildcard ../firmware/*.bin ../firmware/*.BIN ../firmware/*.osx ../firmware/*.os3))
PGO_DIR  := build/pgo
pgo:
	@test -n "$(FIRMWARE)" || { echo "pgo: no firmware found; pass FIRMWARE=file"; exit 1; }
	rm -rf $(PGO_DIR) && mkdir -p $(PGO_DIR)
	for f in $(basename $(SRC)); do $(CC) -O2 -fprofile-generate -c $$f.c -o $(PGO_DIR)/$$f.o || exit 1; done
	$(CC) -fprofile-generate -o $(PGO_DIR)/train $(addprefix $(PGO_DIR)/,$(SRC:.c=.o)) -lm
	cd $(PGO_DIR) && ./train $(abspath $(FIRMWARE)) --exp train_out 300000000 "w=wheel 1 4;tap 3 3;wait 20000000;tap 0 3;wait 20000000;padhit 1 100;wait 60000000" > /dev/null 2>&1; true
	for f in $(basename $(SRC)); do cp $(PGO_DIR)/$$f.gcda $(PGO_DIR)/u_$$f.gcda 2>/dev/null; $(CC) -O2 -fprofile-use -fprofile-correction -Wno-missing-profile -c $$f.c -o $(PGO_DIR)/u_$$f.o || exit 1; done
	$(CC) -o mpc2500emu_hl $(addprefix $(PGO_DIR)/u_,$(SRC:.c=.o)) -lm
	$(CC) -O2 -fprofile-use -fprofile-correction -Wno-missing-profile -DUSE_SDL $(SDL_CF) -c ui_sdl.c -o $(PGO_DIR)/u_ui.o
	cp $(PGO_DIR)/cli.gcda $(PGO_DIR)/u_cli_sdl.gcda; $(CC) -O2 -fprofile-use -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch -DUSE_SDL $(SDL_CF) -c cli.c -o $(PGO_DIR)/u_cli_sdl.o
	$(CC) -o mpc2500emu $(addprefix $(PGO_DIR)/u_,$(filter-out cli.o,$(SRC:.c=.o))) $(PGO_DIR)/u_cli_sdl.o $(PGO_DIR)/u_ui.o $(SDL_LD) -lm

.PHONY: all hl asan clean pgo
