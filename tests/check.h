// SPDX-License-Identifier: GPL-3.0-only
// Minimal test helper: CHECK() records a failure and keeps going; main returns failures != 0.
#pragma once
#include <stdio.h>

static int failures;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
			failures++;                                                              \
		}                                                                            \
	} while (0)
