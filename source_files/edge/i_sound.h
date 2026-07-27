//----------------------------------------------------------------------------
//  EDGE Sound System Header for SDL
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

#include <SDL3/SDL_audio.h>

#include <set>
#include <string>

#include "con_var.h"
#include "s_effect.h"

extern std::set<std::string> available_soundbanks;

extern SDL_AudioDeviceID sound_device;
extern int               sound_device_frequency;
extern int               sound_device_channels;

extern ReverbEffect reverb_effect;

extern bool            sector_reverb; // true if we are in a sector with DDF reverb
extern ConsoleVariable dynamic_reverb;

void SoundDeviceFormatChanged(void);
void ApplyPendingSoundDeviceFormatChange(void);

void LockSoundMixer(void);
void UnlockSoundMixer(void);

void ProcessVacuumEffect(float *frames, int frame_count);
void ProcessUnderwaterEffect(float *frames, int frame_count);
void ProcessReverbEffect(float *frames, int frame_count);
