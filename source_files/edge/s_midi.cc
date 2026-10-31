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

#include "s_midi.h"

#include <stdint.h>

#include <set>

#include "dm_state.h"
#include "ecsynth.h"
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
#include "s_music.h"
#include "w_files.h"

extern int sound_device_frequency;

bool midi_disabled = false;

static ECSynth          *edge_synth             = NULL;
static uint16_t          imf_rate               = 0;

EDGE_DEFINE_CONSOLE_VARIABLE_CLAMPED(midi_soundbank, "0", kConsoleVariableFlagArchive, 0, 1)

bool StartupMIDI(void)
{
    epi::LogMessage(epi::kLogLevelPrint, "Initializing MIDI...\n");

    if (!edge_synth)
        edge_synth = ECSynthCreate(sound_device_frequency);

    if (!edge_synth)
    {
        epi::LogMessage(epi::kLogLevelWarning, "MIDI: Initialization failure.\n");
        return false;
    }

    if (ECSynthLoadBuiltinBank(edge_synth, midi_soundbank.d_ ? kECSynthBankOrganya : kECSynthBankDefault) != 1)
    {
        epi::LogMessage(epi::kLogLevelWarning, "MIDI: Could not load bank: %s.\n", midi_soundbank.s_.c_str());
        return false;
    }

    return true; // OK!
}

// Should only be invoked when switching soundbanks
void RestartMIDI(void)
{
    if (midi_disabled)
        return;

    epi::LogMessage(epi::kLogLevelPrint, "Restarting MIDI...\n");

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

        ECSynthSongFormat format = kECSynthFormatAuto;

        switch (imf_rate)
        {
            case 280:
                format = kECSynthFormatImf280;
                break;
            case 560:
                format = kECSynthFormatImf560;
                break;
            case 700:
                format = kECSynthFormatImf700;
                break;
            default:
                break;
        }

        if (ECSynthLoadSongFromMemory(edge_synth, data, length, format) != 1)
        {
            epi::LogMessage(epi::kLogLevelWarning, "Failed to load MIDI music\n");
            return false;
        }

        if (!OpenStream(sound_device_frequency, 2, SDL_AUDIO_S16))
            return false;

        ECSynthPlay(edge_synth, 0);

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

        status_ = kNotLoaded;
    }

    void Stop(void) override
    {
        if (status_ != kPlaying && status_ != kPaused)
            return;

        AbstractMusicPlayer::Stop();

        ECSynthStop(edge_synth);

        // reset imf_rate in case tracks are switched to another format
        imf_rate = 0;
    }

  protected:
    int StreamIntoBuffer(void *buffer, int frames) override
    {
        ECSynthRender(edge_synth, buffer, frames * 2, kECSynthRender16);

        if (epi::AlmostEquals(ECSynthGetTime(edge_synth), ECSynthGetSongLength(edge_synth)))
        {
            if (looping_)
                ECSynthPlay(edge_synth, 0);
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
        epi::LogMessage(epi::kLogLevelDebug, "MIDI player: error initializing!\n");
        delete[] data;
        return nullptr;
    }

    if (!player->OpenMemory(data,
                            length)) // Lobo: quietly log it instead of completely exiting EDGE
    {
        epi::LogMessage(epi::kLogLevelDebug, "MIDI player: failed to load MIDI file!\n");
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
        epi::LogMessage(epi::kLogLevelDebug, "IMF player: error initializing!\n");
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
        epi::LogMessage(epi::kLogLevelDebug, "IMF player: no IMF sample rate provided!\n");
        delete[] data;
        delete player;
        return nullptr;
    }

    if (!player->OpenMemory(data, length)) // Lobo: quietly log it instead of completely exiting EDGE
    {
        epi::LogMessage(epi::kLogLevelDebug, "IMF player: failed to load IMF file!\n");
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
