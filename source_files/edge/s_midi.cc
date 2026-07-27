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

bool StartupMIDI(void)
{
    LogPrint("Initializing MIDI...\n");

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

        if (midi_disabled || edge_synth == NULL)
            return false;

        if (egt_loadSongFromMemory(edge_synth, (char *)data, length, imf_rate) != 0)
        {
            LogWarning("Failed to load MIDI music\n");
            return false;
        }

        egt_play(edge_synth);

        if (!OpenStream(sound_device_frequency, 2, SDL_AUDIO_S16))
        {
            egt_clearSong(edge_synth);
            return false;
        }

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

        CloseStream();

        egt_clearSong(edge_synth);

        status_ = kNotLoaded;
    }

    void Stop(void) override
    {
        if (status_ != kPlaying && status_ != kPaused)
            return;

        AbstractMusicPlayer::Stop();

        egt_stop(edge_synth, 1);

        // reset imf_rate in case tracks are switched to another format
        imf_rate = 0;
    }

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override
    {
        egt_render(edge_synth, buffer, frames * 2, EGT_RENDER_16);

        if (epi::AlmostEquals(egt_getTime(edge_synth), egt_getSongLength(edge_synth)))
        {
            if (looping_)
                egt_setPosition(edge_synth, 0, 0, 2);
            else
                stream_ended_ = true;
        }

        return frames;
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
