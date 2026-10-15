//----------------------------------------------------------------------------
//  EDGE Threading
//----------------------------------------------------------------------------
//
//  Copyright (c) 1999-2024 The EDGE Team.
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

#include "i_thread.h"

#include <SDL3/SDL.h>

#include "i_system.h"

SystemThread *StartThread(SystemThreadFunction function, void *data, const char *name)
{
    if (!function)
        return nullptr;

    SDL_Thread *thread = SDL_CreateThread((SDL_ThreadFunction)function, name, data);

    if (!thread)
    {
        epi::LogMessage(epi::kLogLevelDebug, "StartThread: could not start '%s': %s\n", name ? name : "(unnamed)",
                        SDL_GetError());
        return nullptr;
    }

    return (SystemThread *)thread;
}

void JoinThread(SystemThread *thread)
{
    if (!thread)
        return;

    SDL_WaitThread((SDL_Thread *)thread, nullptr);
}

SystemMutex *CreateSystemMutex(void)
{
    return (SystemMutex *)SDL_CreateMutex();
}

void DestroySystemMutex(SystemMutex *mutex)
{
    if (!mutex)
        return;

    SDL_DestroyMutex((SDL_Mutex *)mutex);
}

void LockSystemMutex(SystemMutex *mutex)
{
    if (!mutex)
        return;

    SDL_LockMutex((SDL_Mutex *)mutex);
}

void UnlockSystemMutex(SystemMutex *mutex)
{
    if (!mutex)
        return;

    SDL_UnlockMutex((SDL_Mutex *)mutex);
}

SystemCondition *CreateSystemCondition(void)
{
    return (SystemCondition *)SDL_CreateCondition();
}

void DestroySystemCondition(SystemCondition *condition)
{
    if (!condition)
        return;

    SDL_DestroyCondition((SDL_Condition *)condition);
}

void SignalSystemCondition(SystemCondition *condition)
{
    if (!condition)
        return;

    SDL_SignalCondition((SDL_Condition *)condition);
}

void WaitSystemCondition(SystemCondition *condition, SystemMutex *mutex)
{
    if (!condition || !mutex)
        return;

    SDL_WaitCondition((SDL_Condition *)condition, (SDL_Mutex *)mutex);
}

bool WaitSystemConditionTimeout(SystemCondition *condition, SystemMutex *mutex, int timeout_ms)
{
    if (!condition || !mutex)
        return false;

    return SDL_WaitConditionTimeout((SDL_Condition *)condition, (SDL_Mutex *)mutex, timeout_ms);
}

int TotalSystemCPUs(void)
{
    int total = SDL_GetNumLogicalCPUCores();

    return total > 0 ? total : 1;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
