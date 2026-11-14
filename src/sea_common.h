/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Version
#define SEA_VERSION_MAJOR 0
#define SEA_VERSION_MINOR 3
#define SEA_VERSION_PATCH 0
#define SEA_VERSION "0.3.0"

// Compiler hints
#define SEA_LIKELY(x)    __builtin_expect(!!(x), 1)
#define SEA_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define SEA_NORETURN     __attribute__((noreturn))
#define SEA_INLINE       static inline __attribute__((always_inline))
#define SEA_UNREACHABLE() __builtin_unreachable()

// Error codes
#define SEA_OK         0
#define SEA_ERR       -1
#define SEA_SUSPENDED  2   /* goroutine parked on a channel; vm_run exited early */

// Max limits
#define SEA_MAX_REGISTERS   256
#define SEA_MAX_CALL_DEPTH  512
#define SEA_MAX_UPVALUES    256
#define SEA_MAX_CONSTANTS   65536

// Forward declarations
typedef struct SeaVM       SeaVM;
typedef struct SeaObject   SeaObject;
typedef struct SeaString   SeaString;
typedef struct SeaArray    SeaArray;
typedef struct SeaTable    SeaTable;
typedef struct SeaMap      SeaMap;
typedef struct SeaFunction SeaFunction;
typedef struct SeaClosure  SeaClosure;
typedef struct SeaUpvalue  SeaUpvalue;
typedef struct SeaError    SeaError;
typedef struct SeaChannel  SeaChannel;
typedef struct SeaGoroutine SeaGoroutine;
typedef struct SeaChunk    SeaChunk;
typedef uint64_t SeaValue;
