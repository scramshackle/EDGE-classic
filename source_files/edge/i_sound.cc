//----------------------------------------------------------------------------
//  EDGE Sound System for SDL
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

#include "i_sound.h"

#include <SDL3/SDL.h>

#include <set>
#include <vector>

#include "ddf_reverb.h"
#include "epi.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "epi_str_compare.h"
#include "epi_str_util.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_random.h"
#include "s_blit.h"
#include "s_cache.h"
#include "s_effect.h"
#include "s_midi.h"
#include "s_music.h"
#include "s_sound.h"
#include "s_spatial.h"
#include "w_wad.h"

// If true, sound system is off/not working. Changed to false if sound init ok.
bool no_sound = false;

int  sound_device_frequency;
int  sound_device_channels = 2;
bool sector_reverb         = false;

SDL_AudioDeviceID sound_device = 0;

static SDL_AudioStream   *sound_device_stream = nullptr;
static std::vector<float> sound_device_scratch;
static bool               pending_device_format_change = false;

std::set<std::string>  available_soundbanks;
extern std::string     game_directory;
extern std::string     home_directory;
extern ConsoleVariable midi_soundbank;

static LowPassEffect vacuum_filter;
static LowPassEffect underwater_filter;
static DelayEffect   underwater_delay;

ReverbEffect reverb_effect;

EDGE_DEFINE_CONSOLE_VARIABLE_CLAMPED(dynamic_reverb, "0", kConsoleVariableFlagArchive, 0, 2)

void LockSoundMixer(void)
{
    if (sound_device_stream)
        SDL_LockAudioStream(sound_device_stream);
}

void UnlockSoundMixer(void)
{
    if (sound_device_stream)
        SDL_UnlockAudioStream(sound_device_stream);
}

void ProcessVacuumEffect(float *frames, int frame_count)
{
    vacuum_filter.Process(frames, frame_count);
}

void ProcessUnderwaterEffect(float *frames, int frame_count)
{
    underwater_delay.Process(frames, frame_count);
    underwater_filter.Process(frames, frame_count);
}

void ProcessReverbEffect(float *frames, int frame_count)
{
    reverb_effect.Process(frames, frame_count);
}

static void SDLCALL SoundDeviceCallback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                                        int total_amount)
{
    EPI_UNUSED(userdata);
    EPI_UNUSED(total_amount);

    if (additional_amount <= 0)
        return;

    int frame_bytes = (int)sizeof(float) * kSpatialChannels;
    int frames      = additional_amount / frame_bytes;

    if (frames <= 0)
        return;

    if (sound_device_scratch.size() < (size_t)(frames * kSpatialChannels))
        sound_device_scratch.resize((size_t)frames * kSpatialChannels);

    MixSoundEffects(sound_device_scratch.data(), frames);

    SDL_PutAudioStreamData(stream, sound_device_scratch.data(), frames * frame_bytes);
}

static void BuildEffects(void)
{
    SetSpatialGainSmoothTime(sound_device_frequency);

    underwater_delay.Setup(sound_device_frequency, kSpatialChannels, 0.15f, 0.15f);
    underwater_filter.Setup(sound_device_frequency, kSpatialChannels, 800.0f);
    vacuum_filter.Setup(sound_device_frequency, kSpatialChannels, 200.0f);
    reverb_effect.Setup(sound_device_frequency, kSpatialChannels);
}

static void ShutdownEffects(void)
{
    underwater_delay.Shutdown();
    underwater_filter.Shutdown();
    vacuum_filter.Shutdown();
    reverb_effect.Shutdown();
}

void StartupAudio(void)
{
    if (no_sound)
        return;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        LogPrint("StartupSound: Unable to initialize SDL audio subsystem: %s\n", SDL_GetError());
        no_sound = true;
        return;
    }

    SDL_AudioSpec requested;
    requested.format   = SDL_AUDIO_F32;
    requested.channels = kSpatialChannels;
    requested.freq     = 44100;

    sound_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested);

    if (sound_device == 0)
    {
        LogPrint("StartupSound: Unable to open audio device: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        no_sound = true;
        return;
    }

    SDL_AudioSpec obtained;

    if (!SDL_GetAudioDeviceFormat(sound_device, &obtained, NULL))
        obtained = requested;

    sound_device_frequency = obtained.freq;
    sound_device_channels  = obtained.channels;

    music_device = SDL_OpenAudioDevice(sound_device, &requested);

    if (music_device == 0)
    {
        LogPrint("StartupSound: Unable to open music device: %s\n", SDL_GetError());
        no_music = true;
    }
    else
    {
        SDL_SetAudioDeviceGain(music_device, music_volume.f_);
        SDL_ResumeAudioDevice(music_device);
    }

    BuildEffects();

    SDL_AudioSpec stream_spec;
    stream_spec.format   = SDL_AUDIO_F32;
    stream_spec.channels = kSpatialChannels;
    stream_spec.freq     = sound_device_frequency;

    sound_device_stream = SDL_CreateAudioStream(&stream_spec, NULL);

    if (!sound_device_stream)
    {
        LogPrint("StartupSound: Unable to create audio stream: %s\n", SDL_GetError());
        ShutdownEffects();
        SDL_CloseAudioDevice(sound_device);
        sound_device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        no_sound = true;
        return;
    }

    SDL_SetAudioStreamGetCallback(sound_device_stream, SoundDeviceCallback, NULL);
    SDL_BindAudioStream(sound_device, sound_device_stream);
    SDL_ResumeAudioDevice(sound_device);

    // display some useful stuff
    LogPrint("StartupSound: Success @ %d Hz, %d channels\n", sound_device_frequency, sound_device_channels);

    return;
}

void SoundDeviceFormatChanged(void)
{
    pending_device_format_change = true;
}

void ApplyPendingSoundDeviceFormatChange(void)
{
    if (!pending_device_format_change)
        return;

    pending_device_format_change = false;

    if (no_sound || sound_device == 0)
        return;

    SDL_AudioSpec obtained;

    if (!SDL_GetAudioDeviceFormat(sound_device, &obtained, NULL))
        return;

    if (obtained.freq == sound_device_frequency && obtained.channels == sound_device_channels)
        return;

    LogPrint("StartupSound: Device format changed to %d Hz, %d channels\n", obtained.freq, obtained.channels);

    int  restart_entry = entry_playing;
    bool restart_loop  = entry_looped;

    StopMusic();
    StopAllSoundEffects();

    LockSoundMixer();

    sound_device_frequency = obtained.freq;
    sound_device_channels  = obtained.channels;

    ShutdownEffects();
    BuildEffects();

    SDL_AudioSpec stream_spec;
    stream_spec.format   = SDL_AUDIO_F32;
    stream_spec.channels = kSpatialChannels;
    stream_spec.freq     = sound_device_frequency;

    SDL_SetAudioStreamFormat(sound_device_stream, &stream_spec, NULL);

    SoundCacheClearAll();

    UnlockSoundMixer();

    if (restart_entry > 0)
        ChangeMusic(restart_entry, restart_loop);
}

void AudioShutdown(void)
{
    if (no_sound)
        return;

    ShutdownSound();

    if (sound_device_stream)
    {
        SDL_DestroyAudioStream(sound_device_stream);
        sound_device_stream = nullptr;
    }

    ShutdownEffects();

    if (music_device != 0)
    {
        SDL_CloseAudioDevice(music_device);
        music_device = 0;
    }

    if (sound_device != 0)
    {
        SDL_CloseAudioDevice(sound_device);
        sound_device = 0;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    no_sound = true;
}

void StartupMusic(void)
{
    if (no_music)
        return;

    // Check for instrument banks
    std::vector<epi::DirectoryEntry> sbd;
    std::string                      soundbank_dir = epi::PathAppend(home_directory, "soundbank");

    // Create home directory soundbank folder if it doesn't aleady exist
    if (!epi::IsDirectory(soundbank_dir))
        epi::MakeDirectory(soundbank_dir);

    sbd.clear();

    if (ReadDirectory(sbd, soundbank_dir, "*.egtb"))
    {
        for (size_t i = 0; i < sbd.size(); i++)
        {
            if (!sbd[i].is_dir)
            {
                std::string filename = epi::GetFilename(sbd[i].name);
                if (!available_soundbanks.count(filename))
                    available_soundbanks.emplace(filename);
            }
        }
    }

    if (home_directory != game_directory)
    {
        // Read the program directory, but only add names we haven't encountered yet
        sbd.clear();
        soundbank_dir = epi::PathAppend(game_directory, "soundbank");

        if (ReadDirectory(sbd, soundbank_dir, "*.egtb"))
        {
            for (size_t i = 0; i < sbd.size(); i++)
            {
                if (!sbd[i].is_dir)
                {
                    std::string filename = epi::GetFilename(sbd[i].name);
                    if (!available_soundbanks.count(filename))
                        available_soundbanks.emplace(filename);
                }
            }
        }
    }

    if (!StartupMIDI())
        midi_disabled = true;

    return;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
