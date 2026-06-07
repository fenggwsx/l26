# Makefile for the L26 compiler (l26c)
#
# Layout:
#   include/   public headers (the frozen contract)
#   src/       implementation (.c)
#   tests/     L26 test programs
#   web/       (later) Emscripten/wasm shell assets
#
# Targets:
#   make / make all   -> native CLI binary ./l26c
#   make clean        -> remove build artifacts
#   make wasm         -> Emscripten build (guarded; no-op note if emcc absent)

CC      ?= cc
CSTD    := -std=c99
WARN    := -Wall -Wextra
CFLAGS  ?= $(CSTD) $(WARN) -O2 -Iinclude
LDFLAGS ?=

BIN     := l26c
SRCDIR  := src
OBJDIR  := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all clean wasm

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(BIN) l26c.js l26c.wasm web/l26.js web/l26.wasm

# --- wasm (Emscripten) build: guarded so plain `make` never needs emcc ---
# Compiles every core .c PLUS the wasm shell (src/wasm_api.c) but EXCLUDES the
# native CLI entry (src/main.c) - the wasm shell provides its own exported entry
# points. Output goes into web/ as l26.js + l26.wasm. The compiler core is
# NEVER compiled to wasm in the sense of L26 programs; what becomes wasm is the
# C TOOLCHAIN itself (decision #4), driven from the browser via these exports.
#
# Build command (emcc is not on PATH in this environment):
#   export PATH="/usr/lib/emscripten:$PATH"; make wasm
#
# If emcc is not found, print a friendly note instead of failing so plain
# `make` (native) is never affected.
EMCC := $(shell command -v emcc 2>/dev/null)

# Core sources for wasm = all sources except the native CLI entry, plus the
# wasm shell. (wasm_api.c is already in $(SRCS); we only need to drop main.c.)
WASM_SRCS := $(filter-out $(SRCDIR)/main.c,$(SRCS))

WASM_OUT  := web/l26.js

# Exported C functions (KEEPALIVE), each prefixed with '_' for the linker.
WASM_EXPORTS := _l26_compile,_l26_step,_l26_reset,_l26_run,\
_l26_feed_input,_l26_clear_input,_l26_frame_size,_malloc,_free

WASM_FLAGS := -s MODULARIZE=1 -s EXPORT_ES6=0 -s EXPORT_NAME=L26 \
	-s 'EXPORTED_RUNTIME_METHODS=["ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
	-s EXPORTED_FUNCTIONS='[$(WASM_EXPORTS)]' \
	-s ALLOW_MEMORY_GROWTH=1

wasm:
ifeq ($(EMCC),)
	@echo "wasm: emcc (Emscripten) not found in PATH; skipping."
	@echo "      Run: export PATH=\"/usr/lib/emscripten:\$$PATH\"; make wasm"
else
	@mkdir -p web
	$(EMCC) $(CSTD) $(WARN) -O2 -Iinclude $(WASM_SRCS) -o $(WASM_OUT) $(WASM_FLAGS)
	@echo "wasm: built web/l26.js + web/l26.wasm"
endif
