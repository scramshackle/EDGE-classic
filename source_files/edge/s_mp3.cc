//----------------------------------------------------------------------------
//  EDGE MP3 Music Player
//----------------------------------------------------------------------------
//
//  Copyright (c) 2021-2024 The EDGE Team.
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

#include "s_mp3.h"

#include "ddf_playlist.h"
#include "dr_mp3.h"
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

class MP3Player : public AbstractMusicPlayer
{
  public:
    MP3Player();
    ~MP3Player() override;

  private:
    const uint8_t *mp3_data_;
    drmp3          mp3_decoder_;
    bool           mp3_opened_;

  public:
    bool OpenMemory(const uint8_t *data, int length);

    void Close(void) override;

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override;
};

//----------------------------------------------------------------------------

MP3Player::MP3Player() : mp3_data_(nullptr), mp3_opened_(false)
{
    status_ = kNotLoaded;
}

MP3Player::~MP3Player()
{
    Close();
}

bool MP3Player::OpenMemory(const uint8_t *data, int length)
{
    if (status_ != kNotLoaded)
        Close();

    if (!drmp3_init_memory(&mp3_decoder_, data, length, NULL))
    {
        LogWarning("Failed to load MP3 music (corrupt mp3?)\n");
        return false;
    }

    mp3_opened_ = true;

    if (!OpenStream(mp3_decoder_.sampleRate, mp3_decoder_.channels, SDL_AUDIO_F32))
    {
        Close();
        return false;
    }

    mp3_data_ = data;

    // Loaded, but not playing
    status_ = kStopped;

    return true;
}

void MP3Player::Close()
{
    if (status_ == kNotLoaded)
        return;

    // Stop playback
    Stop();

    CloseStream();

    if (mp3_opened_)
    {
        drmp3_uninit(&mp3_decoder_);
        mp3_opened_ = false;
    }

    delete[] mp3_data_;
    mp3_data_ = nullptr;

    status_ = kNotLoaded;
}

int MP3Player::StreamIntoBuffer(void *buffer, int frames)
{
    float *output      = (float *)buffer;
    int    frames_left = frames;

    while (frames_left > 0)
    {
        drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3_decoder_, frames_left, output);

        if (got == 0)
        {
            if (!looping_)
                break;

            if (!drmp3_seek_to_pcm_frame(&mp3_decoder_, 0))
                break;

            continue;
        }

        output += got * mp3_decoder_.channels;
        frames_left -= (int)got;
    }

    return frames - frames_left;
}

//----------------------------------------------------------------------------

AbstractMusicPlayer *PlayMP3Music(uint8_t *data, int length, bool looping)
{
    MP3Player *player = new MP3Player();

    if (!player->OpenMemory(data, length))
    {
        delete[] data;
        delete player;
        return nullptr;
    }

    player->Play(looping);

    return player;
}

bool LoadMP3Sound(SoundData *buf, const uint8_t *data, int length)
{
    drmp3 decode;

    if (!drmp3_init_memory(&decode, data, length, NULL))
    {
        LogWarning("Failed to load MP3 sound (corrupt mp3?)\n");
        return false;
    }

    if (decode.channels > 2)
    {
        LogWarning("MP3 SFX Loader: too many channels: %d\n", decode.channels);
        drmp3_uninit(&decode);
        return false;
    }

    drmp3_uint64 frame_count = drmp3_get_pcm_frame_count(&decode);

    if (frame_count == 0)
    {
        LogWarning("MP3 SFX Loader: no samples!\n");
        drmp3_uninit(&decode);
        return false;
    }

    LogDebug("MP3 SFX Loader: freq %d Hz, %d channels\n", decode.sampleRate, decode.channels);

    bool is_stereo = (decode.channels > 1);

    buf->frequency_ = decode.sampleRate;

    SoundGatherer gather;

    float *buffer = gather.MakeChunk(frame_count, is_stereo);

    drmp3_uint64 frames_read = drmp3_read_pcm_frames_f32(&decode, frame_count, buffer);

    gather.CommitChunk(frames_read);

    if (!gather.Finalise(buf))
        LogWarning("MP3 SFX Loader: no samples!\n");

    drmp3_uninit(&decode);

    return true;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
