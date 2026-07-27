//----------------------------------------------------------------------------
//  EDGE OGG Music Player
//----------------------------------------------------------------------------
//
//  Copyright (c) 2004-2024 The EDGE Team.
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

#include "s_ogg.h"

#include "epi.h"
#include "epi_endian.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "i_movie.h"
#include "i_sound.h"
// clang-format off
#define OV_EXCLUDE_STATIC_CALLBACKS
#include "minivorbis.h"
// clang-format on
#include "s_blit.h"
#include "s_cache.h"
#include "s_music.h"
#include "snd_gather.h"

static size_t ogg_epi_memread(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    epi::MemFile *d = (epi::MemFile *)datasource;
    return d->Read(ptr, size * nmemb) / size;
}

static int ogg_epi_memseek(void *datasource, ogg_int64_t offset, int whence)
{
    epi::MemFile *d = (epi::MemFile *)datasource;

    switch (whence)
    {
    case SEEK_SET: {
        return d->Seek(offset, epi::File::kSeekpointStart) ? 0 : -1;
    }
    case SEEK_CUR: {
        return d->Seek(offset, epi::File::kSeekpointCurrent) ? 0 : -1;
    }
    case SEEK_END: {
        return d->Seek(-offset, epi::File::kSeekpointEnd) ? 0 : -1;
    }
    default: {
        return -1;
    } // WTF?
    }
}

static int ogg_epi_memclose(void *datasource)
{
    // we don't free the data here
    EPI_UNUSED(datasource);
    return 0;
}

static long ogg_epi_memtell(void *datasource)
{
    epi::MemFile *d = (epi::MemFile *)datasource;
    return d->GetPosition();
}

static constexpr ov_callbacks ogg_epi_callbacks = {ogg_epi_memread, ogg_epi_memseek, ogg_epi_memclose, ogg_epi_memtell};

class OGGPlayer : public AbstractMusicPlayer
{
  public:
    OGGPlayer();
    ~OGGPlayer() override;

  private:
    const uint8_t *ogg_data_;
    epi::MemFile  *ogg_memfile_;
    OggVorbis_File ogg_file_;
    bool           ogg_opened_;
    int            ogg_channels_;

  public:
    bool OpenMemory(const uint8_t *data, int length);

    void Close(void) override;

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override;
};

//----------------------------------------------------------------------------

OGGPlayer::OGGPlayer() : ogg_data_(nullptr), ogg_memfile_(nullptr), ogg_opened_(false), ogg_channels_(0)
{
    status_ = kNotLoaded;
}

OGGPlayer::~OGGPlayer()
{
    Close();
}

bool OGGPlayer::OpenMemory(const uint8_t *data, int length)
{
    if (status_ != kNotLoaded)
        Close();

    ogg_memfile_ = new epi::MemFile(data, length);

    if (ov_open_callbacks((void *)ogg_memfile_, &ogg_file_, NULL, 0, ogg_epi_callbacks) < 0)
    {
        delete ogg_memfile_;
        ogg_memfile_ = nullptr;
        LogWarning("Failed to load OGG music (corrupt ogg?)\n");
        return false;
    }

    ogg_opened_ = true;

    const vorbis_info *info = ov_info(&ogg_file_, -1);

    if (!info)
    {
        Close();
        LogWarning("Failed to load OGG music (corrupt ogg?)\n");
        return false;
    }

    ogg_channels_ = info->channels;

    if (!OpenStream(info->rate, ogg_channels_, SDL_AUDIO_F32))
    {
        Close();
        return false;
    }

    ogg_data_ = data;

    // Loaded, but not playing
    status_ = kStopped;

    return true;
}

void OGGPlayer::Close()
{
    if (status_ == kNotLoaded)
        return;

    // Stop playback
    Stop();

    CloseStream();

    if (ogg_opened_)
    {
        ov_clear(&ogg_file_);
        ogg_opened_ = false;
    }

    if (ogg_memfile_)
    {
        delete ogg_memfile_;
        ogg_memfile_ = nullptr;
    }

    delete[] ogg_data_;
    ogg_data_ = nullptr;

    status_ = kNotLoaded;
}

int OGGPlayer::StreamIntoBuffer(void *buffer, int frames)
{
    float *output      = (float *)buffer;
    int    frames_left = frames;

    while (frames_left > 0)
    {
        float **planes  = nullptr;
        int     section = 0;
        long    got     = ov_read_float(&ogg_file_, &planes, frames_left, &section);

        if (got <= 0)
        {
            if (!looping_)
                break;

            if (ov_pcm_seek(&ogg_file_, 0) != 0)
                break;

            continue;
        }

        for (int c = 0; c < ogg_channels_; c++)
        {
            for (int i = 0; i < got; i++)
                output[i * ogg_channels_ + c] = planes[c][i];
        }

        output += got * ogg_channels_;
        frames_left -= got;
    }

    return frames - frames_left;
}

//----------------------------------------------------------------------------

AbstractMusicPlayer *PlayOGGMusic(uint8_t *data, int length, bool looping)
{
    OGGPlayer *player = new OGGPlayer();

    if (!player->OpenMemory(data, length))
    {
        delete[] data;
        delete player;
        return nullptr;
    }

    player->Play(looping);

    return player;
}

bool LoadOGGSound(SoundData *buf, const uint8_t *data, int length)
{
    epi::MemFile *memfile = new epi::MemFile(data, length);

    OggVorbis_File ogg_file;

    if (ov_open_callbacks((void *)memfile, &ogg_file, NULL, 0, ogg_epi_callbacks) < 0)
    {
        delete memfile;
        LogWarning("Failed to load OGG sound (corrupt ogg?)\n");
        return false;
    }

    const vorbis_info *info = ov_info(&ogg_file, -1);

    if (!info)
    {
        ov_clear(&ogg_file);
        delete memfile;
        LogWarning("Failed to load OGG sound (corrupt ogg?)\n");
        return false;
    }

    if (info->channels > 2)
    {
        LogWarning("OGG SFX Loader: too many channels: %d\n", info->channels);
        ov_clear(&ogg_file);
        delete memfile;
        return false;
    }

    ogg_int64_t frame_count = ov_pcm_total(&ogg_file, -1);

    if (frame_count <= 0)
    {
        LogWarning("OGG SFX Loader: no samples!\n");
        ov_clear(&ogg_file);
        delete memfile;
        return false;
    }

    LogDebug("OGG SFX Loader: freq %d Hz, %d channels\n", (int)info->rate, info->channels);

    int  channels  = info->channels;
    bool is_stereo = (channels > 1);

    buf->frequency_ = info->rate;

    SoundGatherer gather;

    float *buffer = gather.MakeChunk(frame_count, is_stereo);

    ogg_int64_t frames_read = 0;

    while (frames_read < frame_count)
    {
        float **planes  = nullptr;
        int     section = 0;
        long    got     = ov_read_float(&ogg_file, &planes, (int)(frame_count - frames_read), &section);

        if (got <= 0)
            break;

        float *output = buffer + frames_read * channels;

        for (int c = 0; c < channels; c++)
        {
            for (int i = 0; i < got; i++)
                output[i * channels + c] = planes[c][i];
        }

        frames_read += got;
    }

    gather.CommitChunk(frames_read);

    if (!gather.Finalise(buf))
        LogWarning("OGG SFX Loader: no samples!\n");

    ov_clear(&ogg_file);
    delete memfile;

    return true;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
