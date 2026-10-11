//----------------------------------------------------------------------------
//  EDGE Lighting Shaders
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

#include "r_shader.h"

#include <float.h>

#include "con_var.h"
#include "ddf_main.h"
#include "epi.h"
#include "i_defs_gl.h"
#include "p_mobj.h"
#include "r_backend.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_state.h"
#include "r_units.h"

//----------------------------------------------------------------------------
//  LIGHT FALLOFF
//----------------------------------------------------------------------------

static constexpr float kLightFalloffCurve = -5.44f;

static inline float LightFalloffIntensity(float d)
{
    return exp(kLightFalloffCurve * d * d);
}

static RGBAColor LightCurvePoint(float d, RGBAColor tint)
{
    float intensity = LightFalloffIntensity(d);

    int r = (int)(epi::GetRGBARed(tint) * intensity);
    int g = (int)(epi::GetRGBAGreen(tint) * intensity);
    int b = (int)(epi::GetRGBABlue(tint) * intensity);

    return epi::MakeRGBA((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

//----------------------------------------------------------------------------
//  DYNAMIC LIGHTS
//----------------------------------------------------------------------------

class dynlight_shader_c : public AbstractShader
{
  private:
    MapObject *mo;

    float radius;

  public:
    dynlight_shader_c(MapObject *object, float r) : mo(object), radius(r)
    {
    }

    ~dynlight_shader_c()
    { /* nothing to do */
    }

  private:
    inline float WhatRadius()
    {
        return radius;
    }

    inline RGBAColor WhatColor()
    {
        return mo->dynamic_light_.color;
    }

    inline DynamicLightType WhatType()
    {
        return mo->info_->dlight_.type_;
    }

  public:
    void Sample(ColorMixer *col, float x, float y, float z)
    {
        float mx = mo->x;
        float my = mo->y;
        float mz = MapObjectMidZ(mo);

        float dx = x - mx;
        float dy = y - my;
        float dz = z - mz;

        float dist = sqrt(dx * dx + dy * dy + dz * dz);

        if (WhatType() == kDynamicLightTypeNone)
            return;

        RGBAColor new_col = LightCurvePoint(dist / WhatRadius(), WhatColor());

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }


    void SetRadius(float r)
    {
        radius = r;
    }

    bool GetLightParameters(DynamicLightParameters *out)
    {
        if (WhatType() == kDynamicLightTypeNone)
            return false;

        float light_x = mo->x;
        float light_y = mo->y;
        float light_z = MapObjectMidZ(mo);

        out->position = {light_x, light_y, light_z};
        out->radius   = WhatRadius();

        RGBAColor col = WhatColor();

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        out->color = {L * epi::GetRGBARed(col), L * epi::GetRGBAGreen(col), L * epi::GetRGBABlue(col)};

        out->additive = (WhatType() == kDynamicLightTypeAdd);

        return true;
    }
};

AbstractShader *MakeDLightShader(MapObject *mo, float r)
{
    return new dynlight_shader_c(mo, r);
}

//----------------------------------------------------------------------------
//  SECTOR GLOWS
//----------------------------------------------------------------------------

class plane_glow_c : public AbstractShader
{
  private:
    MapObject *mo;

    float radius;

  public:
    plane_glow_c(MapObject *_glower, float r) : mo(_glower), radius(r)
    {
    }

    ~plane_glow_c()
    { /* nothing to do */
    }

  private:
    inline float Dist(const Sector *sec, float z)
    {
        if (mo->info_->glow_type_ == kSectorGlowTypeFloor)
            return fabs(sec->floor_height - z);
        else
            return fabs(sec->ceiling_height - z); // kSectorGlowTypeCeiling
    }

    inline float WhatRadius()
    {
        return radius;
    }

    inline RGBAColor WhatColor()
    {
        return mo->dynamic_light_.color;
    }

    inline DynamicLightType WhatType()
    {
        return mo->info_->dlight_.type_;
    }

  public:
    void Sample(ColorMixer *col, float x, float y, float z)
    {
        EPI_UNUSED(x);
        EPI_UNUSED(y);
        const Sector *sec = mo->sector_;

        float dist = Dist(sec, z);

        if (WhatType() == kDynamicLightTypeNone)
            return;

        RGBAColor new_col = LightCurvePoint(dist / WhatRadius(), WhatColor());

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }


    void SetRadius(float r)
    {
        radius = r;
    }
};

AbstractShader *MakePlaneGlow(MapObject *mo, float r)
{
    return new plane_glow_c(mo, r);
}

//----------------------------------------------------------------------------
//  WALL GLOWS
//----------------------------------------------------------------------------

class wall_glow_c : public AbstractShader
{
  private:
    Line      *ld;
    MapObject *mo;

    float norm_x, norm_y; // normal

    float radius;

    inline float Dist(float x, float y)
    {
        return (ld->vertex_1->x - x) * norm_x + (ld->vertex_1->y - y) * norm_y;
    }

    inline float WhatRadius()
    {
        return radius;
    }

    inline RGBAColor WhatColor()
    {
        return mo->dynamic_light_.color;
    }

    inline DynamicLightType WhatType()
    {
        return mo->info_->dlight_.type_;
    }

  public:
    wall_glow_c(MapObject *_glower, float r) : mo(_glower), radius(r)
    {
        EPI_ASSERT(mo->dynamic_light_.glow_wall);
        ld     = mo->dynamic_light_.glow_wall;
        norm_x = (ld->vertex_1->y - ld->vertex_2->y) / ld->length;
        norm_y = (ld->vertex_2->x - ld->vertex_1->x) / ld->length;
    }

    ~wall_glow_c()
    { /* nothing to do */
    }

    void Sample(ColorMixer *col, float x, float y, float z)
    {
        EPI_UNUSED(z);
        float dist = Dist(x, y);

        float L = std::log1p(dist);

        L *= (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        if (WhatType() == kDynamicLightTypeNone)
            return;

        RGBAColor new_col = LightCurvePoint(dist / WhatRadius(), WhatColor());

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }


    void SetRadius(float r)
    {
        radius = r;
    }
};

AbstractShader *MakeWallGlow(MapObject *mo, float r)
{
    return new wall_glow_c(mo, r);
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
