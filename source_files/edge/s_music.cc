//----------------------------------------------------------------------------
//  EDGE Music handling Code
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
// -ACB- 1999/11/13 Written
//

#include "s_music.h"

#include <stdlib.h>

#include "ddf_main.h"
#include "dm_state.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "epi_str_util.h"
#include "i_movie.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "s_flac.h"
#include "s_midi.h"
#include "s_m4p.h"
#include "s_sid.h"
#include "s_mp3.h"
#include "s_ogg.h"
#include "s_sound.h"
#include "snd_types.h"
#include "w_files.h"
#include "w_wad.h"

// music slider value
EDGE_DEFINE_CONSOLE_VARIABLE(music_volume, "0.15", kConsoleVariableFlagArchive)

bool no_music = false;

// Current music handle
static AbstractMusicPlayer *music_player;

int  entry_playing = -1;
bool entry_looped;
bool pc_speaker_mode = false;

SDL_AudioDeviceID music_device = 0;

AbstractMusicPlayer::AbstractMusicPlayer()
    : status_(kNotLoaded), looping_(false), stream_(nullptr), stream_frame_bytes_(0), stream_paused_(false),
      stream_ended_(false)
{
}

AbstractMusicPlayer::~AbstractMusicPlayer()
{
    CloseStream();
}

void SDLCALL AbstractMusicPlayer::StreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                                                 int total_amount)
{
    EPI_UNUSED(total_amount);

    AbstractMusicPlayer *player = (AbstractMusicPlayer *)userdata;

    if (additional_amount <= 0 || player->stream_paused_ || player->stream_ended_)
        return;

    int frames = additional_amount / player->stream_frame_bytes_;

    if (frames <= 0)
        return;

    if (player->stream_scratch_.size() < (size_t)additional_amount)
        player->stream_scratch_.resize((size_t)additional_amount);

    int frames_filled = player->StreamIntoBuffer(player->stream_scratch_.data(), frames);

    if (frames_filled > 0)
        SDL_PutAudioStreamData(stream, player->stream_scratch_.data(), frames_filled * player->stream_frame_bytes_);

    if (frames_filled < frames)
        player->stream_ended_ = true;
}

bool AbstractMusicPlayer::OpenStream(int frequency, int channels, SDL_AudioFormat format)
{
    CloseStream();

    SDL_AudioSpec spec;
    spec.format   = format;
    spec.channels = channels;
    spec.freq     = frequency;

    stream_ = SDL_CreateAudioStream(&spec, NULL);

    if (!stream_)
    {
        LogWarning("Failed to create music stream: %s\n", SDL_GetError());
        return false;
    }

    stream_frame_bytes_ = (int)SDL_AUDIO_BYTESIZE(format) * channels;
    stream_paused_      = true;
    stream_ended_       = false;

    SDL_SetAudioStreamGetCallback(stream_, StreamCallback, this);

    if (!SDL_BindAudioStream(music_device, stream_))
    {
        LogWarning("Failed to bind music stream: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        return false;
    }

    return true;
}

void AbstractMusicPlayer::CloseStream(void)
{
    if (!stream_)
        return;

    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

void AbstractMusicPlayer::Play(bool loop)
{
    if (status_ != kNotLoaded && status_ != kStopped)
        return;

    looping_ = loop;

    if (playing_movie)
    {
        status_ = kPaused;
        return;
    }

    status_ = kPlaying;

    if (stream_)
    {
        SDL_LockAudioStream(stream_);
        stream_paused_ = false;
        SDL_UnlockAudioStream(stream_);
    }
}

void AbstractMusicPlayer::Stop(void)
{
    if (status_ != kPlaying && status_ != kPaused)
        return;

    if (stream_)
    {
        SDL_LockAudioStream(stream_);
        stream_paused_ = true;
        SDL_UnlockAudioStream(stream_);
        SDL_ClearAudioStream(stream_);
    }

    status_ = kStopped;
}

void AbstractMusicPlayer::Pause(void)
{
    if (status_ != kPlaying)
        return;

    if (stream_)
    {
        SDL_LockAudioStream(stream_);
        stream_paused_ = true;
        SDL_UnlockAudioStream(stream_);
    }

    status_ = kPaused;
}

void AbstractMusicPlayer::Resume(void)
{
    if (status_ != kPaused)
        return;

    if (stream_)
    {
        SDL_LockAudioStream(stream_);
        stream_paused_ = false;
        SDL_UnlockAudioStream(stream_);
    }

    status_ = kPlaying;
}

void AbstractMusicPlayer::Ticker(void)
{
    if (status_ != kPlaying)
        return;

    if (pc_speaker_mode)
    {
        Stop();
        return;
    }

    if (stream_ended_ && stream_ && SDL_GetAudioStreamQueued(stream_) == 0)
        Stop();
}

void ChangeMusic(int entry_number, bool loop)
{
    if (no_music)
        return;

    // -AJA- playlist number 0 reserved to mean "no music"
    if (entry_number <= 0)
    {
        StopMusic();
        return;
    }

    // -AJA- don't restart the current song (DOOM compatibility)
    if (entry_number == entry_playing && entry_looped)
        return;

    StopMusic();

    entry_playing = entry_number;
    entry_looped  = loop;

    // when we cannot find the music entry, no music will play
    const PlaylistEntry *play = playlist.Find(entry_number);
    if (!play)
    {
        LogWarning("Could not find music entry [%d]\n", entry_number);
        return;
    }

    // open the file or lump, and read it into memory
    epi::File *F;

    switch (play->infotype_)
    {
    case kDDFMusicDataFile: {
        std::string fn = epi::PathAppendIfNotAbsolute(game_directory, play->info_);

        F = epi::FileOpen(fn, epi::kFileAccessRead | epi::kFileAccessBinary);
        if (!F)
        {
            LogWarning("ChangeMusic: Can't Find File '%s'\n", fn.c_str());
            return;
        }
        break;
    }

    case kDDFMusicDataPackage: {
        F = OpenFileFromPack(play->info_);
        if (!F)
        {
            LogWarning("ChangeMusic: PK3 entry '%s' not found.\n", play->info_.c_str());
            return;
        }
        break;
    }

    case kDDFMusicDataLump: {
        int lump = CheckLumpNumberForName(play->info_.c_str());
        if (lump < 0)
        {
            LogWarning("ChangeMusic: LUMP '%s' not found.\n", play->info_.c_str());
            return;
        }

        F = LoadLumpAsFile(lump);
        break;
    }

    default:
        LogPrint("ChangeMusic: invalid method %d for MUS/MIDI\n", play->infotype_);
        return;
    }

    int      length = F->GetLength();
    uint8_t *data   = F->LoadIntoMemory();

    if (!data)
    {
        delete F;
        LogWarning("ChangeMusic: Error loading data.\n");
        return;
    }
    if (length < 4)
    {
        delete F;
        delete[] data;
        LogPrint("ChangeMusic: ignored short data (%d bytes)\n", length);
        return;
    }

    SoundFormat fmt = kSoundUnknown;

    // IMF Music is the outlier in that it must be predefined in DDFPLAY with
    // the appropriate IMF frequency, as there is no way of determining this
    // from file information alone
    if (play->type_ == kDDFMusicIMF280 || play->type_ == kDDFMusicIMF560 || play->type_ == kDDFMusicIMF700)
        fmt = kSoundIMF;
    else
    {
        if (play->infotype_ == kDDFMusicDataLump)
        {
            // lumps must use auto-detection based on their contents
            fmt = DetectSoundFormat(data, length);
        }
        else
        {
            // for FILE and PACK, use the file extension
            fmt = SoundFilenameToFormat(play->info_);
        }
    }

    // NOTE: players are responsible for freeing 'data'

    switch (fmt)
    {
    case kSoundOGG:
        delete F;
        music_player = PlayOGGMusic(data, length, loop);
        break;
    case kSoundMP3:
        delete F;
        music_player = PlayMP3Music(data, length, loop);
        break;
    case kSoundFLAC:
        delete F;
        music_player = PlayFLACMusic(data, length, loop);
        break;
    case kSoundM4P:
        delete F;
        music_player = PlayM4PMusic(data, length, loop);
        break;
    case kSoundSID:
        delete F;
        music_player = PlaySIDMusic(data, length, loop);
        break;
    case kSoundIMF:
        delete F;
        music_player = PlayIMFMusic(data, length, loop, play->type_);
        break;
    case kSoundMUS:
    case kSoundMIDI:
        delete F;
        music_player = PlayMIDIMusic(data, length, loop);
        break;

    default:
        delete F;
        delete[] data;
        LogPrint("ChangeMusic: unknown format\n");
        break;
    }
}

void ResumeMusic(void)
{
    if (music_player)
        music_player->Resume();
}

void PauseMusic(void)
{
    if (music_player)
        music_player->Pause();
}

void StopMusic(void)
{
    // You can't stop the rock!! This does...

    if (music_player)
    {
        music_player->Stop();
        delete music_player;
        music_player = nullptr;
    }

    entry_playing = -1;
    entry_looped  = false;
}

void MusicTicker(void)
{
    if (music_device != 0)
        SDL_SetAudioDeviceGain(music_device, music_volume.f_);

    if (music_player)
        music_player->Ticker();
}
//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
