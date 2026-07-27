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
//
// Reverb Setup and Parser Code
//

#include "ddf_reverb.h"

#include "ddf_local.h"

namespace ddf
{

static ReverbDefinition *dynamic_reverb = nullptr;

//
//  DDF PARSE ROUTINES
//

void ReverbDefinition::StartEntry(const char *name, bool extend)
{
    if (!name)
        DDFError("New REVERB entry is missing a name!\n");

    dynamic_reverb = Lookup(name);

    if (extend)
    {
        if (!dynamic_reverb)
            DDFError("Unknown REVERB to extend: %s\n", name);
        return;
    }

    // replaces an existing entry?
    if (dynamic_reverb)
    {
        dynamic_reverb->Default();
        return;
    }

    // not found, create a new one
    dynamic_reverb = new ReverbDefinition;

    StoreReverb(DDFCreateStringHash(name), dynamic_reverb);
}

void ReverbDefinition::ParseField(const char *field, const char *contents, int index, bool is_last)
{
#if (DDF_DEBUG)
    LogDebug("REVERB_PARSE: %s = %s;\n", field, contents);
#endif
    EPI_UNUSED(index);
    EPI_UNUSED(is_last);
    float *member  = nullptr;
    bool   is_time = false;
    switch (DDFCreateStringHash(field).Value())
    {
    case kDecayTime:
        member  = &dynamic_reverb->parameters_[kReverbDecayTime];
        is_time = true;
        break;
    case kRoomSize:
        member = &dynamic_reverb->parameters_[kReverbRoomSize];
        break;
    case kPreDelay:
        member  = &dynamic_reverb->parameters_[kReverbPreDelay];
        is_time = true;
        break;
    case kDiffusion:
        member = &dynamic_reverb->parameters_[kReverbDiffusion];
        break;
    case kHighFrequencyDamping:
        member = &dynamic_reverb->parameters_[kReverbHighFrequencyDamping];
        break;
    case kLowFrequencyDamping:
        member = &dynamic_reverb->parameters_[kReverbLowFrequencyDamping];
        break;
    case kWetLevel:
        member = &dynamic_reverb->parameters_[kReverbWetLevel];
        break;
    case kDryLevel:
        member = &dynamic_reverb->parameters_[kReverbDryLevel];
        break;
    case kWidthKey:
        member = &dynamic_reverb->parameters_[kReverbWidth];
        break;
    default:
        DDFError("Unknown reverbs.ddf command: %s\n", field);
    }
    EPI_ASSERT(member);
    if (is_time)
        DDFMainGetFloat(contents, member);
    else
        DDFMainGetPercent(contents, member);
}

void ReverbDefinition::FinishEntry(void)
{
    if (dynamic_reverb->parameters_[kReverbDecayTime] > 20.0f)
        dynamic_reverb->parameters_[kReverbDecayTime] = 20.0f;

    if (dynamic_reverb->parameters_[kReverbPreDelay] > 0.2f)
        dynamic_reverb->parameters_[kReverbPreDelay] = 0.2f;
}

void ReverbDefinition::ClearEntries(void)
{
    LogWarning("Ignoring #CLEARALL in reverbs.ddf\n");
}

void ReverbDefinition::ReadDDF(const std::string &data)
{
    DDFReadInfo reverbs;

    reverbs.tag      = "REVERBS";
    reverbs.lumpname = "DDFVERB";

    reverbs.start_entry  = StartEntry;
    reverbs.parse_field  = ParseField;
    reverbs.finish_entry = FinishEntry;
    reverbs.clear_all    = ClearEntries;

    DDFMainReadFile(&reverbs, data);
}

// ---> ReverbDefinition class

ReverbDefinition::ReverbDefinition()
{
    Default();
}

ReverbDefinition::ReverbDefinition(float decay, float size, float pre_delay, float diffusion, float high_damping,
                                   float low_damping, float wet, float dry, float width)
    : parameters_{decay, size, pre_delay, diffusion, high_damping, low_damping, wet, dry, width}
{
}

//
// Copies all the detail with the exception of ddf info
//
void ReverbDefinition::CopyDetail(const ReverbDefinition &src)
{
    for (int i = 0; i < kTotalReverbParameters; i++)
        parameters_[i] = src.parameters_[i];
}

void ReverbDefinition::ApplyReverb(ReverbEffect *reverb) const
{
    reverb->SetParameters(parameters_);
}

void ReverbDefinition::Default()
{
    parameters_[kReverbDecayTime]            = 0.8f;
    parameters_[kReverbRoomSize]             = 0.4f;
    parameters_[kReverbPreDelay]             = 0.0f;
    parameters_[kReverbDiffusion]            = 0.7f;
    parameters_[kReverbHighFrequencyDamping] = 0.5f;
    parameters_[kReverbLowFrequencyDamping]  = 0.0f;
    parameters_[kReverbWetLevel]             = 0.3f;
    parameters_[kReverbDryLevel]             = 1.0f;
    parameters_[kReverbWidth]                = 1.0f;
}

const ReverbDefinition ReverbDefinition::kOutdoorStrong(2.20f, 0.85f, 0.030f, 0.85f, 0.30f, 0.10f, 0.25f, 1.0f, 0.60f);
const ReverbDefinition ReverbDefinition::kIndoorStrong(1.30f, 0.55f, 0.012f, 0.75f, 0.55f, 0.05f, 0.35f, 1.0f, 1.00f);
const ReverbDefinition ReverbDefinition::kOutdoorWeak(1.60f, 0.80f, 0.025f, 0.85f, 0.40f, 0.10f, 0.15f, 1.0f, 0.55f);
const ReverbDefinition ReverbDefinition::kIndoorWeak(0.90f, 0.50f, 0.010f, 0.75f, 0.60f, 0.05f, 0.20f, 1.0f, 0.90f);

// ---> Container class

ReverbDefinition::Container ReverbDefinition::reverb_defs_;

} // namespace ddf

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab