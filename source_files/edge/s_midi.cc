//----------------------------------------------------------------------------
//  EDGE MIDI Music Player
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

#include <stdint.h>

#include <set>

#include "HandmadeMath.h"
#include "dm_state.h"
#include "egtlib.h"
#include "epi.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "epi_math.h"
#include "epi_str_compare.h"
#include "epi_str_util.h"
#include "i_movie.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "s_blit.h"
#include "s_midi.h"
#include "s_music.h"
#include "w_files.h"

extern int sound_device_frequency;

bool midi_disabled = false;

static egtsynth         *edge_synth             = NULL;
static uint16_t          imf_rate               = 0;

EDGE_DEFINE_CONSOLE_VARIABLE(midi_soundbank, "Default.egtb", kConsoleVariableFlagArchive)

extern std::set<std::string> available_soundbanks;

typedef struct
{
    ma_data_source_base     ds;
    ma_read_proc            onRead;
    ma_seek_proc            onSeek;
    ma_tell_proc            onTell;
    void                   *pReadSeekTellUserData;
    ma_allocation_callbacks allocationCallbacks;
    ma_format               format;
    ma_uint32               channels;
    ma_uint32               sampleRate;
    ma_uint64               cursor;
} ma_midi;

static ma_result ma_midi_init(ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
                              void *pReadSeekTellUserData, const ma_decoding_backend_config *pConfig,
                              const ma_allocation_callbacks *pAllocationCallbacks, ma_midi *pMIDI);
static ma_result ma_midi_init_memory(const void *pData, size_t dataSize, const ma_decoding_backend_config *pConfig,
                                     const ma_allocation_callbacks *pAllocationCallbacks, ma_midi *pMIDI);
static void      ma_midi_uninit(ma_midi *pMIDI, const ma_allocation_callbacks *pAllocationCallbacks);
static ma_result ma_midi_read_pcm_frames(ma_midi *pMIDI, void *pFramesOut, ma_uint64 frameCount,
                                         ma_uint64 *pFramesRead);
static ma_result ma_midi_seek_to_pcm_frame(ma_midi *pMIDI, ma_uint64 frameIndex);
static ma_result ma_midi_get_data_format(const ma_midi *pMIDI, ma_format *pFormat, ma_uint32 *pChannels,
                                         ma_uint32 *pSampleRate, ma_channel *pChannelMap, size_t channelMapCap);
static ma_result ma_midi_get_cursor_in_pcm_frames(const ma_midi *pMIDI, ma_uint64 *pCursor);
static ma_result ma_midi_get_length_in_pcm_frames(const ma_midi *pMIDI, ma_uint64 *pLength);

static ma_result ma_midi_ds_read(ma_data_source *pDataSource, void *pFramesOut, ma_uint64 frameCount,
                                 ma_uint64 *pFramesRead)
{
    return ma_midi_read_pcm_frames((ma_midi *)pDataSource, pFramesOut, frameCount, pFramesRead);
}

static ma_result ma_midi_ds_seek(ma_data_source *pDataSource, ma_uint64 frameIndex)
{
    return ma_midi_seek_to_pcm_frame((ma_midi *)pDataSource, frameIndex);
}

static ma_result ma_midi_ds_get_data_format(ma_data_source *pDataSource, ma_format *pFormat, ma_uint32 *pChannels,
                                            ma_uint32 *pSampleRate, ma_channel *pChannelMap, size_t channelMapCap)
{
    return ma_midi_get_data_format((ma_midi *)pDataSource, pFormat, pChannels, pSampleRate, pChannelMap, channelMapCap);
}

static ma_result ma_midi_ds_get_cursor(ma_data_source *pDataSource, ma_uint64 *pCursor)
{
    return ma_midi_get_cursor_in_pcm_frames((ma_midi *)pDataSource, pCursor);
}

static ma_result ma_midi_ds_get_length(ma_data_source *pDataSource, ma_uint64 *pLength)
{
    return ma_midi_get_length_in_pcm_frames((ma_midi *)pDataSource, pLength);
}

static ma_data_source_vtable g_ma_midi_ds_vtable = {ma_midi_ds_read,
                                                    ma_midi_ds_seek,
                                                    ma_midi_ds_get_data_format,
                                                    ma_midi_ds_get_cursor,
                                                    ma_midi_ds_get_length,
                                                    NULL, /* onSetLooping */
                                                    0};

static ma_result ma_midi_init_internal(const ma_decoding_backend_config *pConfig, ma_midi *pMIDI)
{
    ma_result             result;
    ma_data_source_config dataSourceConfig;

    EPI_UNUSED(pConfig);

    if (pMIDI == NULL)
    {
        return MA_INVALID_ARGS;
    }

    EPI_CLEAR_MEMORY(pMIDI, ma_midi, 1);

    pMIDI->format = ma_format_s16;

    dataSourceConfig        = ma_data_source_config_init();
    dataSourceConfig.vtable = &g_ma_midi_ds_vtable;

    result = ma_data_source_init(&dataSourceConfig, &pMIDI->ds);
    if (result != MA_SUCCESS)
    {
        return result; /* Failed to initialize the base data source. */
    }

    return MA_SUCCESS;
}

static ma_result ma_midi_post_init(ma_midi *pMIDI)
{
    EPI_ASSERT(pMIDI != NULL);

    pMIDI->channels   = 2;
    pMIDI->sampleRate = sound_device_frequency;
    egt_play(edge_synth);

    return MA_SUCCESS;
}

static ma_result ma_midi_init(ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
                              void *pReadSeekTellUserData, const ma_decoding_backend_config *pConfig,
                              const ma_allocation_callbacks *pAllocationCallbacks, ma_midi *pMIDI)
{
    if (midi_disabled || edge_synth == NULL)
        return MA_ERROR;

    EPI_UNUSED(pAllocationCallbacks);

    ma_result result;

    result = ma_midi_init_internal(pConfig, pMIDI);
    if (result != MA_SUCCESS)
    {
        return result;
    }

    if (onRead == NULL || onSeek == NULL)
    {
        return MA_INVALID_ARGS; /* onRead and onSeek are mandatory. */
    }

    pMIDI->onRead                = onRead;
    pMIDI->onSeek                = onSeek;
    pMIDI->onTell                = onTell;
    pMIDI->pReadSeekTellUserData = pReadSeekTellUserData;

    return MA_SUCCESS;
}

static ma_result ma_midi_init_memory(const void *pData, size_t dataSize, const ma_decoding_backend_config *pConfig,
                                     const ma_allocation_callbacks *pAllocationCallbacks, ma_midi *pMIDI)
{
    ma_result result;

    result = ma_midi_init_internal(pConfig, pMIDI);
    if (result != MA_SUCCESS)
    {
        return result;
    }

    EPI_UNUSED(pAllocationCallbacks);

    if (egt_loadSongFromMemory(edge_synth, (char *)pData, dataSize, imf_rate) != 0)
    {
        return MA_INVALID_FILE;
    }

    result = ma_midi_post_init(pMIDI);
    if (result != MA_SUCCESS)
    {
        return result;
    }

    return MA_SUCCESS;
}

static void ma_midi_uninit(ma_midi *pMIDI, const ma_allocation_callbacks *pAllocationCallbacks)
{
    EPI_UNUSED(pAllocationCallbacks);

    if (pMIDI == NULL)
    {
        return;
    }

    ma_data_source_uninit(&pMIDI->ds);
}

static ma_result ma_midi_read_pcm_frames(ma_midi *pMIDI, void *pFramesOut, ma_uint64 frameCount, ma_uint64 *pFramesRead)
{
    if (pFramesRead != NULL)
    {
        *pFramesRead = 0;
    }

    if (frameCount == 0)
    {
        return MA_INVALID_ARGS;
    }

    if (pMIDI == NULL)
    {
        return MA_INVALID_ARGS;
    }

    /* We always use floating point format. */
    ma_result result          = MA_SUCCESS; /* Must be initialized to MA_SUCCESS. */
    ma_uint64 totalFramesRead = 0;
    ma_format format;
    ma_uint32 channels;

    ma_midi_get_data_format(pMIDI, &format, &channels, NULL, NULL, 0);

    if (format == ma_format_s16)
    {
        egt_render(edge_synth, pFramesOut, frameCount * 2, EGT_RENDER_16);
        totalFramesRead = frameCount;
    }
    else
    {
        result = MA_INVALID_ARGS;
    }

    pMIDI->cursor += totalFramesRead;

    if (pFramesRead != NULL)
    {
        *pFramesRead = totalFramesRead;
    }

    if (result == MA_SUCCESS && epi::AlmostEquals(egt_getTime(edge_synth), egt_getSongLength(edge_synth)))
    {
        result = MA_AT_END;
    }

    return result;
}

static ma_result ma_midi_seek_to_pcm_frame(ma_midi *pMIDI, ma_uint64 frameIndex)
{
    if (pMIDI == NULL || frameIndex != 0)
    {
        return MA_INVALID_ARGS;
    }

    egt_setPosition(edge_synth, 0, 0, 2);

    pMIDI->cursor = frameIndex;

    return MA_SUCCESS;
}

static ma_result ma_midi_get_data_format(const ma_midi *pMIDI, ma_format *pFormat, ma_uint32 *pChannels,
                                         ma_uint32 *pSampleRate, ma_channel *pChannelMap, size_t channelMapCap)
{
    /* Defaults for safety. */
    if (pFormat != NULL)
    {
        *pFormat = ma_format_unknown;
    }
    if (pChannels != NULL)
    {
        *pChannels = 0;
    }
    if (pSampleRate != NULL)
    {
        *pSampleRate = 0;
    }
    if (pChannelMap != NULL)
    {
        EPI_CLEAR_MEMORY(pChannelMap, ma_channel, channelMapCap);
    }

    if (pMIDI == NULL)
    {
        return MA_INVALID_OPERATION;
    }

    if (pFormat != NULL)
    {
        *pFormat = pMIDI->format;
    }

    if (pChannels != NULL)
    {
        *pChannels = pMIDI->channels;
    }

    if (pSampleRate != NULL)
    {
        *pSampleRate = pMIDI->sampleRate;
    }

    if (pChannelMap != NULL)
    {
        ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, pMIDI->channels);
    }

    return MA_SUCCESS;
}

static ma_result ma_midi_get_cursor_in_pcm_frames(const ma_midi *pMIDI, ma_uint64 *pCursor)
{
    if (pCursor == NULL)
    {
        return MA_INVALID_ARGS;
    }

    *pCursor = 0; /* Safety. */

    if (pMIDI == NULL)
    {
        return MA_INVALID_ARGS;
    }

    *pCursor = pMIDI->cursor;

    return MA_SUCCESS;
}

static ma_result ma_midi_get_length_in_pcm_frames(const ma_midi *pMIDI, ma_uint64 *pLength)
{
    if (pLength == NULL)
    {
        return MA_INVALID_ARGS;
    }

    *pLength = 0; /* Safety. */

    if (pMIDI == NULL)
    {
        return MA_INVALID_ARGS;
    }

    return MA_SUCCESS;
}

static ma_result ma_decoding_backend_init__midi(void *pUserData, ma_read_proc onRead, ma_seek_proc onSeek,
                                                ma_tell_proc onTell, void *pReadSeekTellUserData,
                                                const ma_decoding_backend_config *pConfig,
                                                const ma_allocation_callbacks    *pAllocationCallbacks,
                                                ma_data_source                  **ppBackend)
{
    ma_result result;
    ma_midi  *pMIDI;

    EPI_UNUSED(pUserData); /* For now not using pUserData, but once we start storing the vorbis decoder state within the
                              ma_decoder structure this will be set to the decoder so we can avoid a malloc. */

    /* For now we're just allocating the decoder backend on the heap. */
    pMIDI = (ma_midi *)ma_malloc(sizeof(*pMIDI), pAllocationCallbacks);
    if (pMIDI == NULL)
    {
        return MA_OUT_OF_MEMORY;
    }

    result = ma_midi_init(onRead, onSeek, onTell, pReadSeekTellUserData, pConfig, pAllocationCallbacks, pMIDI);
    if (result != MA_SUCCESS)
    {
        ma_free(pMIDI, pAllocationCallbacks);
        return result;
    }

    *ppBackend = pMIDI;

    return MA_SUCCESS;
}

static ma_result ma_decoding_backend_init_memory__midi(void *pUserData, const void *pData, size_t dataSize,
                                                       const ma_decoding_backend_config *pConfig,
                                                       const ma_allocation_callbacks    *pAllocationCallbacks,
                                                       ma_data_source                  **ppBackend)
{
    ma_result result;
    ma_midi  *pMIDI;

    EPI_UNUSED(pUserData); /* For now not using pUserData, but once we start storing the vorbis decoder state within the
                              ma_decoder structure this will be set to the decoder so we can avoid a malloc. */

    /* For now we're just allocating the decoder backend on the heap. */
    pMIDI = (ma_midi *)ma_malloc(sizeof(*pMIDI), pAllocationCallbacks);
    if (pMIDI == NULL)
    {
        return MA_OUT_OF_MEMORY;
    }

    result = ma_midi_init_memory(pData, dataSize, pConfig, pAllocationCallbacks, pMIDI);
    if (result != MA_SUCCESS)
    {
        ma_free(pMIDI, pAllocationCallbacks);
        return result;
    }

    *ppBackend = pMIDI;

    return MA_SUCCESS;
}

static void ma_decoding_backend_uninit__midi(void *pUserData, ma_data_source *pBackend,
                                             const ma_allocation_callbacks *pAllocationCallbacks)
{
    ma_midi *pMIDI = (ma_midi *)pBackend;

    EPI_UNUSED(pUserData);

    ma_midi_uninit(pMIDI, pAllocationCallbacks);
    ma_free(pMIDI, pAllocationCallbacks);
}

static ma_decoding_backend_vtable g_ma_decoding_backend_vtable_midi = {ma_decoding_backend_init__midi,
                                                                       NULL, // onInitFile()
                                                                       NULL, // onInitFileW()
                                                                       ma_decoding_backend_init_memory__midi,
                                                                       ma_decoding_backend_uninit__midi};

static ma_decoding_backend_vtable *midi_custom_vtable = &g_ma_decoding_backend_vtable_midi;

static ma_decoder_config midi_decoder_config;

bool StartupMIDI(void)
{
    LogPrint("Initializing MIDI...\n");

    midi_decoder_config                        = ma_decoder_config_init_default();
    midi_decoder_config.customBackendCount     = 1;
    midi_decoder_config.pCustomBackendUserData = NULL;
    midi_decoder_config.ppCustomBackendVTables = &midi_custom_vtable;

    // Check for presence of previous CVAR value's file
    if (!available_soundbanks.count(midi_soundbank.s_))
    {
        LogWarning("MIDI: Cannot find previously used soundbank %s, falling back to "
                   "default!\n",
                   midi_soundbank.c_str());
        midi_soundbank = "Default.egtb";
    }

    if (!edge_synth)
        edge_synth = egt_create(sound_device_frequency);

    if (!edge_synth)
    {
        LogWarning("MIDI: Initialization failure.\n");
        return false;
    }

    std::string soundbank_dir = epi::PathAppend(home_directory, "soundbank");

    epi::File *bank = epi::FileOpen(epi::PathAppend(soundbank_dir, midi_soundbank.s_), epi::kFileAccessRead|epi::kFileAccessBinary);

    if (!bank && home_directory != game_directory)
    {
        soundbank_dir = epi::PathAppend(game_directory, "soundbank");
        bank = epi::FileOpen(epi::PathAppend(soundbank_dir, midi_soundbank.s_), epi::kFileAccessRead|epi::kFileAccessBinary);
    }

    if (!bank)
    {
        LogWarning("MIDI: Could not load bank: %s.\n", midi_soundbank.s_.c_str());
        return false;
    }

    uint8_t *raw_bank = bank->LoadIntoMemory();

    if (!raw_bank)
    {
        LogWarning("MIDI: Could not load bank: %s.\n", midi_soundbank.s_.c_str());
        delete bank;
        return false;
    }

    if (egt_loadInstrumentBankFromMemory(edge_synth, (char *)raw_bank, bank->GetLength()) != 0)
    {
        LogWarning("MIDI: Could not load bank: %s.\n", midi_soundbank.s_.c_str());
        delete bank;
        delete[] raw_bank;
        return false;
    }

    delete bank;
    delete[] raw_bank;

    egt_clearSong(edge_synth);

    return true; // OK!
}

// Should only be invoked when switching soundbanks
void RestartMIDI(void)
{
    if (midi_disabled)
        return;

    LogPrint("Restarting MIDI...\n");

    int old_entry = entry_playing;

    StopMusic();

    if (!StartupMIDI())
    {
        midi_disabled = true;
        return;
    }

    ChangeMusic(old_entry,
                true); // Restart track that was playing when switched

    return;            // OK!
}

static ma_decoder midi_decoder;
static ma_sound   midi_stream;

class MIDIPlayer : public AbstractMusicPlayer
{
  public:
    MIDIPlayer(bool looping)
    {
        status_  = kNotLoaded;
        looping_ = looping;
    }

    ~MIDIPlayer() override
    {
        Close();
    }

    bool OpenMemory(const uint8_t *data, int length)
    {
        if (status_ != kNotLoaded)
            Close();

        midi_decoder_config.format = ma_format_s16;

        if (ma_decoder_init_memory(data, length, &midi_decoder_config, &midi_decoder) != MA_SUCCESS)
        {
            LogWarning("Failed to load MIDI music\n");
            return false;
        }

        if (ma_sound_init_from_data_source(&sound_engine, &midi_decoder,
                                           MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_STREAM |
                                               MA_SOUND_FLAG_UNKNOWN_LENGTH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                           NULL, &midi_stream) != MA_SUCCESS)
        {
            ma_decoder_uninit(&midi_decoder);
            LogWarning("Failed to load MIDI music\n");
            return false;
        }

        ma_node_attach_output_bus(&midi_stream, 0, &music_node, 0);

        // Loaded, but not playing
        status_ = kStopped;

        return true;
    }

    void Close(void) override
    {
        if (status_ == kNotLoaded)
            return;

        // Stop playback
        Stop();

        ma_sound_uninit(&midi_stream);

        ma_decoder_uninit(&midi_decoder);

        egt_clearSong(edge_synth);

        status_ = kNotLoaded;
    }

    void Play(bool loop) override
    {
        looping_ = loop;

        ma_sound_set_looping(&midi_stream, looping_ ? MA_TRUE : MA_FALSE);

        // Let 'er rip (maybe)
        if (playing_movie)
            status_ = kPaused;
        else
        {
            status_ = kPlaying;
            ma_sound_start(&midi_stream);
        }
    }

    void Stop(void) override
    {
        if (status_ != kPlaying && status_ != kPaused)
            return;

        ma_sound_stop(&midi_stream);

        egt_stop(edge_synth, 1);

        // reset imf_rate in case tracks are switched to another format
        imf_rate = 0;

        status_ = kStopped;
    }

    void Pause(void) override
    {
        if (status_ != kPlaying)
            return;

        ma_sound_stop(&midi_stream);

        status_ = kPaused;
    }

    void Resume(void) override
    {
        if (status_ != kPaused)
            return;

        ma_sound_start(&midi_stream);

        status_ = kPlaying;
    }

    void Ticker(void) override
    {
        if (status_ == kPlaying)
        {
            if (pc_speaker_mode)
                Stop();
            if (ma_sound_at_end(&midi_stream)) // This should only be true if finished and not set to looping
                Stop();
        }
    }
};

AbstractMusicPlayer *PlayMIDIMusic(uint8_t *data, int length, bool loop)
{
    if (midi_disabled)
    {
        delete[] data;
        return nullptr;
    }

    MIDIPlayer *player = new MIDIPlayer(loop);

    if (!player)
    {
        LogDebug("MIDI player: error initializing!\n");
        delete[] data;
        return nullptr;
    }

    if (!player->OpenMemory(data,
                            length)) // Lobo: quietly log it instead of completely exiting EDGE
    {
        LogDebug("MIDI player: failed to load MIDI file!\n");
        delete[] data;
        delete player;
        return nullptr;
    }

    delete[] data;

    player->Play(loop);

    return player;
}

AbstractMusicPlayer *PlayIMFMusic(uint8_t *data, int length, bool loop, int type)
{
    MIDIPlayer *player = new MIDIPlayer(loop);

    if (!player)
    {
        LogDebug("IMF player: error initializing!\n");
        delete[] data;
        return nullptr;
    }

    switch (type)
    {
    case kDDFMusicIMF280:
        imf_rate = 280;
        break;
    case kDDFMusicIMF560:
        imf_rate = 560;
        break;
    case kDDFMusicIMF700:
        imf_rate = 700;
        break;
    default:
        imf_rate = 0;
        break;
    }

    if (imf_rate == 0)
    {
        LogDebug("IMF player: no IMF sample rate provided!\n");
        delete[] data;
        delete player;
        return nullptr;
    }

    if (!player->OpenMemory(data, length)) // Lobo: quietly log it instead of completely exiting EDGE
    {
        LogDebug("IMF player: failed to load IMF file!\n");
        delete[] data;
        delete player;
        return nullptr;
    }

    delete[] data;

    player->Play(loop);

    return player;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
