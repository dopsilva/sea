# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
include ../platform.mk
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC $(SHARED)
SEA_SRC = ../../src

.PHONY: all clean

all: xml.so

xml.so: xml.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $<

clean:
	rm -f xml.so
