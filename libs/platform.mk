# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

# Compiler flags that differ between Linux and macOS, shared by the libraries'
# Makefiles (include ../platform.mk):
#   SHARED  link a loadable module (its sea_* symbols come from the sea binary)
#   NATIVE  tune for the machine the library is built on
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
SHARED = -dynamiclib -undefined dynamic_lookup
ifeq ($(UNAME_M),arm64)
NATIVE = -mcpu=native
else
NATIVE = -march=native
endif
else
SHARED = -shared
NATIVE = -march=native
endif
