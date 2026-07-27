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

#pragma once

#include <atomic>
#include <vector>

constexpr int kMaximumEffectChannels = 2;

class LowPassEffect
{
  public:
    LowPassEffect();
    ~LowPassEffect();

    bool Setup(int frequency, int channels, float cutoff);
    void Shutdown(void);
    void Process(float *frames, int frame_count);

  private:
    float b0_;
    float b1_;
    float b2_;
    float a1_;
    float a2_;
    float r1_[kMaximumEffectChannels];
    float r2_[kMaximumEffectChannels];
    int   channels_;
    bool  ready_;
};

class DelayEffect
{
  public:
    DelayEffect();
    ~DelayEffect();

    bool Setup(int frequency, int channels, float delay_seconds, float decay);
    void Shutdown(void);
    void Process(float *frames, int frame_count);

  private:
    std::vector<float> buffer_;
    int                buffer_frames_;
    int                cursor_;
    int                channels_;
    float              decay_;
    float              wet_;
    float              dry_;
    bool               ready_;
};

enum ReverbParameterIndex
{
    kReverbDecayTime            = 0,
    kReverbRoomSize             = 1,
    kReverbPreDelay             = 2,
    kReverbDiffusion            = 3,
    kReverbHighFrequencyDamping = 4,
    kReverbLowFrequencyDamping  = 5,
    kReverbWetLevel             = 6,
    kReverbDryLevel             = 7,
    kReverbWidth                = 8,
    kTotalReverbParameters      = 9
};

constexpr int kReverbDelayLines    = 4;
constexpr int kReverbAllpassStages = 4;

class ReverbEffect
{
  public:
    ReverbEffect();
    ~ReverbEffect();

    bool Setup(int frequency, int channels);
    void Shutdown(void);

    void SetParameter(int index, float value);
    void SetParameters(const float *values);

    void Process(float *frames, int frame_count);

  private:
    void ApplyParameters(void);

    bool  ready_;
    int   frequency_;
    int   channels_;

    std::atomic<float> parameters_[kTotalReverbParameters];
    std::atomic<bool>  pending_change_;

    float active_[kTotalReverbParameters];

    std::vector<float> pre_delay_buffer_;
    int                pre_delay_capacity_;
    int                pre_delay_length_;
    int                pre_delay_cursor_;

    std::vector<float> allpass_buffer_[kReverbAllpassStages];
    int                allpass_length_[kReverbAllpassStages];
    int                allpass_cursor_[kReverbAllpassStages];
    float              allpass_feedback_;

    std::vector<float> line_buffer_[kReverbDelayLines];
    int                line_capacity_;
    int                line_length_[kReverbDelayLines];
    int                line_cursor_[kReverbDelayLines];
    float              line_gain_[kReverbDelayLines];
    float              line_state_[kReverbDelayLines];
    float              damping_low_state_[kReverbDelayLines];
    float              high_damping_;
    float              low_damping_;
    float              low_damping_coefficient_;

    float modulation_phase_[kReverbDelayLines];
    float modulation_step_[kReverbDelayLines];
    float modulation_depth_;

    float interpolator_input_[kReverbDelayLines];
    float interpolator_output_[kReverbDelayLines];

    float wet_level_;
    float dry_level_;
    float width_;
};

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
