# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc

# Linux and macOS (Darwin) differ in a few flags (see libs/platform.mk): the
# machine tuning option, and how the executable exports its symbols to the
# native libraries it loads.
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
NATIVE_FLAGS = -mcpu=native
else
NATIVE_FLAGS = -march=native -mtune=native
endif
LDBASE = -lpthread -lm -ldl -Wl,-export_dynamic
SHARED = -dynamiclib -undefined dynamic_lookup
else
NATIVE_FLAGS = -march=native -mtune=native
LDBASE = -lpthread -lm -ldl -rdynamic
SHARED = -shared
endif

CFLAGS = -std=c11 -O3 $(NATIVE_FLAGS) -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE
LDFLAGS = $(LDBASE)

# The REPL uses readline for line editing when its headers are installed
# (src/sea_main.c checks with __has_include); link it in that case.
HAVE_READLINE := $(shell printf '\043include <stdio.h>\n\043include <readline/readline.h>\n' | \
                   $(CC) -E -x c - >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_READLINE),1)
LDFLAGS += -lreadline
endif

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

SRCS    = $(wildcard $(SRC_DIR)/*.c)
# Native libraries built into the binary as modules (json = require("json")
# with no .so): libs/<name>/<name>.c, its sea_open renamed sea_open_<name>
EMBED_LIBS = json csv
OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o) $(EMBED_LIBS:%=$(OBJ_DIR)/lib_%.o)
TARGET  = $(BIN_DIR)/sea

# Separate JIT-enabled binary (coexists with the plain interpreter bin/sea),
# built into its own object dir so the two never clobber each other.
JIT_OBJ_DIR = obj-jit
JIT_OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(JIT_OBJ_DIR)/%.o) $(EMBED_LIBS:%=$(JIT_OBJ_DIR)/lib_%.o)
JIT_TARGET  = $(BIN_DIR)/sea-jit

.PHONY: all clean test test-jit test-seac test-build test-dwe test-format test-license install uninstall asan debug noreadline avx2 lib jemalloc test-jemalloc jit sea-jit

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# the embedded libraries: obj/lib_<name>.o from libs/<name>/<name>.c
define EMBED_RULE
$$(OBJ_DIR)/lib_$(1).o: libs/$(1)/$(1).c | $$(OBJ_DIR)
	$$(CC) $$(CFLAGS) -I$$(SRC_DIR) -Dsea_open=sea_open_$(1) -MMD -MP -c -o $$@ $$<
$$(JIT_OBJ_DIR)/lib_$(1).o: libs/$(1)/$(1).c | $$(JIT_OBJ_DIR)
	$$(CC) $$(CFLAGS) -I$$(SRC_DIR) -DSEA_JIT -Dsea_open=sea_open_$(1) -MMD -MP -c -o $$@ $$<
endef
$(foreach l,$(EMBED_LIBS),$(eval $(call EMBED_RULE,$(l))))

-include $(OBJS:.o=.d)

# Standalone JIT binary at bin/sea-jit (separate objects under obj-jit/).
sea-jit: $(JIT_TARGET)

$(JIT_TARGET): $(JIT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -DSEA_JIT -o $@ $^ $(LDFLAGS)

$(JIT_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(JIT_OBJ_DIR)
	$(CC) $(CFLAGS) -DSEA_JIT -MMD -MP -c -o $@ $<

-include $(JIT_OBJS:.o=.d)

# The `sea fmt`, `sea doc`, ... tools: Sea scripts in tools/, embedded in the
# binary as byte arrays (gen/sea_tools.h, included by sea_main.c). Modules
# that `require("sea/...")` finds without a file: gen/sea_modules.h, included
# by sea_stdlib.c (name=file).
TOOLS = $(wildcard tools/*.sea)
MODULES = sea/test=libs/test/test.sea sea/check=tools/lib/check.sea sea/stream=libs/stream/stream.sea sea/flags=libs/flags/flags.sea sea/log=libs/log/log.sea sea/dotenv=libs/dotenv/dotenv.sea
GEN_DIR = gen
CFLAGS += -I$(GEN_DIR)

$(GEN_DIR)/sea_tools.h: $(TOOLS) tools/embed.sh | $(GEN_DIR)
	sh tools/embed.sh SEA_TOOLS $(TOOLS) > $@

$(GEN_DIR)/sea_modules.h: $(foreach m,$(MODULES),$(lastword $(subst =, ,$(m)))) tools/embed.sh | $(GEN_DIR)
	sh tools/embed.sh SEA_MODULES $(MODULES) > $@

$(OBJ_DIR)/sea_main.o $(JIT_OBJ_DIR)/sea_main.o: $(GEN_DIR)/sea_tools.h
$(OBJ_DIR)/sea_stdlib.o $(JIT_OBJ_DIR)/sea_stdlib.o: $(GEN_DIR)/sea_modules.h

$(BIN_DIR) $(OBJ_DIR) $(JIT_OBJ_DIR) $(GEN_DIR):
	mkdir -p $@

avx2: CFLAGS += -mavx2
avx2: $(TARGET)

# Build with jemalloc allocator (sudo apt install libjemalloc-dev)
# sea_free() passes size → je_sdallocx skips the internal size-class lookup
jemalloc: CFLAGS  += -DUSE_JEMALLOC
jemalloc: LDFLAGS += -ljemalloc
jemalloc: $(TARGET)

test-jemalloc: jemalloc
	@$(TARGET) tests/run_tests.sea

# Dead-write elimination checks: with SEA_DWE_CHECK=1 an eliminated write loads
# a poison string instead of becoming a NOP (a write wrongly judged dead then
# shows up in the output); SEA_NO_DWE=1 compiles with no elimination at all.
# SEA_TYPE_CHECK=1 makes IIADD/IISUB (emitted only for operands the compiler
# proved int) abort if that proof is wrong.
test-dwe: $(TARGET)
	SEA_DWE_CHECK=1 SEA_TYPE_CHECK=1 bash tests/run_tests.sh
	SEA_NO_DWE=1 bash tests/run_tests.sh

# Unit tests run as compiled bytecode: each test is `sea compile`d to .seac and
# that file is executed (catches serialization bugs in sea_chunk_write/read).
test-seac: $(TARGET)
	SEAC=1 bash tests/run_tests.sh

# Unit tests run as standalone executables made by `sea build`
test-build: $(TARGET)
	BUNDLE=1 bash tests/run_tests.sh

# Unit tests on bin/sea-jit with every chunk JIT-compiled at its first call /
# loop back-edge, synchronously (deterministic), then asynchronously, then with
# the default thresholds (code compiled after the interpreter has recorded type
# feedback, which selects the typed-array/table fallbacks). STRICT
# makes any JIT codegen/compile failure abort instead of silently interpreting.
test-jit: $(JIT_TARGET)
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_SYNC=1 SEA_JIT_HOT_CALLS=1 SEA_JIT_HOT_LOOPS=1 bash tests/run_tests.sh
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_HOT_CALLS=1 SEA_JIT_HOT_LOOPS=1 bash tests/run_tests.sh
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_SYNC=1 bash tests/run_tests.sh

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: $(TARGET)

# Build with the optional method-JIT enabled (see docs/feature_jit.md).
# Needs libgccjit: `sudo apt install libgccjit-dev` (headers at build time; the
# library itself is dlopen()ed at run time, only when --jit is used).
# Object files are shared with the default build, so run `make clean` first when
# switching between a plain build and a JIT build.
jit: CFLAGS  += -DSEA_JIT
jit: $(TARGET)

debug: CFLAGS += -g -O0 -DSEA_DEBUG
debug: $(TARGET)

# Build without readline (for environments without it)
noreadline: CFLAGS += -DSEA_NO_READLINE
noreadline: LDFLAGS = $(LDBASE)
noreadline: $(TARGET)

# Build an external Sea library: make lib SRC=mylib.c OUT=mylib.so
# Example: make lib SRC=examples/hello.c OUT=examples/hello.so
lib:
ifndef SRC
	$(error Usage: make lib SRC=mylib.c OUT=mylib.so)
endif
	$(CC) $(SHARED) -fPIC $(CFLAGS) \
	    -I$(SRC_DIR) \
	    -o $(if $(OUT),$(OUT),$(basename $(SRC)).so) \
	    $(SRC)

test: all
	@$(TARGET) tests/run_tests.sea
	@$(TARGET) utils/license/license.sea -l BSD-3-Clause -c "The Sea Authors" -check

# every source file has the BSD-3-Clause header (fix with: bin/sea utils/license/license.sea -l BSD-3-Clause -c "The Sea Authors")
test-license: $(TARGET)
	@$(TARGET) utils/license/license.sea -l BSD-3-Clause -c "The Sea Authors" -check

# sea/parser's format round trip over every .sea file (see the script)
test-format: $(TARGET)
	@$(TARGET) tests/format_roundtrip.sea $$(find . -name '*.sea' -not -path './.git/*' | sort)

# Run tests under AddressSanitizer (leak detection disabled; register-borrowing
# semantics mean local variables that are never stored to a container will appear
# as leaks at exit. Use-after-free and other errors are still detected.)
test-asan: clean
	$(MAKE) asan
	@ASAN_OPTIONS=detect_leaks=0 $(TARGET) tests/run_tests.sea

clean:
	rm -rf $(OBJ_DIR) $(JIT_OBJ_DIR) $(BIN_DIR) $(GEN_DIR)

# make install [PREFIX=/usr/local] [DESTDIR=...]: bin/sea (and bin/sea-jit
# when built) into $(PREFIX)/bin, and the libraries into $(PREFIX)/lib/sea/<lib>/
# keeping their layout (their requires are relative). require() looks in
# <dir of the executable>/../lib/sea, so require("json") then works anywhere.
# Native libraries are installed when built (make -C libs/<lib>), with the
# shared libraries they load from their vendor/ (libduckdb); libs/test is
# skipped (built in as sea/test).
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin
SEALIB   = $(DESTDIR)$(PREFIX)/lib/sea

install: $(TARGET)
	install -d $(BINDIR) $(SEALIB)
	install -m 755 $(TARGET) $(BINDIR)/sea
	@if [ -x $(JIT_TARGET) ]; then install -m 755 $(JIT_TARGET) $(BINDIR)/sea-jit && echo "installed sea-jit"; fi
	@for d in libs/*/; do \
	    lib=$$(basename $$d); \
	    case $$lib in test|stream|flags|log) continue;; esac; \
	    files=$$(cd $$d && ls *.sea *.so 2>/dev/null | grep -v -e '^test\.sea$$' -e '_test\.sea$$'); \
	    [ -z "$$files" ] && continue; \
	    if ls $$d/*.c >/dev/null 2>&1 && ! ls $$d/*.so >/dev/null 2>&1; then \
	        echo "skipped $$lib (not built: make -C libs/$$lib)"; continue; \
	    fi; \
	    install -d $(SEALIB)/$$lib; \
	    for f in $$files; do \
	        case $$f in *.so) m=755;; *) m=644;; esac; \
	        install -m $$m $$d$$f $(SEALIB)/$$lib/$$f; \
	    done; \
	    for f in $$d/vendor/*.so*; do \
	        [ -f "$$f" ] || continue; \
	        install -d $(SEALIB)/$$lib/vendor; \
	        install -m 755 $$f $(SEALIB)/$$lib/vendor/; \
	    done; \
	    for f in $$d/vendor/lib/*.so*; do \
	        [ -e "$$f" ] || continue; \
	        install -d $(SEALIB)/$$lib/vendor/lib; \
	        cp -P $$f $(SEALIB)/$$lib/vendor/lib/; \
	    done; \
	    echo "installed $$lib"; \
	done
	@echo "sea installed in $(BINDIR); libraries in $(SEALIB)"

uninstall:
	rm -f $(BINDIR)/sea $(BINDIR)/sea-jit
	rm -rf $(SEALIB)
