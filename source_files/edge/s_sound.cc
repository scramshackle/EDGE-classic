//----------------------------------------------------------------------------
//  EDGE Sound System
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

#ifdef EDGE_WEB
#include <emscripten.h>
#endif

#include "dm_state.h"
#include "epi.h"
#include "i_movie.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_random.h"
#include "p_local.h" // ApproximateDistance
#include "s_blit.h"
#include "s_cache.h"
#include "s_music.h"
#include "s_sound.h"
#include "w_wad.h"

extern void StartupProgressMessage(const char *message);

static bool allow_hogs = true;

extern float listen_x;
extern float listen_y;
extern float listen_z;

static constexpr uint8_t category_limit_table[kTotalCategories] = {

    /* 32 channel */
    2,  // UI
    2,  // Player
    3,  // Weapon

    3,  // Opponent
    12, // Monster
    6,  // Object
    4,  // Level

    // NOTE: never put a '0' on the WEAPON line, since the top
    // four categories should never be merged with the rest.
};

static int category_limits[kTotalCategories];
static int category_counts[kTotalCategories];

static void SetupCategoryLimits(void)
{
    int multiply = 1;
    if (total_channels >= 64)
        multiply = total_channels / 32;

    for (int t = 0; t < kTotalCategories; t++)
    {
        category_limits[t] = category_limit_table[t] * multiply;
        category_counts[t] = 0;
    }
}

static int FindFreeChannel(void)
{
    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelFinished)
            KillSoundChannel(i);

        if (chan->state_ == kChannelEmpty)
            return i;
    }

    return -1; // not found
}

static int FindPlayingFX(const SoundEffectDefinition *def, int cat, const Position *pos)
{
    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelPlaying && chan->category_ == cat && chan->position_ == pos)
        {
            if (chan->definition_ == def)
                return i;

            if (chan->definition_->singularity_ > 0 && chan->definition_->singularity_ == def->singularity_)
                return i;
        }
    }

    return -1; // not found
}

static int FindBiggestHog(int real_cat)
{
    int biggest_hog   = -1;
    int biggest_extra = 0;

    for (int hog = 0; hog < kTotalCategories; hog++)
    {
        if (hog == real_cat)
            continue;

        int extra = category_counts[hog] - category_limits[hog];

        if (extra <= 0)
            continue;

        // found a hog!
        if (biggest_hog < 0 || extra > biggest_extra)
        {
            biggest_hog   = hog;
            biggest_extra = extra;
        }
    }

    EPI_ASSERT(biggest_hog >= 0);

    return biggest_hog;
}

static void CountPlayingCats(void)
{
    for (int c = 0; c < kTotalCategories; c++)
        category_counts[c] = 0;

    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelPlaying)
            category_counts[chan->category_] += 1;
    }
}

static int ChannelScore(const SoundEffectDefinition *def, int category, const Position *pos, bool boss)
{
    // for full-volume sounds, use the priority from DDF
    if (category <= kCategoryWeapon)
    {
        return 200 - def->priority_;
    }

    // for stuff in the level, use the distance
    EPI_ASSERT(pos);

    float dist = boss ? 0 : ApproximateDistance(listen_x - pos->x, listen_y - pos->y, listen_z - pos->z);

    int base_score = 999 - (int)(dist / 10.0);

    return base_score * 100 - def->priority_;
}

static int FindChannelToKill(int kill_cat, int real_cat, int new_score)
{
    int kill_idx   = -1;
    int kill_score = (1 << 30);

    // LogPrint("FindChannelToKill: cat:%d new_score:%d\n", kill_cat,
    // new_score);
    for (int j = 0; j < total_channels; j++)
    {
        const SoundChannel *chan = mix_channels[j];

        if (chan->state_ != kChannelPlaying)
            continue;

        if (chan->category_ != kill_cat)
            continue;

        int score = ChannelScore(chan->definition_, chan->category_, chan->position_, chan->boss_);

        if (score < kill_score)
        {
            kill_idx   = j;
            kill_score = score;
        }
    }
    // LogPrint("kill_idx = %d\n", kill_idx);
    EPI_ASSERT(kill_idx >= 0);

    if (kill_cat != real_cat)
        return kill_idx;

    // if the score for new sound is worse than any existing
    // channel, then simply discard the new sound.
    if (new_score >= kill_score)
        return kill_idx;

    return -1;
}

void InitializeSound(void)
{
    if (no_sound)
        return;

    StartupProgressMessage("Initializing sound device...");

    LogPrint("StartupSound: Init %d mixing channels\n", kMaximumSoundChannels);

    // setup channels
    InitializeSoundChannels(kMaximumSoundChannels);

    SetupCategoryLimits();
}

void ShutdownSound(void)
{
    if (no_sound)
        return;

    FreeSoundChannels();

    SoundCacheClearAll();
}

// Not-rejigged-yet stuff..
SoundEffectDefinition *LookupEffectDef(const SoundEffect *s)
{
    EPI_ASSERT(s->num >= 1);

    int num;

    if (s->num > 1)
        num = s->sounds[RandomByte() % s->num];
    else
        num = s->sounds[0];

    EPI_ASSERT(0 <= num && (size_t)num < sfxdefs.size());

    return sfxdefs[num];
}

static int SelectSoundBus(int category)
{
    if (pc_speaker_mode)
        return kSoundBusDry;

    if (category == kCategoryUi)
        return kSoundBusDry;

    if (vacuum_sound_effects)
        return kSoundBusVacuum;

    if (submerged_sound_effects)
        return kSoundBusUnderwater;

    if (sector_reverb || dynamic_reverb.d_)
        return kSoundBusReverb;

    return kSoundBusDry;
}

static void S_PlaySound(int idx, const SoundEffectDefinition *def, int category, const Position *pos, int flags,
                        SoundData *buf)
{
    SoundChannel *chan = mix_channels[idx];

    chan->state_ = kChannelPlaying;
    chan->data_  = buf;

    chan->definition_ = def;
    chan->position_   = pos;
    chan->category_   = category;

    chan->boss_ = (flags & kSoundEffectBoss) ? true : false;

    chan->cursor_  = 0;
    chan->looping_ = def->looping_;
    chan->volume_  = chan->boss_ ? 1.0f : def->volume_;

    bool attenuate =
        (!chan->boss_ && pos && category != kCategoryWeapon && category != kCategoryPlayer && category != kCategoryUi);

    chan->attenuate_ = attenuate;

    if (attenuate)
    {
        chan->bus_ = SelectSoundBus(category);

        // Lobo 2026: possible to get here before we actually have a player mobj so make sure
        if (players[display_player]->map_object_ &&
            CheckSightToPoint(players[display_player]->map_object_, pos->x, pos->y, pos->z))
            chan->minimum_distance_ = kMinimumSoundClipDistance;
        else
            chan->minimum_distance_ = kMinimumOccludedSoundClipDistance;

        HMM_Vec3 emitter = {{pos->x, pos->z, -pos->y}};

        chan->spatializer_.Update(spatial_listener, emitter, chan->minimum_distance_, kMaximumSoundClipDistance,
                                  chan->volume_);
    }
    else
    {
        chan->bus_ = SelectSoundBus(category);
        chan->spatializer_.SetUniformGain(chan->volume_);
    }
}

static void DoStartFX(const SoundEffectDefinition *def, int category, const Position *pos, int flags, SoundData *buf)
{
    LockSoundMixer();

    CountPlayingCats();

    int k = FindPlayingFX(def, category, pos);

    if (k >= 0)
    {
        SoundChannel *chan = mix_channels[k];

        if (def->looping_ && def == chan->definition_)
        {
            chan->looping_ = true;
            UnlockSoundMixer();
            return;
        }
        else if (flags & kSoundEffectSingle)
        {
            if (chan->definition_->precious_)
            {
                UnlockSoundMixer();
                return;
            }

            KillSoundChannel(k);
            S_PlaySound(k, def, category, pos, flags, buf);
            UnlockSoundMixer();
            return;
        }
    }

    k = FindFreeChannel();

    if (!allow_hogs)
    {
        if (category_counts[category] >= category_limits[category])
            k = -1;
    }

    if (k < 0)
    {
        // all channels are in use.
        // either kill one, or drop the new sound.

        int new_score = ChannelScore(def, category, pos, (flags & kSoundEffectBoss) ? true : false);

        // decide which category to kill a sound in.
        int kill_cat = category;

        if (category_counts[category] < category_limits[category])
        {
            // we haven't reached our quota yet, hence kill a hog.
            kill_cat = FindBiggestHog(category);
        }

        EPI_ASSERT(category_counts[kill_cat] >= category_limits[kill_cat]);

        k = FindChannelToKill(kill_cat, category, new_score);

        if (k < 0)
        {
            UnlockSoundMixer();
            return;
        }

        KillSoundChannel(k);
    }

    S_PlaySound(k, def, category, pos, flags, buf);

    UnlockSoundMixer();
}

void StartSoundEffect(const SoundEffect *sfx, int category, const Position *pos, int flags)
{
    if (no_sound || !sfx)
        return;

    if (fast_forward_active)
        return;

    EPI_ASSERT(0 <= category && category < kTotalCategories);

    if (category >= kCategoryOpponent && !pos)
        FatalError("StartSoundEffect: position missing for category: %d\n", category);

    SoundEffectDefinition *def = LookupEffectDef(sfx);
    EPI_ASSERT(def);

    // ignore very far away sounds
    if (category >= kCategoryOpponent && !(flags & kSoundEffectBoss))
    {
        float dist = ApproximateDistance(listen_x - pos->x, listen_y - pos->y, listen_z - pos->z);

        if (dist > def->max_distance_)
            return;
    }

    if (def->singularity_ > 0)
    {
        flags |= kSoundEffectSingle;
        flags |= (def->precious_ ? kSoundEffectPrecious : 0);
    }

    while (category_limits[category] == 0)
        category++;

    SoundData *buf = SoundCacheLoad(def);
    if (!buf)
        return;

    DoStartFX(def, category, pos, flags, buf);
}

void StopSoundEffect(const Position *pos)
{
    if (no_sound)
        return;

    LockSoundMixer();

    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelPlaying && chan->position_ == pos)
        {
            // LogPrint("StopSoundEffect: killing #%d\n", i);
            KillSoundChannel(i);
        }
    }

    UnlockSoundMixer();
}

void StopSoundEffect(const SoundEffect *sfx)
{
    if (no_sound)
        return;

    SoundEffectDefinition *def = LookupEffectDef(sfx);
    EPI_ASSERT(def);

    LockSoundMixer();

    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ == kChannelPlaying && chan->definition_ == def)
        {
            KillSoundChannel(i);
        }
    }

    UnlockSoundMixer();
}

void StopAllSoundEffects(void)
{
    if (no_sound)
        return;

    LockSoundMixer();

    for (int i = 0; i < total_channels; i++)
    {
        const SoundChannel *chan = mix_channels[i];

        if (chan->state_ != kChannelEmpty)
        {
            KillSoundChannel(i);
        }
    }

    UnlockSoundMixer();
}

void SoundTicker(void)
{
    if (no_sound)
        return;

    if (!playing_movie)
        ApplyPendingSoundDeviceFormatChange();

    if (playing_movie)
        return;

    if (game_state == kGameStateLevel)
    {
        EPI_ASSERT(::total_players > 0);

        MapObject *pmo = ::players[display_player]->map_object_;
        EPI_ASSERT(pmo);

        UpdateSounds(pmo, pmo->angle_);
    }
    else
    {
        UpdateSounds(nullptr, 0);
    }
}

void PrecacheSounds(void)
{
    StartupProgressMessage("Precaching SFX...");
    for (size_t i = 0; i < sfxdefs.size(); i++)
    {
        SoundCacheLoad(sfxdefs[i]);
    }
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
