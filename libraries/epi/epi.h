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

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <type_traits>

#if defined(__MINGW32__)
#define EPI_PRINTF_FORMAT(format_index, first_argument)                                                                \
    __attribute__((format(gnu_printf, format_index, first_argument)))
#elif defined(__GNUC__)
#define EPI_PRINTF_FORMAT(format_index, first_argument) __attribute__((format(printf, format_index, first_argument)))
#else
#define EPI_PRINTF_FORMAT(format_index, first_argument)
#endif

namespace epi
{

enum LogLevel
{
    kLogLevelPrint,
    kLogLevelWarning,
    kLogLevelDebug
};

void SetFatalErrorHandler(void (*handler)(const char *message));
void SetLogHandler(void (*handler)(LogLevel level, const char *format, va_list arguments));

[[noreturn]] void FatalError(const char *error, ...) EPI_PRINTF_FORMAT(1, 2);
void              LogMessage(LogLevel level, const char *format, ...) EPI_PRINTF_FORMAT(2, 3);

template <typename T> inline void ClearMemory(T *pointer, size_t count)
{
    static_assert(std::is_trivially_copyable_v<T>);
    memset((void *)pointer, 0, count * sizeof(T));
}

} // namespace epi

// Assertion macro
#define EPI_ASSERT(cond)                                                                                               \
    ((cond) ? (void)0 : epi::FatalError("Assertion '%s' failed (%s:%d).\n", #cond, __FILE__, __LINE__))

// Used for params/variables that we know are unused but still need to exist
#define EPI_UNUSED(x) (void)x

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
