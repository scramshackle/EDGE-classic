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

#include "s_blit.h"

#include <string.h>

#include <vector>

#include "con_var.h"
#include "dm_state.h"
#include "epi.h"
#include "epi_math.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "p_blockmap.h"
#include "p_local.h" // ApproximateDistance
#include "r_misc.h"  // PointToAngle
#include "s_cache.h"
#include "s_effect.h"
#include "s_music.h"
#include "s_sound.h"

extern ConsoleVariable fliplevels;

SoundChannel *mix_channels[kMaximumSoundChannels];
int           total_channels;

bool vacuum_sound_effects    = false;
bool submerged_sound_effects = false;

EDGE_DEFINE_CONSOLE_VARIABLE(sound_effect_volume, "0.15", kConsoleVariableFlagArchive)

static bool sound_effects_paused = false;

// these are analogous to view_x/y/z/angle
float    listen_x;
float    listen_y;
float    listen_z;
BAMAngle listen_angle;

SpatialListener spatial_listener;

static std::vector<float> mix_bus[kTotalSoundBuses];
static std::vector<float> mix_scratch;

SoundChannel::SoundChannel() : state_(kChannelEmpty), data_(nullptr), definition_(nullptr), position_(nullptr)
{
    Reset();
}

SoundChannel::~SoundChannel()
{
}

void SoundChannel::Reset(void)
{
    state_            = kChannelEmpty;
    data_             = nullptr;
    definition_       = nullptr;
    position_         = nullptr;
    category_         = 0;
    boss_             = false;
    cursor_           = 0;
    looping_          = false;
    attenuate_        = false;
    volume_           = 1.0f;
    minimum_distance_ = kMinimumSoundClipDistance;
    bus_              = kSoundBusDry;

    spatializer_.Reset();
}

//----------------------------------------------------------------------------

void InitializeSoundChannels(int total)
{
    total_channels = total;

    for (int i = 0; i < total_channels; i++)
        mix_channels[i] = new SoundChannel();
}

void FreeSoundChannels(void)
{
    LockSoundMixer();

    for (int i = 0; i < total_channels; i++)
    {
        SoundChannel *chan = mix_channels[i];

        if (chan)
            chan->Reset();

        delete chan;
    }

    EPI_CLEAR_MEMORY(mix_channels, SoundChannel *, total_channels);

    total_channels = 0;

    UnlockSoundMixer();
}

void KillSoundChannel(int k)
{
    SoundChannel *chan = mix_channels[k];

    if (chan->state_ != kChannelEmpty)
        chan->Reset();
}

void UpdateSounds(MapObject *listener, BAMAngle angle)
{
    LockSoundMixer();

    listen_x = listener ? listener->x : 0;
    listen_y = listener ? listener->y : 0;
    listen_z = listener ? listener->z : 0;

    spatial_listener.position = {{listen_x, listen_z, -listen_y}};

    if (listener)
    {
        if (fliplevels.d_)
        {
            spatial_listener.direction.X = epi::BAMCos(angle - kBAMAngle180);
            spatial_listener.direction.Y = epi::BAMTan(listener->vertical_angle_);
            spatial_listener.direction.Z = -epi::BAMSin(angle);
        }
        else
        {
            spatial_listener.direction.X = epi::BAMCos(angle);
            spatial_listener.direction.Y = epi::BAMTan(listener->vertical_angle_);
            spatial_listener.direction.Z = -epi::BAMSin(angle);
        }
    }
    else
    {
        spatial_listener.direction.X = 0;
        spatial_listener.direction.Y = 0;
        spatial_listener.direction.Z = 0;
    }

    for (int i = 0; i < total_channels; i++)
    {
        SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelPlaying && chan->attenuate_ && chan->position_)
        {
            if (listener)
            {
                if (CheckSightToPoint(listener, chan->position_->x, chan->position_->y, chan->position_->z))
                    chan->minimum_distance_ = kMinimumSoundClipDistance;
                else
                    chan->minimum_distance_ = kMinimumOccludedSoundClipDistance;
            }

            HMM_Vec3 emitter = {{chan->position_->x, chan->position_->z, -chan->position_->y}};

            chan->spatializer_.Update(spatial_listener, emitter, chan->minimum_distance_, kMaximumSoundClipDistance,
                                      chan->volume_);
        }

        if (chan->state_ == kChannelFinished)
            KillSoundChannel(i);
    }

    UnlockSoundMixer();
}

void PauseSound(void)
{
    sound_effects_paused = true;
}

void ResumeSound(void)
{
    sound_effects_paused = false;
}

//----------------------------------------------------------------------------

static int RenderChannel(SoundChannel *chan, float *destination, int frames)
{
    const SoundData *data = chan->data_;

    if (!data || data->length_ <= 0)
        return 0;

    int written = 0;

    while (written < frames)
    {
        if (chan->cursor_ >= data->length_)
        {
            if (!chan->looping_)
                break;

            chan->looping_ = false;
            chan->cursor_  = 0;
        }

        int chunk = data->length_ - chan->cursor_;

        if (chunk > frames - written)
            chunk = frames - written;

        memcpy(destination + written * 2, data->data_ + chan->cursor_ * 2, (size_t)chunk * 2 * sizeof(float));

        chan->cursor_ += chunk;
        written += chunk;
    }

    if (written < frames)
        memset(destination + written * 2, 0, (size_t)(frames - written) * 2 * sizeof(float));

    return written;
}

void MixSoundEffects(float *output, int frames)
{
    memset(output, 0, (size_t)frames * 2 * sizeof(float));

    if (frames <= 0)
        return;

    size_t needed = (size_t)frames * 2;

    if (mix_scratch.size() < needed)
        mix_scratch.resize(needed);

    bool bus_used[kTotalSoundBuses] = {false, false, false, false};

    for (int b = 0; b < kTotalSoundBuses; b++)
    {
        if (mix_bus[b].size() < needed)
            mix_bus[b].resize(needed);
    }

    for (int i = 0; i < total_channels; i++)
    {
        SoundChannel *chan = mix_channels[i];

        if (!chan || chan->state_ != kChannelPlaying)
            continue;

        int written = RenderChannel(chan, mix_scratch.data(), frames);

        if (written == 0)
        {
            chan->state_ = kChannelFinished;
            continue;
        }

        chan->spatializer_.Process(mix_scratch.data(), frames);

        float *bus = mix_bus[chan->bus_].data();

        if (!bus_used[chan->bus_])
        {
            memcpy(bus, mix_scratch.data(), needed * sizeof(float));
            bus_used[chan->bus_] = true;
        }
        else
        {
            for (size_t s = 0; s < needed; s++)
                bus[s] += mix_scratch[s];
        }

        if (written < frames)
            chan->state_ = kChannelFinished;
    }

    for (int b = kSoundBusVacuum; b < kTotalSoundBuses; b++)
    {
        if (!bus_used[b])
            memset(mix_bus[b].data(), 0, needed * sizeof(float));
    }

    ProcessVacuumEffect(mix_bus[kSoundBusVacuum].data(), frames);
    ProcessUnderwaterEffect(mix_bus[kSoundBusUnderwater].data(), frames);
    ProcessReverbEffect(mix_bus[kSoundBusReverb].data(), frames);

    float master = sound_effect_volume.f_ * 0.5f;

    if (bus_used[kSoundBusDry])
    {
        const float *bus = mix_bus[kSoundBusDry].data();

        for (size_t s = 0; s < needed; s++)
            output[s] += bus[s];
    }

    for (int b = kSoundBusVacuum; b < kTotalSoundBuses; b++)
    {
        const float *bus = mix_bus[b].data();

        for (size_t s = 0; s < needed; s++)
            output[s] += bus[s];
    }

    for (size_t s = 0; s < needed; s++)
        output[s] *= master;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
