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
	rm -rf $(OBJDIR) $(BIN) l26c.js l26c.wasm

# --- wasm (Emscripten) build: guarded so plain `make` never needs emcc ---
# The core is pure C and links unchanged under emcc; main.c serves as the
# entry. EXPORTED functions for the JS visualizer shell are added in the web
# phase. If emcc is not installed, print a friendly note instead of failing.
EMCC := $(shell command -v emcc 2>/dev/null)
wasm:
ifeq ($(EMCC),)
	@echo "wasm: emcc (Emscripten) not found in PATH; skipping."
	@echo "      Install Emscripten and re-run 'make wasm' to build l26c.js/.wasm."
else
	$(EMCC) $(CSTD) $(WARN) -O2 -Iinclude $(SRCS) -o l26c.js \
	    -s MODULARIZE=1 -s EXPORT_NAME=L26 \
	    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]'
	@echo "wasm: built l26c.js + l26c.wasm"
endif
