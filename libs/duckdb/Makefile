# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
include ../platform.mk
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC $(SHARED)
SEA_SRC = ../../src

# DuckDB comes from vendor/ (downloaded by fetch.sh on the first build) unless
# DUCKDB_INC / DUCKDB_LIB point at an installed copy:
#   make DUCKDB_INC=/usr/local/include DUCKDB_LIB=/usr/local/lib
DUCKDB_INC ?= vendor
DUCKDB_LIB ?= vendor

ifeq ($(DUCKDB_LIB),vendor)
RPATH = -Wl,-rpath,'$$ORIGIN/vendor'
else
RPATH = -Wl,-rpath,$(DUCKDB_LIB)
endif

.PHONY: all test clean distclean

all: duckdb_native.so

vendor/duckdb.h vendor/libduckdb.so:
	./fetch.sh

duckdb_native.so: duckdb_native.c $(DUCKDB_INC)/duckdb.h $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -I$(DUCKDB_INC) -o $@ $< -L$(DUCKDB_LIB) -lduckdb $(RPATH) -lpthread

test: all
	@../../bin/sea test.sea > test.out 2>&1; \
	if diff -u test.expected test.out; then echo "duckdb: all tests passed"; rm -f test.out; else exit 1; fi

clean:
	rm -f duckdb_native.so test.out

distclean: clean
	rm -rf vendor
