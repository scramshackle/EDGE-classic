//----------------------------------------------------------------------------
//  EDGE WAV Sound Loader
//----------------------------------------------------------------------------
//
//  Copyright (c) 2022-2024 The EDGE Team.
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

#include "s_wav.h"

#include "dr_wav.h"
#include "epi.h"
#include "epi_endian.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "s_blit.h"
#include "s_cache.h"
#include "snd_gather.h"
#include "w_wad.h"

bool LoadWAVSound(SoundData *buf, const uint8_t *data, int length)
{
    drwav decode;

    if (!drwav_init_memory(&decode, data, length, NULL))
    {
        LogWarning("Failed to load WAV sound (corrupt wav?)\n");
        return false;
    }

    if (decode.channels > 2)
    {
        LogWarning("WAV SFX Loader: too many channels: %d\n", decode.channels);
        drwav_uninit(&decode);
        return false;
    }

    drwav_uint64 frame_count = decode.totalPCMFrameCount;

    if (frame_count == 0)
    {
        LogWarning("WAV SFX Loader: no samples!\n");
        drwav_uninit(&decode);
        return false;
    }

    LogDebug("WAV SFX Loader: freq %d Hz, %d channels\n", decode.sampleRate, decode.channels);

    bool is_stereo = (decode.channels > 1);

    buf->frequency_ = decode.sampleRate;

    SoundGatherer gather;

    float *buffer = gather.MakeChunk(frame_count, is_stereo);

    drwav_uint64 frames_read = drwav_read_pcm_frames_f32(&decode, frame_count, buffer);

    gather.CommitChunk(frames_read);

    if (!gather.Finalise(buf))
        LogWarning("WAV SFX Loader: no samples!\n");

    drwav_uninit(&decode);

    return true;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
