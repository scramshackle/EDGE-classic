//----------------------------------------------------------------------------
//  EDGE Movie Playback (MPEG)
//----------------------------------------------------------------------------
//
//  Copyright (c) 2018-2024 The EDGE Team
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

#include "e_event.h"
#include "epi.h"
#include "hu_draw.h"
#include "i_defs_gl.h"
#include "r_movie.h"
#include "i_sound.h"
#include "i_system.h"
#include "pl_mpeg.h"
#include "r_backend.h"
#include "r_gldefs.h"
#include "r_modes.h"
#include "r_state.h"
#include "r_units.h"
#include "r_wipe.h"
#include "s_blit.h"
#include "s_music.h"
#include "s_sound.h"
#include "w_files.h"
#include "w_wad.h"

extern int sound_device_frequency;

bool             playing_movie = false;
static bool      skip_bar_active;
static float     luma_scale_x      = 1.0f;
static float     luma_scale_y      = 1.0f;
static float     chroma_scale_x    = 1.0f;
static float     chroma_scale_y    = 1.0f;
static plm_t    *decoder           = nullptr;
static int       movie_sample_rate = 0;
static float     skip_time;
static uint8_t  *movie_bytes  = nullptr;
static double    fadein       = 0;
static double    fadeout      = 0;
static double    elapsed_time = 0;
static float     vx1          = 0.0f;
static float     vx2          = 0.0f;
static float     vy1          = 0.0f;
static float     vy2          = 0.0f;
static float     tx1          = 0.0f;
static float     tx2          = 1.0f;
static float     ty1          = 0.0f;
static float     ty2          = 1.0f;
static double           last_time        = 0;
static double           movie_start_time = 0;
static SDL_AudioStream *movie_stream;
static int              movie_stream_queue_limit;
static bool             canvas_can_update;

static bool MovieSetupAudioStream(int rate)
{
    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq     = rate;

    movie_stream = SDL_CreateAudioStream(&spec, NULL);

    if (!movie_stream)
    {
        LogWarning("MovieSetupAudioStream: Failed to create the audio stream.");
        return false;
    }

    if (!SDL_BindAudioStream(music_device, movie_stream))
    {
        SDL_DestroyAudioStream(movie_stream);
        movie_stream = nullptr;
        LogWarning("MovieSetupAudioStream: Failed to bind the audio stream.");
        return false;
    }

    movie_stream_queue_limit = PLM_AUDIO_SAMPLES_PER_FRAME * 4 * 2 * (int)sizeof(float);

    plm_set_audio_lead_time(decoder, (double)1024 / (double)rate);
    PauseMusic();
    SDL_SetAudioDeviceGain(music_device, music_volume.f_);
    return true;
}

void MovieAudioCallback(plm_t *mpeg, plm_samples_t *samples, void *user)
{
    EPI_UNUSED(mpeg);
    EPI_UNUSED(user);

    if (!samples || !movie_stream || pc_speaker_mode)
        return;

    if (SDL_GetAudioStreamQueued(movie_stream) >= movie_stream_queue_limit)
        return;

    SDL_PutAudioStreamData(movie_stream, samples->interleaved, (int)(samples->count * 2 * sizeof(float)));
}

void MovieVideoCallback(plm_t *mpeg, plm_frame_t *frame, void *user)
{
    EPI_UNUSED(mpeg);
    EPI_UNUSED(user);

    if (canvas_can_update)
    {
        MovieUploadPlanes(frame->y.data, frame->cb.data, frame->cr.data);

        canvas_can_update = false;
    }
}

void PlayMovie(const std::string &name)
{
    MovieDefinition *movie = moviedefs.Lookup(name.c_str());

    if (!movie)
    {
        LogWarning("PlayMovie: Movie definition %s not found!\n", name.c_str());
        return;
    }

    playing_movie   = false;
    skip_bar_active = false;
    skip_time       = 0;

    int length = 0;

    if (movie->type_ == kMovieDataLump)
        movie_bytes = LoadLumpIntoMemory(movie->info_.c_str(), &length);
    else
    {
        epi::File *mf = OpenFileFromPack(movie->info_);
        if (mf)
        {
            movie_bytes = mf->LoadIntoMemory();
            length      = mf->GetLength();
        }
        delete mf;
    }

    if (!movie_bytes)
    {
        LogWarning("PlayMovie: Could not open %s!\n", movie->info_.c_str());
        return;
    }

    if (decoder)
    {
        plm_destroy(decoder);
        decoder = nullptr;
    }

    decoder = plm_create_with_memory(movie_bytes, length, 0);

    if (!decoder)
    {
        LogWarning("PlayMovie: Could not open %s!\n", name.c_str());
        delete[] movie_bytes;
        movie_bytes = nullptr;
        return;
    }

    if (!no_sound && !(movie->special_ & kMovieSpecialMute) && plm_get_num_audio_streams(decoder) > 0)
    {
        movie_sample_rate = plm_get_samplerate(decoder);
        if (!MovieSetupAudioStream(movie_sample_rate))
        {
            plm_destroy(decoder);
            delete[] movie_bytes;
            movie_bytes = nullptr;
            decoder     = nullptr;
            return;
        }
    }

    int   movie_width  = plm_get_width(decoder);
    int   movie_height = plm_get_height(decoder);
    float movie_ratio  = (float)movie_width / movie_height;
    // Size frame using DDFMOVIE scaling selection
    // Should only need to be set once unless at some point
    // we allow menu access/console while a movie is playing
    int frame_height = 0;
    int frame_width  = 0;
    tx1              = 0.0f;
    tx2              = 1.0f;
    ty1              = 0.0f;
    ty2              = 1.0f;
    if (movie->scaling_ == kMovieScalingAutofit)
    {
        // If movie and display ratios match (ish), stretch it
        if (fabs((float)current_screen_width / current_screen_height / movie_ratio - 1.0f) <= 0.10f)
        {
            frame_height = current_screen_height;
            frame_width  = current_screen_width;
        }
        else // Zoom
        {
            frame_height = current_screen_height;
            frame_width  = RoundToInteger((float)current_screen_height * movie_ratio);
        }
    }
    else if (movie->scaling_ == kMovieScalingNoScale)
    {
        frame_height = movie_height;
        frame_width  = movie_width;
    }
    else if (movie->scaling_ == kMovieScalingZoom)
    {
        frame_height = current_screen_height;
        frame_width  = RoundToInteger((float)current_screen_height * movie_ratio);
    }
    else // Stretch, aspect ratio gets BTFO potentially
    {
        frame_height = current_screen_height;
        frame_width  = current_screen_width;
    }

    plm_frame_t *first_frame = plm_decode_video(decoder);

    if (!first_frame)
    {
        LogWarning("PlayMovie: could not decode a frame from %s!\n", name.c_str());
        plm_destroy(decoder);
        decoder = nullptr;
        delete[] movie_bytes;
        movie_bytes = nullptr;
        return;
    }

    MoviePlaneSizes plane_sizes;
    plane_sizes.luma_width    = (int)first_frame->y.width;
    plane_sizes.luma_height   = (int)first_frame->y.height;
    plane_sizes.chroma_width  = (int)first_frame->cb.width;
    plane_sizes.chroma_height = (int)first_frame->cb.height;

    MovieSetupPlanes(plane_sizes);

    luma_scale_x   = (float)movie_width / (float)plane_sizes.luma_width;
    luma_scale_y   = (float)movie_height / (float)plane_sizes.luma_height;
    chroma_scale_x = (float)(movie_width / 2) / (float)plane_sizes.chroma_width;
    chroma_scale_y = (float)(movie_height / 2) / (float)plane_sizes.chroma_height;

    MovieUploadPlanes(first_frame->y.data, first_frame->cb.data, first_frame->cr.data);

    vx1 = current_screen_width / 2 - frame_width / 2;
    vx2 = current_screen_width / 2 + frame_width / 2;
    vy1 = current_screen_height / 2 + frame_height / 2;
    vy2 = current_screen_height / 2 - frame_height / 2;

    plm_set_video_decode_callback(decoder, MovieVideoCallback, nullptr);

    if (movie_stream)
    {
        plm_set_audio_decode_callback(decoder, MovieAudioCallback, nullptr);
        plm_set_audio_enabled(decoder, 1);
        plm_set_audio_stream(decoder, 0);
    }
    else
        plm_set_audio_enabled(decoder, 0);

    BlackoutWipeTexture();

    last_time        = (double)GetMilliseconds() / 1000.0;
    movie_start_time = last_time;
    fadein           = 0;
    fadeout          = 0;

    playing_movie     = true;
    canvas_can_update = true;
}

static void EndMovie()
{
    plm_destroy(decoder);
    decoder = nullptr;
    delete[] movie_bytes;
    movie_bytes = nullptr;
    MovieReleasePlanes();
    if (movie_stream)
    {
        SDL_DestroyAudioStream(movie_stream);
        movie_stream = nullptr;
    }
    ResumeMusic();
}

void MovieDrawer()
{
    if (!playing_movie)
        return;

    render_backend->SetRenderLayer(kRenderLayerHUD);

    if (!plm_has_ended(decoder))
    {
        MovieDrawFrame(vx1, vy1, vx2, vy2, luma_scale_x, luma_scale_y, chroma_scale_x, chroma_scale_y);

        StartUnitBatch(false);

        RGBAColor       unit_col = kRGBAWhite;
        RendererVertex *glvert   = nullptr;

        // Fade-in
        fadein = (double)GetMilliseconds() / 1000.0 - movie_start_time;
        if (fadein <= 0.25f)
        {
            unit_col = epi::MakeRGBAFloat(0.0f, 0.0f, 0.0f, ((0.25f - (float)fadein) / 0.25f));

            glvert =
                BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, 0, (GLuint)kTextureEnvironmentDisable, 0, 0, kBlendingAlpha);

            glvert->rgba       = unit_col;
            glvert++->position = {{vx1, vy2, 0}};
            glvert->rgba       = unit_col;
            glvert++->position = {{vx2, vy2, 0}};
            glvert->rgba       = unit_col;
            glvert++->position = {{vx2, vy1, 0}};
            glvert->rgba       = unit_col;
            glvert->position   = {{vx1, vy1, 0}};

            EndRenderUnit(4);
        }

        FinishUnitBatch();

        if (skip_bar_active)
        {
            // Draw black box at bottom of screen
            HUDSolidBox(hud_x_left, 196, hud_x_right, 200, kRGBABlack);

            // Draw progress
            HUDSolidBox(hud_x_left, 197, hud_x_right * (skip_time / 0.9f), 199, kRGBAWhite);
        }
    }
    else
    {
        double current_time = (double)GetMilliseconds() / 1000.0;
        fadeout             = current_time - last_time;

        if (fadeout <= 0.25f)
            MovieDrawFrame(vx1, vy1, vx2, vy2, luma_scale_x, luma_scale_y, chroma_scale_x, chroma_scale_y);

        StartUnitBatch(false);

        RGBAColor unit_col;

        if (fadeout > 0.25f)
            unit_col = kRGBABlack;
        else
            unit_col = epi::MakeRGBAFloat(0.0f, 0.0f, 0.0f, (float)fadeout / 0.25f);

        RendererVertex *glvert =
            BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, 0, (GLuint)kTextureEnvironmentDisable, 0, 0,
                            fadeout > 0.25f ? kBlendingNone : kBlendingAlpha);

        glvert->rgba       = unit_col;
        glvert++->position = {{vx1, vy2, 0}};
        glvert->rgba       = unit_col;
        glvert++->position = {{vx2, vy2, 0}};
        glvert->rgba       = unit_col;
        glvert++->position = {{vx2, vy1, 0}};
        glvert->rgba       = unit_col;
        glvert->position   = {{vx1, vy1, 0}};

        EndRenderUnit(4);

        FinishUnitBatch();
    }

    canvas_can_update = true;
}

bool MovieResponder(InputEvent *ev)
{
    if (playing_movie)
    {
        switch (ev->type)
        {
        case kInputEventKeyDown:
            skip_bar_active = true;
            break;

        case kInputEventKeyUp:
            skip_bar_active = false;
            skip_time       = 0;
            break;

        default:
            break;
        }
        return true; // eat it no matter what
    }
    else
        return false;
}

void MovieTicker()
{
    if (!playing_movie)
    {
        if (decoder)
            EndMovie();
        return;
    }
    if (fadeout > 0.25f)
    {
        playing_movie = false;
        EndMovie();
        return;
    }
    if (!plm_has_ended(decoder))
    {
        double current_time = (double)GetMilliseconds() / 1000.0;
        elapsed_time        = current_time - last_time;
        if (elapsed_time > 1.0 / 30.0)
            elapsed_time = 1.0 / 30.0;
        last_time = current_time;

        plm_decode(decoder, elapsed_time);

        if (skip_bar_active)
        {
            skip_time += elapsed_time;
            if (skip_time > 1)
                playing_movie = false;
        }
    }
}