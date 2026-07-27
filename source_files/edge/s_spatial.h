//----------------------------------------------------------------------------
//  EDGE Sound Spatialization
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

#include "HandmadeMath.h"

constexpr int   kSpatialChannels             = 2;
constexpr int   kSpatialGainSmoothMilliseconds = 8;
constexpr float kMinimumSpatialChannelGain   = 0.2f;

void SetSpatialGainSmoothTime(int frequency);

struct SpatialListener
{
    HMM_Vec3 position;
    HMM_Vec3 direction;
};

class SoundSpatializer
{
  public:
    SoundSpatializer();

    void Reset(void);

    void SetUniformGain(float gain);

    void Update(const SpatialListener &listener, const HMM_Vec3 &emitter, float minimum_distance,
                float maximum_distance, float volume);

    void Process(float *frames, int frame_count);

  private:
    float CurrentGain(int channel) const;
    void  SetGains(const float *gains);

    float    old_gains_[kSpatialChannels];
    float    new_gains_[kSpatialChannels];
    uint32_t smooth_time_;
};

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
