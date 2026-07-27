//----------------------------------------------------------------------------
//  Sound Blitter
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
//
//  Based on the DOOM source code, released by Id Software under the
//  following copyright:
//
//    Copyright (C) 1993-1996 by id Software, Inc.
//
//----------------------------------------------------------------------------

#pragma once

#include "con_var.h"
#include "ddf_types.h"
#include "p_mobj.h"
#include "s_spatial.h"
#include "snd_data.h"

// Forward declarations
class SoundEffectDefinition;
struct Position;

enum ChannelState
{
    kChannelEmpty    = 0,
    kChannelPlaying  = 1,
    kChannelFinished = 2
};

enum SoundEffectBus
{
    kSoundBusDry        = 0,
    kSoundBusVacuum     = 1,
    kSoundBusUnderwater = 2,
    kSoundBusReverb     = 3,
    kTotalSoundBuses    = 4
};

// channel info
class SoundChannel
{
  public:
    int state_; // CHAN_xxx

    const SoundData *data_;

    int                          category_;
    const SoundEffectDefinition *definition_;
    const Position              *position_;

    bool boss_;

    int   cursor_;
    bool  looping_;
    bool  attenuate_;
    float volume_;
    float minimum_distance_;
    int   bus_;

    SoundSpatializer spatializer_;

  public:
    SoundChannel();
    ~SoundChannel();

    void Reset(void);
};

constexpr uint16_t kMaximumSoundChannels = 128;

extern ConsoleVariable sound_effect_volume;

extern SpatialListener spatial_listener;

extern SoundChannel *mix_channels[];
extern int           total_channels;

extern bool            vacuum_sound_effects;
extern bool            submerged_sound_effects;
extern ConsoleVariable dynamic_reverb;

void InitializeSoundChannels(int total);
void FreeSoundChannels(void);

void KillSoundChannel(int k);

void UpdateSounds(MapObject *listener, BAMAngle angle);

void MixSoundEffects(float *output, int frames);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
