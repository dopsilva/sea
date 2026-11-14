# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
include ../platform.mk
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC $(SHARED)
SEA_SRC = ../../src

# llama.cpp comes from vendor/ (downloaded and built by fetch.sh on the first
# build) unless LLAMA_INC / LLAMA_LIB point at an installed copy:
#   make LLAMA_INC=/usr/local/include LLAMA_LIB=/usr/local/lib
LLAMA_INC ?= vendor/include
LLAMA_LIB ?= vendor/lib

ifeq ($(LLAMA_LIB),vendor/lib)
RPATH = -Wl,-rpath,'$$ORIGIN/vendor/lib'
LLAMA_VERSION = $(shell cat vendor/VERSION 2>/dev/null)
else
RPATH = -Wl,-rpath,$(LLAMA_LIB)
LLAMA_VERSION = installed
endif

.PHONY: all test clean distclean

all: embed_native.so

vendor/include/llama.h vendor/lib/libllama.so:
	./fetch.sh

embed_native.so: embed_native.c $(LLAMA_INC)/llama.h $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -I$(LLAMA_INC) -DEMBED_LLAMA_VERSION='"$(LLAMA_VERSION)"' \
		-o $@ $< -L$(LLAMA_LIB) -lllama $(RPATH) -lpthread -lm

# the tests need a model: EMBED_TEST_MODEL (a GGUF path or a name embed.load
# takes), else one ollama already downloaded; without any they are skipped
test: all
	@../../bin/sea test embed_test.sea

clean:
	rm -f embed_native.so

distclean: clean
	rm -rf vendor
