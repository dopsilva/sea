# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
include ../platform.mk
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC $(SHARED)
SEA_SRC = ../../src

OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   = $(shell pkg-config --libs   openssl 2>/dev/null || echo "-lssl -lcrypto")

.PHONY: all clean

all: jwt_native.so

jwt_native.so: jwt_native.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) $(OPENSSL_CFLAGS) -I$(SEA_SRC) -o $@ $< $(OPENSSL_LIBS)

clean:
	rm -f jwt_native.so
