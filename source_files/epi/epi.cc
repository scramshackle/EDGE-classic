//----------------------------------------------------------------------------
//  EDGE Platform Interface Main Functions
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

#include "epi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "epi_str_util.h"

namespace epi
{

static constexpr int kMessageBufferSize = 4096;

static void DefaultFatalErrorHandler(const char *message)
{
    fflush(stdout);
    fprintf(stderr, "ERROR: %s\n", message);
}

static void DefaultLogHandler(LogLevel level, const char *message)
{
    if (level == kLogLevelPrint)
        fputs(message, stdout);
    else if (level == kLogLevelWarning)
        fprintf(stderr, "WARNING: %s", message);
}

static void (*fatal_error_handler)(const char *message)         = DefaultFatalErrorHandler;
static void (*log_handler)(LogLevel level, const char *message) = DefaultLogHandler;

void SetFatalErrorHandler(void (*handler)(const char *message))
{
    fatal_error_handler = handler ? handler : DefaultFatalErrorHandler;
}

void SetLogHandler(void (*handler)(LogLevel level, const char *message))
{
    log_handler = handler ? handler : DefaultLogHandler;
}

[[noreturn]] void FatalError(const char *error, ...)
{
    char    message[kMessageBufferSize];
    va_list arguments;

    va_start(arguments, error);
    FormatToBufferSizedArgs(message, sizeof(message), error, arguments);
    va_end(arguments);

    fatal_error_handler(message);
    abort();
}

static void LogMessage(LogLevel level, const char *format, va_list arguments)
{
    char message[kMessageBufferSize];
    FormatToBufferSizedArgs(message, sizeof(message), format, arguments);
    log_handler(level, message);
}

void LogWarning(const char *warning, ...)
{
    va_list arguments;
    va_start(arguments, warning);
    LogMessage(kLogLevelWarning, warning, arguments);
    va_end(arguments);
}

void LogPrint(const char *message, ...)
{
    va_list arguments;
    va_start(arguments, message);
    LogMessage(kLogLevelPrint, message, arguments);
    va_end(arguments);
}

void LogDebug(const char *message, ...)
{
    va_list arguments;
    va_start(arguments, message);
    LogMessage(kLogLevelDebug, message, arguments);
    va_end(arguments);
}

} // namespace epi

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
