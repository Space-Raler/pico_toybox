/* Tiny stand-in for the Flipper's <furi.h>, only what the reused LEGO files touch. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif
#define furi_assert(x) assert(x)
#define furi_check(x) assert(x)
#ifndef COUNT_OF
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))
#endif
