//----------------------------------------------------------------------------
//  EDGE FLAC Music Player
//----------------------------------------------------------------------------
//
//  Copyright (c) 2022-2023 - The EDGE Team.
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

#include "s_flac.h"

#include "ddf_playlist.h"
#include "dr_flac.h"
#include "epi.h"
#include "epi_endian.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "i_movie.h"
#include "i_sound.h"
#include "s_blit.h"
#include "s_cache.h"
#include "s_music.h"
#include "snd_gather.h"
#include "w_wad.h"

class FLACPlayer : public AbstractMusicPlayer
{
  public:
    FLACPlayer();
    ~FLACPlayer() override;

  private:
    uint8_t *flac_data_;
    drflac  *flac_decoder_;

  public:
    bool OpenMemory(uint8_t *data, int length);

    void Close(void) override;

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override;
};

//----------------------------------------------------------------------------

FLACPlayer::FLACPlayer() : flac_data_(nullptr), flac_decoder_(nullptr)
{
    status_ = kNotLoaded;
}

FLACPlayer::~FLACPlayer()
{
    Close();
}

bool FLACPlayer::OpenMemory(uint8_t *data, int length)
{
    if (status_ != kNotLoaded)
        Close();

    flac_decoder_ = drflac_open_memory(data, length, NULL);

    if (!flac_decoder_)
    {
        LogWarning("Failed to load FLAC music (corrupt flac?)\n");
        return false;
    }

    if (!OpenStream(flac_decoder_->sampleRate, flac_decoder_->channels, SDL_AUDIO_F32))
    {
        Close();
        return false;
    }

    flac_data_ = data;

    // Loaded, but not playing
    status_ = kStopped;

    return true;
}

void FLACPlayer::Close()
{
    if (status_ == kNotLoaded)
        return;

    // Stop playback
    Stop();

    CloseStream();

    if (flac_decoder_)
    {
        drflac_close(flac_decoder_);
        flac_decoder_ = nullptr;
    }

    delete[] flac_data_;
    flac_data_ = nullptr;

    status_ = kNotLoaded;
}

int FLACPlayer::StreamIntoBuffer(void *buffer, int frames)
{
    float *output      = (float *)buffer;
    int    frames_left = frames;

    while (frames_left > 0)
    {
        drflac_uint64 got = drflac_read_pcm_frames_f32(flac_decoder_, frames_left, output);

        if (got == 0)
        {
            if (!looping_)
                break;

            if (!drflac_seek_to_pcm_frame(flac_decoder_, 0))
                break;

            continue;
        }

        output += got * flac_decoder_->channels;
        frames_left -= (int)got;
    }

    return frames - frames_left;
}

//----------------------------------------------------------------------------

AbstractMusicPlayer *PlayFLACMusic(uint8_t *data, int length, bool looping)
{
    FLACPlayer *player = new FLACPlayer();

    if (!player->OpenMemory(data, length))
    {
        delete[] data;
        delete player;
        return nullptr;
    }

    // data is freed when Close() is called on the player; must be retained
    // until then

    player->Play(looping);

    return player;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
