//----------------------------------------------------------------------------
//  EDGE Mod4Play (Tracker Module) Music Player
//----------------------------------------------------------------------------
//
//  Copyright (c) 2022-2024 - The EDGE Team.
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

#include "ddf_playlist.h"
#include "epi.h"
#include "epi_endian.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "i_movie.h"
#include "i_sound.h"
#include "m4p.h"
#include "s_blit.h"
#include "s_cache.h"
#include "s_music.h"
#include "snd_gather.h"
#include "w_wad.h"

extern int sound_device_frequency;

class M4PPlayer : public AbstractMusicPlayer
{
  public:
    M4PPlayer();
    ~M4PPlayer() override;

    bool OpenMemory(const uint8_t *data, int length, bool loop);

    void Close(void) override;

    void Play(bool loop) override;

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override;

  private:
    bool m4p_loaded_;
};

//----------------------------------------------------------------------------

M4PPlayer::M4PPlayer() : m4p_loaded_(false)
{
    status_ = kNotLoaded;
}

M4PPlayer::~M4PPlayer()
{
    Close();
}

bool M4PPlayer::OpenMemory(const uint8_t *data, int length, bool loop)
{
    if (status_ != kNotLoaded)
        Close();

    looping_ = loop;

    int rate = HMM_MIN(64000, sound_device_frequency);

    if (!m4p_LoadFromData((uint8_t *)data, length, rate, 1024))
    {
        LogWarning("M4P: failure to load song!\n");
        return false;
    }

    m4p_loaded_ = true;

    if (!OpenStream(rate, 2, SDL_AUDIO_S16))
    {
        Close();
        return false;
    }

    // Loaded, but not playing
    status_ = kStopped;

    return true;
}

void M4PPlayer::Close()
{
    if (status_ == kNotLoaded)
    {
        if (m4p_loaded_)
        {
            m4p_Close();
            m4p_FreeSong();
            m4p_loaded_ = false;
        }
        return;
    }

    // Stop playback
    Stop();

    CloseStream();

    if (m4p_loaded_)
    {
        m4p_Close();
        m4p_FreeSong();
        m4p_loaded_ = false;
    }

    status_ = kNotLoaded;
}

void M4PPlayer::Play(bool loop)
{
    EPI_UNUSED(loop); // Already handled for m4p in OpenMemory

    if (status_ != kNotLoaded && status_ != kStopped)
        return;

    m4p_PlaySong(looping_);

    AbstractMusicPlayer::Play(looping_);
}

int M4PPlayer::StreamIntoBuffer(void *buffer, int frames)
{
    m4p_GenerateSamples((int16_t *)buffer, frames);

    if (m4p_AtEnd()) // should only be the case if not looping
        stream_ended_ = true;

    return frames;
}

AbstractMusicPlayer *PlayM4PMusic(uint8_t *data, int length, bool looping)
{
    M4PPlayer *player = new M4PPlayer();

    if (!player->OpenMemory(data, length, looping))
    {
        delete[] data;
        delete player;
        return nullptr;
    }

    delete[] data;

    player->Play(looping);

    return player;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
