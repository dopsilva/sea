# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
CFLAGS = -std=c11 -O3 -march=native -ffast-math -fno-finite-math-only -Wall -Wextra \
         -Wno-unused-parameter -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: tensor.so

tensor.so: tensor.c tensor_kernels.h $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $< -lm

clean:
	rm -f tensor.so
