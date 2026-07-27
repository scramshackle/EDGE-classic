//----------------------------------------------------------------------------
//  EDGE SID Music Player
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
#include "libcRSID.h"
#include "s_blit.h"
#include "s_cache.h"
#include "s_music.h"
#include "snd_gather.h"
#include "w_wad.h"

extern int sound_device_frequency;

class SIDPlayer : public AbstractMusicPlayer
{
  public:
    SIDPlayer();
    ~SIDPlayer() override;

    bool OpenMemory(const uint8_t *data, int length);

    void Close(void) override;

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override;

  private:
    cRSID_C64instance *sid_;
    cRSID_SIDheader   *sid_header_;
};

//----------------------------------------------------------------------------

SIDPlayer::SIDPlayer() : sid_(nullptr), sid_header_(nullptr)
{
    status_ = kNotLoaded;
}

SIDPlayer::~SIDPlayer()
{
    Close();
}

bool SIDPlayer::OpenMemory(const uint8_t *data, int length)
{
    if (status_ != kNotLoaded)
        Close();

    sid_ = cRSID_init(sound_device_frequency, 0);

    if (!sid_)
    {
        LogWarning("SID: failure to initialize!\n");
        return false;
    }

    sid_header_ = cRSID_processSIDbuffer(sid_, (unsigned char *)data, length);

    if (!sid_header_)
    {
        LogWarning("SID: failure to load song!\n");
        return false;
    }

    cRSID_initSIDtune(sid_, sid_header_, 0);

    if (!OpenStream(sound_device_frequency, 2, SDL_AUDIO_S16))
        return false;

    // Loaded, but not playing
    status_ = kStopped;

    return true;
}

void SIDPlayer::Close()
{
    if (status_ == kNotLoaded)
        return;

    // Stop playback
    Stop();

    CloseStream();

    sid_        = nullptr;
    sid_header_ = nullptr;

    status_ = kNotLoaded;
}

int SIDPlayer::StreamIntoBuffer(void *buffer, int frames)
{
    cRSID_generateSound(sid_, (unsigned char *)buffer, (unsigned short)(frames * 2 * sizeof(int16_t)));

    return frames;
}

AbstractMusicPlayer *PlaySIDMusic(uint8_t *data, int length, bool looping)
{
    SIDPlayer *player = new SIDPlayer();

    if (!player->OpenMemory(data, length))
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
