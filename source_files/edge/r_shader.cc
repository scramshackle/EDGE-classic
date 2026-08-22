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

    HMM_Vec3 surface_normal_;
    HMM_Vec3 surface_tangent_u_;
    HMM_Vec3 surface_tangent_v_;

  public:
    dynlight_shader_c(MapObject *object, float r)
        : mo(object), radius(r), surface_normal_({{0.0f, 0.0f, 1.0f}}), surface_tangent_u_({{1.0f, 0.0f, 0.0f}}),
          surface_tangent_v_({{0.0f, 1.0f, 0.0f}})
    {
    }

    ~dynlight_shader_c()
    { /* nothing to do */
    }

  private:
    inline void PrepareNormal(const HMM_Vec3 *normal)
    {
        float n_len = sqrt(normal->X * normal->X + normal->Y * normal->Y + normal->Z * normal->Z);

        if (n_len < 0.0001f)
            surface_normal_ = {{0.0f, 0.0f, 1.0f}};
        else
            surface_normal_ = {{normal->X / n_len, normal->Y / n_len, normal->Z / n_len}};

        HMM_Vec3 guide = {{0.0f, 0.0f, 1.0f}};

        if (fabs(surface_normal_.Z) >= fabs(surface_normal_.X) && fabs(surface_normal_.Z) >= fabs(surface_normal_.Y))
            guide = {{1.0f, 0.0f, 0.0f}};

        surface_tangent_u_ = HMM_NormV3(HMM_Cross(guide, surface_normal_));
        surface_tangent_v_ = HMM_Cross(surface_normal_, surface_tangent_u_);
    }

    inline float TexCoord(HMM_Vec2 *texc, float r, const HMM_Vec3 *lit_pos)
    {
        HMM_Vec3 delta = {{lit_pos->X - mo->x, lit_pos->Y - mo->y, lit_pos->Z - MapObjectMidZ(mo)}};

        texc->X = 0.5f + HMM_DotV3(delta, surface_tangent_u_) / r * 0.5f;
        texc->Y = 0.5f + HMM_DotV3(delta, surface_tangent_v_) / r * 0.5f;

        return fabs(HMM_DotV3(delta, surface_normal_)) / r;
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

    void Corner(ColorMixer *col, float nx, float ny, float nz, MapObject *mod_pos, bool is_weapon)
    {
        float mx = mo->x;
        float my = mo->y;
        float mz = MapObjectMidZ(mo);

        if (is_weapon)
        {
            mx += view_cosine * 24;
            my += view_sine * 24;
        }

        float dx = mod_pos->x;
        float dy = mod_pos->y;
        float dz = MapObjectMidZ(mod_pos);

        dx -= mx;
        dy -= my;
        dz -= mz;

        float dist = sqrt(dx * dx + dy * dy + dz * dz);

        dx /= dist;
        dy /= dist;
        dz /= dist;

        dist = HMM_MAX(1.0, dist - mod_pos->radius_);

        float L = 0.6 - 0.7 * (dx * nx + dy * ny + dz * nz);

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

    bool GetLightParameters(DynamicLightParameters *out)
    {
        if (WhatType() == kDynamicLightTypeNone)
            return false;

        float light_x = mo->x;
        float light_y = mo->y;
        float light_z = MapObjectMidZ(mo);

        out->position = {{light_x, light_y, light_z}};
        out->radius   = WhatRadius();

        RGBAColor col = WhatColor();

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        out->color = {{L * epi::GetRGBARed(col), L * epi::GetRGBAGreen(col), L * epi::GetRGBABlue(col)}};

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

    inline void TexCoord(HMM_Vec2 *texc, float r, const Sector *sec, const HMM_Vec3 *lit_pos, const HMM_Vec3 *normal)
    {
        EPI_UNUSED(normal);
        texc->X = 0.5;
        texc->Y = 0.5 + Dist(sec, lit_pos->Z) / r / 2.0;
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
        const Sector *sec = mo->subsector_->sector;

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

    void Corner(ColorMixer *col, float nx, float ny, float nz, MapObject *mod_pos, bool is_weapon)
    {
        EPI_UNUSED(nx);
        EPI_UNUSED(ny);
        const Sector *sec = mo->subsector_->sector;

        float dz = (mo->info_->glow_type_ == kSectorGlowTypeFloor) ? +1 : -1;
        float dist;

        if (is_weapon)
        {
            float weapon_z = mod_pos->z + mod_pos->height_ * mod_pos->info_->shotheight_;

            if (mo->info_->glow_type_ == kSectorGlowTypeFloor)
                dist = weapon_z - sec->floor_height;
            else
                dist = sec->ceiling_height - weapon_z;
        }
        else if (mo->info_->glow_type_ == kSectorGlowTypeFloor)
            dist = mod_pos->z - sec->floor_height;
        else
            dist = sec->ceiling_height - (mod_pos->z + mod_pos->height_);

        dist = HMM_MAX(1.0, fabs(dist));

        float L = 0.6 - 0.7 * (dz * nz);

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
        return (ld->vertex_1->X - x) * norm_x + (ld->vertex_1->Y - y) * norm_y;
    }

    inline void TexCoord(HMM_Vec2 *texc, float r, const Sector *sec, const HMM_Vec3 *lit_pos, const HMM_Vec3 *normal)
    {
        EPI_UNUSED(sec);
        EPI_UNUSED(normal);
        texc->X = 0.5;
        texc->Y = 0.5 + Dist(lit_pos->X, lit_pos->Y) / r / 2.0;
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
        norm_x = (ld->vertex_1->Y - ld->vertex_2->Y) / ld->length;
        norm_y = (ld->vertex_2->X - ld->vertex_1->X) / ld->length;
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

    void Corner(ColorMixer *col, float nx, float ny, float nz, MapObject *mod_pos, bool is_weapon = false)
    {
        EPI_UNUSED(nx);
        EPI_UNUSED(ny);
        EPI_UNUSED(nz);
        EPI_UNUSED(is_weapon);
        float dist = Dist(mod_pos->x, mod_pos->y);

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
