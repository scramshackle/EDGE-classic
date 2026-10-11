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

#include "s_spatial.h"

#include <math.h>

#include "epi.h"

static constexpr uint32_t kSmoothTimeUnset = (uint32_t)-1;

static uint32_t spatial_gain_smooth_frames = 352;

void SetSpatialGainSmoothTime(int frequency)
{
    spatial_gain_smooth_frames = (uint32_t)((kSpatialGainSmoothMilliseconds * frequency) / 1000);

    if (spatial_gain_smooth_frames < 1)
        spatial_gain_smooth_frames = 1;
}

static const epi::Vec3 kChannelDirections[kSpatialChannels] = {{-1.0f, 0.0f, 0.0f}, {+1.0f, 0.0f, 0.0f}};

static constexpr float kChannelConversionGain = 0.5f;

static const epi::Vec3 kWorldUp = {0.0f, 1.0f, 0.0f};

static inline epi::Vec3 SafeNormalize(const epi::Vec3 &v)
{
    if (epi::VectorLengthSquared(v) == 0.0f)
        return v;

    return epi::NormalizeVector(v);
}

static float AttenuationExponential(float distance, float minimum_distance, float maximum_distance, float rolloff)
{
    if (minimum_distance >= maximum_distance)
        return 1.0f;

    float clamped = distance;

    if (clamped < minimum_distance)
        clamped = minimum_distance;
    if (clamped > maximum_distance)
        clamped = maximum_distance;

    return (float)pow((double)(clamped / minimum_distance), (double)-rolloff);
}

static epi::Vec3 ToListenerSpace(const SpatialListener &listener, const epi::Vec3 &emitter)
{
    epi::Vec3 axis_z = SafeNormalize(listener.direction);
    epi::Vec3 axis_x = SafeNormalize(epi::CrossProduct(axis_z, kWorldUp));

    if (epi::VectorLengthSquared(axis_x) == 0.0f)
        axis_x = {1.0f, 0.0f, 0.0f};

    epi::Vec3 axis_y     = epi::CrossProduct(axis_x, axis_z);
    epi::Vec3 negative_z = {-axis_z.x, -axis_z.y, -axis_z.z};

    epi::Vec3 result;
    result.x = epi::DotProduct(axis_x, emitter) - epi::DotProduct(axis_x, listener.position);
    result.y = epi::DotProduct(axis_y, emitter) - epi::DotProduct(axis_y, listener.position);
    result.z = epi::DotProduct(negative_z, emitter) - epi::DotProduct(negative_z, listener.position);

    return result;
}

SoundSpatializer::SoundSpatializer()
{
    Reset();
}

void SoundSpatializer::Reset(void)
{
    for (int i = 0; i < kSpatialChannels; i++)
    {
        old_gains_[i] = 1.0f;
        new_gains_[i] = 1.0f;
    }

    smooth_time_ = kSmoothTimeUnset;
}

float SoundSpatializer::CurrentGain(int channel) const
{
    if (smooth_time_ == kSmoothTimeUnset || smooth_time_ >= spatial_gain_smooth_frames)
        return new_gains_[channel];

    float a = (float)smooth_time_ / (float)spatial_gain_smooth_frames;

    return old_gains_[channel] + (new_gains_[channel] - old_gains_[channel]) * a;
}

void SoundSpatializer::SetGains(const float *gains)
{
    for (int i = 0; i < kSpatialChannels; i++)
    {
        old_gains_[i] = CurrentGain(i);
        new_gains_[i] = gains[i];
    }

    if (smooth_time_ == kSmoothTimeUnset)
        smooth_time_ = spatial_gain_smooth_frames;
    else
        smooth_time_ = 0;
}

void SoundSpatializer::SetUniformGain(float gain)
{
    float gains[kSpatialChannels];

    for (int i = 0; i < kSpatialChannels; i++)
        gains[i] = gain;

    SetGains(gains);
}

void SoundSpatializer::Update(const SpatialListener &listener, const epi::Vec3 &emitter, float minimum_distance,
                              float maximum_distance, float volume)
{
    epi::Vec3 relative = ToListenerSpace(listener, emitter);

    float distance = epi::VectorLength(relative);

    float gain = AttenuationExponential(distance, minimum_distance, maximum_distance, 1.0f);

    if (gain < 0.0f)
        gain = 0.0f;
    if (gain > 1.0f)
        gain = 1.0f;

    gain *= volume * kChannelConversionGain;

    float gains[kSpatialChannels];

    for (int i = 0; i < kSpatialChannels; i++)
        gains[i] = gain;

    if (distance > 0.001f)
    {
        epi::Vec3 unit = epi::ScaleVector(relative, 1.0f / distance);

        for (int i = 0; i < kSpatialChannels; i++)
        {
            float d = epi::DotProduct(unit, kChannelDirections[i]);

            d = (d + 1.0f) * 0.5f;

            if (d < kMinimumSpatialChannelGain)
                d = kMinimumSpatialChannelGain;

            gains[i] *= d;
        }
    }

    SetGains(gains);
}

void SoundSpatializer::Process(float *frames, int frame_count)
{
    if (frame_count <= 0)
        return;

    int interpolated = 0;

    if (smooth_time_ != kSmoothTimeUnset && smooth_time_ < spatial_gain_smooth_frames)
    {
        interpolated = (int)(spatial_gain_smooth_frames - smooth_time_);

        if (interpolated > frame_count)
            interpolated = frame_count;

        float a = (float)smooth_time_ / (float)spatial_gain_smooth_frames;
        float d = 1.0f / (float)spatial_gain_smooth_frames;

        float *output = frames;

        for (int i = 0; i < interpolated; i++)
        {
            for (int c = 0; c < kSpatialChannels; c++)
                output[c] *= old_gains_[c] + (new_gains_[c] - old_gains_[c]) * a;

            output += kSpatialChannels;

            a += d;

            if (a > 1.0f)
                a = 1.0f;
        }

        smooth_time_ += (uint32_t)interpolated;

        if (smooth_time_ > spatial_gain_smooth_frames)
            smooth_time_ = spatial_gain_smooth_frames;
    }

    float *output = frames + interpolated * kSpatialChannels;

    for (int i = interpolated; i < frame_count; i++)
    {
        for (int c = 0; c < kSpatialChannels; c++)
            output[c] *= new_gains_[c];

        output += kSpatialChannels;
    }

    if (smooth_time_ == kSmoothTimeUnset)
    {
        smooth_time_ = (uint32_t)frame_count;

        if (smooth_time_ > spatial_gain_smooth_frames)
            smooth_time_ = spatial_gain_smooth_frames;
    }
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
