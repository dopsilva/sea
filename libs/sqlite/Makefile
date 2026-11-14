# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
include ../platform.mk
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC
SEA_SRC = ../../src

# SQLite is compiled into the module from the amalgamation in vendor/
# (downloaded by fetch.sh on the first build): nothing to install.
SQLITE_FLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_MATH_FUNCTIONS \
               -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS

.PHONY: all test clean distclean

all: sqlite_native.so

vendor/sqlite3.c vendor/sqlite3.h:
	./fetch.sh

vendor/sqlite3.o: vendor/sqlite3.c
	$(CC) -O2 -fPIC -w $(SQLITE_FLAGS) -c -o $@ $<

sqlite_native.so: sqlite_native.c vendor/sqlite3.h vendor/sqlite3.o $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -shared -I$(SEA_SRC) -Ivendor -o $@ sqlite_native.c vendor/sqlite3.o -lpthread -lm

test: all
	@../../bin/sea test.sea > test.out 2>&1; \
	if diff -u test.expected test.out; then echo "sqlite: all tests passed"; rm -f test.out; else exit 1; fi

clean:
	rm -f sqlite_native.so test.out

distclean: clean
	rm -rf vendor
