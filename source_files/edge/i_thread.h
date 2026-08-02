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

#pragma once

#include <stdint.h>

struct SystemThread;
struct SystemMutex;
struct SystemCondition;

struct SystemAtomicU32
{
    uint32_t value;
};

typedef int (*SystemThreadFunction)(void *data);

SystemThread *StartThread(SystemThreadFunction function, void *data, const char *name);

void JoinThread(SystemThread *thread);

SystemMutex *CreateSystemMutex(void);

void DestroySystemMutex(SystemMutex *mutex);

void LockSystemMutex(SystemMutex *mutex);

void UnlockSystemMutex(SystemMutex *mutex);

SystemCondition *CreateSystemCondition(void);

void DestroySystemCondition(SystemCondition *condition);

void SignalSystemCondition(SystemCondition *condition);

void WaitSystemCondition(SystemCondition *condition, SystemMutex *mutex);

bool WaitSystemConditionTimeout(SystemCondition *condition, SystemMutex *mutex, int timeout_ms);

void SetAtomicU32(SystemAtomicU32 *atomic, uint32_t value);

uint32_t GetAtomicU32(SystemAtomicU32 *atomic);

uint32_t AddAtomicU32(SystemAtomicU32 *atomic, int32_t delta);

int TotalSystemCPUs(void);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
