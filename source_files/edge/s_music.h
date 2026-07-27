//----------------------------------------------------------------------------
//  EDGE Music Handling Code
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
//  Based on the DOOM source code, released by Id Software under the
//  following copyright:
//
//    Copyright (C) 1993-1996 by id Software, Inc.
//
//----------------------------------------------------------------------------

#pragma once

#include <SDL3/SDL_audio.h>
#include <stdint.h>

#include <vector>

#include "con_var.h"

/* abstract base class */
class AbstractMusicPlayer
{
  public:
    enum Status
    {
        kNotLoaded,
        kPlaying,
        kPaused,
        kStopped
    };

    Status status_;
    bool   looping_;

  public:
    AbstractMusicPlayer();
    virtual ~AbstractMusicPlayer();

    virtual void Close(void) = 0;

    virtual void Play(bool loop);
    virtual void Stop(void);

    virtual void Pause(void);
    virtual void Resume(void);

    virtual void Ticker(void);

  protected:
    bool OpenStream(int frequency, int channels, SDL_AudioFormat format);
    void CloseStream(void);

    virtual int StreamIntoBuffer(void *buffer, int frames) = 0;

    SDL_AudioStream *stream_;
    int              stream_frame_bytes_;
    bool             stream_paused_;
    bool             stream_ended_;

  private:
    static void SDLCALL StreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                                       int total_amount);

    std::vector<uint8_t> stream_scratch_;
};

/* VARIABLES */

extern ConsoleVariable music_volume;
extern int             entry_playing;
extern bool            entry_looped;
extern bool            pc_speaker_mode;

/* FUNCTIONS */

void ChangeMusic(int entry_number, bool loop);
void ResumeMusic(void);
void PauseMusic(void);
void StopMusic(void);
void MusicTicker(void);

extern SDL_AudioDeviceID music_device;

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
