//----------------------------------------------------------------------------
//  EDGE Sound Effects (DSP)
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

#include "s_effect.h"

#include <math.h>

#include "HandmadeMath.h"
#include "epi.h"

LowPassEffect::LowPassEffect()
    : b0_(0.0f), b1_(0.0f), b2_(0.0f), a1_(0.0f), a2_(0.0f), channels_(kMaximumEffectChannels), ready_(false)
{
    for (int c = 0; c < kMaximumEffectChannels; c++)
    {
        r1_[c] = 0.0f;
        r2_[c] = 0.0f;
    }
}

LowPassEffect::~LowPassEffect()
{
    Shutdown();
}

bool LowPassEffect::Setup(int frequency, int channels, float cutoff)
{
    Shutdown();

    if (channels < 1 || channels > kMaximumEffectChannels || frequency <= 0)
        return false;

    channels_ = channels;

    double q = 1.0 / (2.0 * cos(HMM_PI / 4.0));
    double w = 2.0 * HMM_PI * (double)cutoff / (double)frequency;
    double s = sin(w);
    double c = cos(w);
    double a = s / (2.0 * q);

    double b0 = (1.0 - c) / 2.0;
    double b1 = 1.0 - c;
    double b2 = (1.0 - c) / 2.0;
    double a0 = 1.0 + a;
    double a1 = -2.0 * c;
    double a2 = 1.0 - a;

    if (a0 == 0.0)
        return false;

    b0_ = (float)(b0 / a0);
    b1_ = (float)(b1 / a0);
    b2_ = (float)(b2 / a0);
    a1_ = (float)(a1 / a0);
    a2_ = (float)(a2 / a0);

    for (int i = 0; i < kMaximumEffectChannels; i++)
    {
        r1_[i] = 0.0f;
        r2_[i] = 0.0f;
    }

    ready_ = true;

    return true;
}

void LowPassEffect::Shutdown(void)
{
    ready_ = false;
}

void LowPassEffect::Process(float *frames, int frame_count)
{
    if (!ready_ || frame_count <= 0)
        return;

    for (int i = 0; i < frame_count; i++)
    {
        for (int c = 0; c < channels_; c++)
        {
            float x = frames[i * channels_ + c];
            float y = b0_ * x + r1_[c];

            r1_[c] = b1_ * x - a1_ * y + r2_[c];
            r2_[c] = b2_ * x - a2_ * y;

            frames[i * channels_ + c] = y;
        }
    }
}

DelayEffect::DelayEffect()
    : buffer_frames_(0), cursor_(0), channels_(kMaximumEffectChannels), decay_(0.0f), wet_(1.0f), dry_(1.0f),
      ready_(false)
{
}

DelayEffect::~DelayEffect()
{
    Shutdown();
}

bool DelayEffect::Setup(int frequency, int channels, float delay_seconds, float decay)
{
    Shutdown();

    if (channels < 1 || channels > kMaximumEffectChannels || frequency <= 0)
        return false;

    if (decay < 0.0f || decay > 1.0f)
        return false;

    channels_      = channels;
    decay_         = decay;
    wet_           = 1.0f;
    dry_           = 1.0f;
    buffer_frames_ = (int)((float)frequency * delay_seconds);

    if (buffer_frames_ < 1)
        return false;

    buffer_.assign((size_t)buffer_frames_ * channels_, 0.0f);

    cursor_ = 0;
    ready_  = true;

    return true;
}

void DelayEffect::Shutdown(void)
{
    ready_ = false;
    buffer_.clear();
    buffer_frames_ = 0;
    cursor_        = 0;
}

void DelayEffect::Process(float *frames, int frame_count)
{
    if (!ready_ || frame_count <= 0)
        return;

    for (int i = 0; i < frame_count; i++)
    {
        for (int c = 0; c < channels_; c++)
        {
            int index = cursor_ * channels_ + c;

            buffer_[index] = buffer_[index] * decay_ + frames[i * channels_ + c] * dry_;

            frames[i * channels_ + c] = buffer_[index] * wet_;
        }

        cursor_ = (cursor_ + 1) % buffer_frames_;
    }
}

static const int kReverbBaseLineLengths[kReverbDelayLines]      = {1499, 1889, 2381, 2999};
static const int kReverbAllpassLengths[kReverbAllpassStages]    = {379, 277, 213, 149};
static const float kReverbModulationRates[kReverbDelayLines]    = {0.31f, 0.43f, 0.57f, 0.71f};

static constexpr int   kReverbReferenceRate    = 44100;
static constexpr float kReverbMaximumSizeScale = 2.0f;
static constexpr float kReverbMinimumSizeScale = 0.5f;
static constexpr float kReverbMaximumPreDelay  = 0.2f;
static constexpr float kReverbMaximumDiffusion = 0.7f;
static constexpr float kReverbModulationDepth  = 0.0015f;
static constexpr float kReverbLowDampingCrossover = 250.0f;

static bool IsPrime(int value)
{
    if (value < 2)
        return false;

    if (value % 2 == 0)
        return value == 2;

    for (int i = 3; i * i <= value; i += 2)
    {
        if (value % i == 0)
            return false;
    }

    return true;
}

static int NearestPrime(int value)
{
    if (value < 2)
        return 2;

    for (int offset = 0; offset < value; offset++)
    {
        if (IsPrime(value - offset))
            return value - offset;

        if (IsPrime(value + offset))
            return value + offset;
    }

    return value;
}

ReverbEffect::ReverbEffect()
    : ready_(false), frequency_(kReverbReferenceRate), channels_(kMaximumEffectChannels), pending_change_(false),
      pre_delay_capacity_(0), pre_delay_length_(0), pre_delay_cursor_(0), allpass_feedback_(0.0f), line_capacity_(0),
      high_damping_(0.0f), low_damping_(0.0f), low_damping_coefficient_(0.0f), modulation_depth_(0.0f), wet_level_(0.0f), dry_level_(1.0f),
      width_(1.0f)
{
    for (int i = 0; i < kTotalReverbParameters; i++)
    {
        parameters_[i].store(0.0f);
        active_[i] = 0.0f;
    }

    for (int i = 0; i < kReverbAllpassStages; i++)
    {
        allpass_length_[i] = 0;
        allpass_cursor_[i] = 0;
    }

    for (int i = 0; i < kReverbDelayLines; i++)
    {
        line_length_[i]       = 0;
        line_cursor_[i]       = 0;
        line_gain_[i]         = 0.0f;
        line_state_[i]        = 0.0f;
        damping_low_state_[i] = 0.0f;
        modulation_phase_[i]  = 0.0f;
        modulation_step_[i]   = 0.0f;
        interpolator_input_[i]  = 0.0f;
        interpolator_output_[i] = 0.0f;
    }
}

ReverbEffect::~ReverbEffect()
{
    Shutdown();
}

bool ReverbEffect::Setup(int frequency, int channels)
{
    Shutdown();

    if (frequency <= 0 || channels < 1 || channels > kMaximumEffectChannels)
        return false;

    frequency_ = frequency;
    channels_  = channels;

    float rate_scale = (float)frequency / (float)kReverbReferenceRate;

    pre_delay_capacity_ = (int)(kReverbMaximumPreDelay * (float)frequency) + 1;
    pre_delay_buffer_.assign((size_t)pre_delay_capacity_, 0.0f);
    pre_delay_length_ = 0;
    pre_delay_cursor_ = 0;

    for (int i = 0; i < kReverbAllpassStages; i++)
    {
        allpass_length_[i] = NearestPrime((int)((float)kReverbAllpassLengths[i] * rate_scale));
        allpass_buffer_[i].assign((size_t)allpass_length_[i], 0.0f);
        allpass_cursor_[i] = 0;
    }

    line_capacity_ =
        (int)((float)kReverbBaseLineLengths[kReverbDelayLines - 1] * kReverbMaximumSizeScale * rate_scale) + 64;

    for (int i = 0; i < kReverbDelayLines; i++)
    {
        line_buffer_[i].assign((size_t)line_capacity_, 0.0f);
        line_cursor_[i]       = 0;
        line_state_[i]        = 0.0f;
        damping_low_state_[i] = 0.0f;
        modulation_phase_[i]     = (float)i * 0.25f;
        modulation_step_[i]      = kReverbModulationRates[i] / (float)frequency;
        interpolator_input_[i]   = 0.0f;
        interpolator_output_[i]  = 0.0f;
    }

    modulation_depth_ = kReverbModulationDepth;

    low_damping_coefficient_ = (float)exp(-2.0 * HMM_PI * (double)kReverbLowDampingCrossover / (double)frequency);

    ready_ = true;

    pending_change_.store(true);
    ApplyParameters();

    return true;
}

void ReverbEffect::Shutdown(void)
{
    ready_ = false;

    pre_delay_buffer_.clear();
    pre_delay_capacity_ = 0;

    for (int i = 0; i < kReverbAllpassStages; i++)
        allpass_buffer_[i].clear();

    for (int i = 0; i < kReverbDelayLines; i++)
        line_buffer_[i].clear();

    line_capacity_ = 0;
}

void ReverbEffect::SetParameter(int index, float value)
{
    if (index < 0 || index >= kTotalReverbParameters)
        return;

    parameters_[index].store(value);
    pending_change_.store(true);
}

void ReverbEffect::SetParameters(const float *values)
{
    for (int i = 0; i < kTotalReverbParameters; i++)
        parameters_[i].store(values[i]);

    pending_change_.store(true);
}

void ReverbEffect::ApplyParameters(void)
{
    for (int i = 0; i < kTotalReverbParameters; i++)
        active_[i] = parameters_[i].load();

    float rate_scale = (float)frequency_ / (float)kReverbReferenceRate;

    float decay_time = active_[kReverbDecayTime];

    if (decay_time < 0.0f)
        decay_time = 0.0f;

    float room_size = active_[kReverbRoomSize];

    if (room_size < 0.0f)
        room_size = 0.0f;
    if (room_size > 1.0f)
        room_size = 1.0f;

    float size_scale = kReverbMinimumSizeScale + room_size * (kReverbMaximumSizeScale - kReverbMinimumSizeScale);

    float pre_delay = active_[kReverbPreDelay];

    if (pre_delay < 0.0f)
        pre_delay = 0.0f;
    if (pre_delay > kReverbMaximumPreDelay)
        pre_delay = kReverbMaximumPreDelay;

    int new_pre_delay_length = (int)(pre_delay * (float)frequency_);

    if (new_pre_delay_length >= pre_delay_capacity_)
        new_pre_delay_length = pre_delay_capacity_ - 1;

    if (new_pre_delay_length != pre_delay_length_)
    {
        pre_delay_length_ = new_pre_delay_length;
        pre_delay_cursor_ = 0;
    }

    float diffusion = active_[kReverbDiffusion];

    if (diffusion < 0.0f)
        diffusion = 0.0f;
    if (diffusion > 1.0f)
        diffusion = 1.0f;

    allpass_feedback_ = diffusion * kReverbMaximumDiffusion;

    high_damping_ = active_[kReverbHighFrequencyDamping];

    if (high_damping_ < 0.0f)
        high_damping_ = 0.0f;
    if (high_damping_ > 1.0f)
        high_damping_ = 1.0f;

    low_damping_ = active_[kReverbLowFrequencyDamping];

    if (low_damping_ < 0.0f)
        low_damping_ = 0.0f;
    if (low_damping_ > 1.0f)
        low_damping_ = 1.0f;

    wet_level_ = active_[kReverbWetLevel];
    dry_level_ = active_[kReverbDryLevel];
    width_     = active_[kReverbWidth];

    for (int i = 0; i < kReverbDelayLines; i++)
    {
        int length = NearestPrime((int)((float)kReverbBaseLineLengths[i] * size_scale * rate_scale));

        if (length < 8)
            length = 8;
        if (length > line_capacity_ - 8)
            length = line_capacity_ - 8;

        if (length != line_length_[i])
        {
            line_length_[i] = length;
            line_cursor_[i] = 0;
        }

        if (decay_time <= 0.0f)
            line_gain_[i] = 0.0f;
        else
            line_gain_[i] =
                (float)pow(10.0, -3.0 * (double)length / ((double)frequency_ * (double)decay_time));
    }
}

void ReverbEffect::Process(float *frames, int frame_count)
{
    if (!ready_ || frame_count <= 0)
        return;

    if (pending_change_.exchange(false))
        ApplyParameters();

    if (active_[kReverbDecayTime] <= 0.0f && wet_level_ <= 0.0f)
    {
        if (dry_level_ != 1.0f)
        {
            for (int i = 0; i < frame_count * channels_; i++)
                frames[i] *= dry_level_;
        }
        return;
    }

    for (int i = 0; i < frame_count; i++)
    {
        float left  = frames[i * channels_];
        float right = (channels_ > 1) ? frames[i * channels_ + 1] : left;

        float input = (left + right) * 0.5f;

        if (pre_delay_length_ > 0)
        {
            float delayed                        = pre_delay_buffer_[pre_delay_cursor_];
            pre_delay_buffer_[pre_delay_cursor_] = input;
            pre_delay_cursor_                    = (pre_delay_cursor_ + 1) % pre_delay_length_;
            input                                = delayed;
        }

        for (int stage = 0; stage < kReverbAllpassStages; stage++)
        {
            float delayed = allpass_buffer_[stage][allpass_cursor_[stage]];
            float output  = -allpass_feedback_ * input + delayed;

            allpass_buffer_[stage][allpass_cursor_[stage]] = input + allpass_feedback_ * output;
            allpass_cursor_[stage] = (allpass_cursor_[stage] + 1) % allpass_length_[stage];

            input = output;
        }

        float taps[kReverbDelayLines];

        for (int line = 0; line < kReverbDelayLines; line++)
        {
            modulation_phase_[line] += modulation_step_[line];

            if (modulation_phase_[line] >= 1.0f)
                modulation_phase_[line] -= 1.0f;

            float offset = modulation_depth_ * (float)line_length_[line] *
                           (0.5f + 0.5f * sinf(modulation_phase_[line] * 2.0f * HMM_PI32));

            float read_position = (float)line_cursor_[line] + offset;

            while (read_position >= (float)line_length_[line])
                read_position -= (float)line_length_[line];

            int   index = (int)read_position;
            float frac  = read_position - (float)index;

            float eta = (1.0f - frac) / (1.0f + frac);
            float x   = line_buffer_[line][index];
            float y   = eta * x + interpolator_input_[line] - eta * interpolator_output_[line];

            interpolator_input_[line]  = x;
            interpolator_output_[line] = y;

            taps[line] = y;
        }

        float a = taps[0] + taps[1];
        float b = taps[2] + taps[3];
        float c = taps[0] - taps[1];
        float d = taps[2] - taps[3];

        float mixed[kReverbDelayLines];
        mixed[0] = a + b;
        mixed[1] = c + d;
        mixed[2] = a - b;
        mixed[3] = c - d;

        for (int line = 0; line < kReverbDelayLines; line++)
        {
            float feedback = mixed[line] * 0.5f * line_gain_[line];

            line_state_[line] = (1.0f - high_damping_) * feedback + high_damping_ * line_state_[line];

            float damped = line_state_[line];

            if (low_damping_ > 0.0f)
            {
                damping_low_state_[line] = (1.0f - low_damping_coefficient_) * damped +
                                           low_damping_coefficient_ * damping_low_state_[line];
                damped -= damping_low_state_[line] * low_damping_;
            }

            line_buffer_[line][line_cursor_[line]] = input + damped;

            line_cursor_[line] = (line_cursor_[line] + 1) % line_length_[line];
        }

        float wet_left  = taps[0] + taps[1] - taps[2] - taps[3];
        float wet_right = taps[0] - taps[1] + taps[2] - taps[3];

        wet_left *= 0.5f;
        wet_right *= 0.5f;

        float mono = (wet_left + wet_right) * 0.5f;

        wet_left  = mono + (wet_left - mono) * width_;
        wet_right = mono + (wet_right - mono) * width_;

        frames[i * channels_] = left * dry_level_ + wet_left * wet_level_;

        if (channels_ > 1)
            frames[i * channels_ + 1] = right * dry_level_ + wet_right * wet_level_;
    }
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
