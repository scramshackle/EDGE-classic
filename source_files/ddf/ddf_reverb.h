//----------------------------------------------------------------------------
//  EDGE Data Definition File Code (Reverbs)
//----------------------------------------------------------------------------
//
//  Copyright (c) 2025 The EDGE Team.
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

#pragma once

#include <unordered_map>

#include "ddf_types.h"
#include "epi.h"
#include "epi_str_hash.h"
#include "s_effect.h"

extern epi::StringHash DDFCreateStringHash(std::string_view name);
#ifdef __GNUC__
[[noreturn]] extern void DDFError(const char *err, ...) __attribute__((format(printf, 1, 2)));
#else
[[noreturn]] extern void DDFError(const char *err, ...);
#endif

namespace ddf
{
class ReverbDefinition final
{
  public:
    ReverbDefinition();
    ~ReverbDefinition(){};

    void                            Default(void);
    void                            CopyDetail(const ReverbDefinition &src);
    void                            ApplyReverb(ReverbEffect *reverb) const;
    static void                     ReadDDF(const std::string &data);
    static inline ReverbDefinition *Lookup(std::string_view refname)
    {
        if (refname.empty())
            return nullptr;

        epi::StringHash verb_hash = DDFCreateStringHash(refname);

        if (verb_hash == epi::StringHash::kEmpty)
            return nullptr;

        if (reverb_defs_.find(verb_hash) != reverb_defs_.end())
            return reverb_defs_[verb_hash];
        else
            return nullptr;
    }
    // Called when parsing DDF sectors using the REVERB_PRESET special
    static inline void AssignReverb(const char *info, void *storage)
    {
        *(const ReverbDefinition **)storage = Lookup(info);
        if (*(const ReverbDefinition **)storage == nullptr)
            DDFError("AssignReverb: No such reverb preset '%s'\n", info);
    }

    // Presets for Dynamic Reverb
    static const ReverbDefinition kOutdoorStrong;
    static const ReverbDefinition kIndoorStrong;
    static const ReverbDefinition kOutdoorWeak;
    static const ReverbDefinition kIndoorWeak;

  private:
    ReverbDefinition(float decay, float size, float pre_delay, float diffusion, float high_damping,
                     float low_damping, float wet, float dry, float width);
    // disable copy construct and assignment operator
    explicit ReverbDefinition(ReverbDefinition &rhs)
    {
        EPI_UNUSED(rhs);
    }
    ReverbDefinition &operator=(ReverbDefinition &rhs)
    {
        EPI_UNUSED(rhs);
        return *this;
    }

    static inline void StoreReverb(epi::StringHash name, ReverbDefinition *reverb_def)
    {
        reverb_defs_.try_emplace(name, reverb_def);
    }

    // Member vars

    float parameters_[kTotalReverbParameters];

    // Constants and functions for DDF parsing

    EPI_KNOWN_STRINGHASH(kDecayTime, "DECAYTIME")
    EPI_KNOWN_STRINGHASH(kRoomSize, "ROOMSIZE")
    EPI_KNOWN_STRINGHASH(kPreDelay, "PREDELAY")
    EPI_KNOWN_STRINGHASH(kDiffusion, "DIFFUSION")
    EPI_KNOWN_STRINGHASH(kHighFrequencyDamping, "HIGHFREQUENCYDAMPING")
    EPI_KNOWN_STRINGHASH(kLowFrequencyDamping, "LOWFREQUENCYDAMPING")
    EPI_KNOWN_STRINGHASH(kWetLevel, "WETLEVEL")
    EPI_KNOWN_STRINGHASH(kDryLevel, "DRYLEVEL")
    EPI_KNOWN_STRINGHASH(kWidthKey, "REVERBWIDTH")

    static void StartEntry(const char *name, bool extend);
    static void ParseField(const char *field, const char *contents, int index, bool is_last);
    static void FinishEntry();
    static void ClearEntries();

  protected:
    class Container final : public std::unordered_map<epi::StringHash, ReverbDefinition *>
    {
      public:
        Container()
        {
        }
        ~Container()
        {
            for (std::unordered_map<epi::StringHash, ReverbDefinition *>::iterator iter = begin(), iter_end = end();
                 iter != iter_end; iter++)
            {
                ReverbDefinition *verb = iter->second;
                delete verb;
                verb = nullptr;
            }
        }
    };

    static Container reverb_defs_;
};

} // namespace ddf

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
