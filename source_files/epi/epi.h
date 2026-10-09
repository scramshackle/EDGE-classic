//----------------------------------------------------------------------------
//  EDGE Platform Interface Main Header
//----------------------------------------------------------------------------
//
//  Copyright (c) 2002-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------

#pragma once

#include <math.h>
#include <string.h>

#if defined(__MINGW32__)
#define EPI_PRINTF_FORMAT(format_index, first_argument)                                                                \
    __attribute__((format(gnu_printf, format_index, first_argument)))
#elif defined(__GNUC__)
#define EPI_PRINTF_FORMAT(format_index, first_argument) __attribute__((format(printf, format_index, first_argument)))
#else
#define EPI_PRINTF_FORMAT(format_index, first_argument)
#endif

/* Important functions provided by Engine code */
[[noreturn]] void FatalError(const char *error, ...) EPI_PRINTF_FORMAT(1, 2);
void              LogWarning(const char *warning, ...) EPI_PRINTF_FORMAT(1, 2);
void              LogPrint(const char *message, ...) EPI_PRINTF_FORMAT(1, 2);
void              LogDebug(const char *message, ...) EPI_PRINTF_FORMAT(1, 2);

// Move these to dedicated EPI math file - Dasho
inline int RoundToInteger(float x)
{
    return (int)roundf(x);
}
inline int RoundToInteger(double x)
{
    return (int)round(x);
}

// Assertion macro
#define EPI_ASSERT(cond) ((cond) ? (void)0 : FatalError("Assertion '%s' failed (%s:%d).\n", #cond, __FILE__, __LINE__))

// Clears memory to zero.
#define EPI_CLEAR_MEMORY(ptr, type, num) memset((void *)(ptr), ((ptr) - ((type *)(ptr))), (num) * sizeof(type))

// Used for params/variables that we know are unused but still need to exist
#define EPI_UNUSED(x) (void)x

namespace epi
{
void Initialize(void);
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
