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

#include <unordered_map>

#include "con_var.h"
#include "ddf_main.h"
#include "epi.h"
#include "epi_str_hash.h"
#include "i_defs_gl.h"
#include "im_data.h"
#include "p_mobj.h"
#include "r_backend.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_state.h"
#include "r_texgl.h"
#include "r_units.h"

//----------------------------------------------------------------------------
//  LIGHT IMAGES
//----------------------------------------------------------------------------

static constexpr uint8_t kLightImageCurveSize = 32;

class LightImage
{
  public:
    std::string name_;

    const Image *image_;

    RGBAColor curve_[kLightImageCurveSize];

  public:
    LightImage(std::string_view name, const Image *img) : name_(name), image_(img)
    {
    }

    ~LightImage()
    {
    }

    inline GLuint TextureId() const
    {
        return ImageCache(image_, false);
    }

    void MakeStandardCurve() // TEMP CRUD
    {
        for (int i = 0; i < kLightImageCurveSize - 1; i++)
        {
            float d = i / (float)(kLightImageCurveSize - 1);

            float sq = exp(-5.44 * d * d);

            int r = (int)(255 * sq);
            int g = (int)(255 * sq);
            int b = (int)(255 * sq);

            curve_[i] = epi::MakeRGBA(r, g, b);
        }

        curve_[kLightImageCurveSize - 1] = kRGBABlack;
    }

    RGBAColor CurvePoint(float d, RGBAColor tint)
    {
        // d is distance away from centre, between 0.0 and 1.0

        d *= (float)kLightImageCurveSize;

        if (d >= kLightImageCurveSize - 1.01)
            return curve_[kLightImageCurveSize - 1];

        // linearly interpolate between curve points

        int p1 = (int)floor(d);
        int dd = (int)(256 * (d - p1));

        int r1 = epi::GetRGBARed(curve_[p1]);
        int g1 = epi::GetRGBAGreen(curve_[p1]);
        int b1 = epi::GetRGBABlue(curve_[p1]);

        int r2 = epi::GetRGBARed(curve_[p1 + 1]);
        int g2 = epi::GetRGBAGreen(curve_[p1 + 1]);
        int b2 = epi::GetRGBABlue(curve_[p1 + 1]);

        r1 = (r1 * (256 - dd) + r2 * dd) >> 8;
        g1 = (g1 * (256 - dd) + g2 * dd) >> 8;
        b1 = (b1 * (256 - dd) + b2 * dd) >> 8;

        r1 = r1 * epi::GetRGBARed(tint) / 255;
        g1 = g1 * epi::GetRGBAGreen(tint) / 255;
        b1 = b1 * epi::GetRGBABlue(tint) / 255;

        return epi::MakeRGBA(r1, g1, b1);
    }
};

static std::unordered_map<epi::StringHash, LightImage *> known_light_images;

static LightImage *GetLightImage(const MapObjectDefinition *info)
{
    // Intentional Const Overrides
    DynamicLightDefinition *D_info = (DynamicLightDefinition *)&info->dlight_;

    if (!D_info->cache_data_)
    {
        const std::string &shape = D_info->shape_;

        EPI_ASSERT(!shape.empty());

        if (known_light_images.count(shape))
        {
            D_info->cache_data_ = known_light_images[shape];
        }
        else
        {
            const Image *image = ImageLookup(shape.c_str(), kImageNamespaceGraphic, kImageLookupNull);

            if (!image)
                FatalError("Missing dynamic light graphic: %s\n", shape.c_str());

            LightImage *lim = new LightImage(shape, image);

            lim->MakeStandardCurve();

            known_light_images.try_emplace(shape, lim);

            D_info->cache_data_ = lim;
        }
    }

    return (LightImage *)D_info->cache_data_;
}

//----------------------------------------------------------------------------
//  LIGHT SCISSORS
//----------------------------------------------------------------------------

static constexpr float kLightScissorMinimumW = 0.0001f;

enum LightScissorResult
{
    kLightScissorFull,
    kLightScissorRect,
    kLightScissorCull
};

struct LightInfluenceBounds
{
    float minimum[3];
    float maximum[3];
};

static LightInfluenceBounds surface_light_bounds;
static bool                 surface_light_bounds_valid = false;

void SetSurfaceLightBounds(float min_x, float min_y, float min_z, float max_x, float max_y, float max_z)
{
    surface_light_bounds.minimum[0] = min_x;
    surface_light_bounds.minimum[1] = min_y;
    surface_light_bounds.minimum[2] = min_z;

    surface_light_bounds.maximum[0] = max_x;
    surface_light_bounds.maximum[1] = max_y;
    surface_light_bounds.maximum[2] = max_z;

    surface_light_bounds_valid = true;
}

void ClearSurfaceLightBounds()
{
    surface_light_bounds_valid = false;
}

static void SetInfluenceBoundsInfinite(LightInfluenceBounds *bounds)
{
    for (int axis = 0; axis < 3; axis++)
    {
        bounds->minimum[axis] = -FLT_MAX;
        bounds->maximum[axis] = FLT_MAX;
    }
}

static void ClipInfluenceToSlab(LightInfluenceBounds *bounds, int axis, float low, float high)
{
    bounds->minimum[axis] = HMM_MAX(bounds->minimum[axis], low);
    bounds->maximum[axis] = HMM_MIN(bounds->maximum[axis], high);
}

static void ClipInfluenceToSurface(LightInfluenceBounds *bounds)
{
    if (!surface_light_bounds_valid)
        return;

    for (int axis = 0; axis < 3; axis++)
        ClipInfluenceToSlab(bounds, axis, surface_light_bounds.minimum[axis], surface_light_bounds.maximum[axis]);
}

static LightScissorResult ComputeScissorFromBounds(const LightInfluenceBounds *bounds, RendererScissor *out)
{
    if (view_window_width <= 0 || view_window_height <= 0)
        return kLightScissorFull;

    for (int axis = 0; axis < 3; axis++)
    {
        if (bounds->minimum[axis] > bounds->maximum[axis])
            return kLightScissorCull;

        if (bounds->minimum[axis] <= -FLT_MAX || bounds->maximum[axis] >= FLT_MAX)
            return kLightScissorFull;
    }

    HMM_Mat4 view_projection = render_backend->WorldViewProjection();

    float minimum_x = 0.0f;
    float minimum_y = 0.0f;
    float maximum_x = 0.0f;
    float maximum_y = 0.0f;

    int in_front = 0;

    for (int corner = 0; corner < 8; corner++)
    {
        HMM_Vec4 world;

        world.X = (corner & 1) ? bounds->maximum[0] : bounds->minimum[0];
        world.Y = (corner & 2) ? bounds->maximum[1] : bounds->minimum[1];
        world.Z = (corner & 4) ? bounds->maximum[2] : bounds->minimum[2];
        world.W = 1.0f;

        HMM_Vec4 clip = HMM_MulM4V4(view_projection, world);

        if (clip.W <= kLightScissorMinimumW)
            continue;

        float screen_x = (float)view_window_x + ((clip.X / clip.W) * 0.5f + 0.5f) * (float)view_window_width;
        float screen_y = (float)view_window_y + ((clip.Y / clip.W) * 0.5f + 0.5f) * (float)view_window_height;

        in_front++;

        if (in_front == 1)
        {
            minimum_x = maximum_x = screen_x;
            minimum_y = maximum_y = screen_y;
        }
        else
        {
            minimum_x = HMM_MIN(minimum_x, screen_x);
            maximum_x = HMM_MAX(maximum_x, screen_x);
            minimum_y = HMM_MIN(minimum_y, screen_y);
            maximum_y = HMM_MAX(maximum_y, screen_y);
        }
    }

    if (in_front == 0)
        return kLightScissorCull;

    if (in_front < 8)
        return kLightScissorFull;

    int32_t x1 = (int32_t)floor(minimum_x) - 1;
    int32_t y1 = (int32_t)floor(minimum_y) - 1;
    int32_t x2 = (int32_t)ceil(maximum_x) + 1;
    int32_t y2 = (int32_t)ceil(maximum_y) + 1;

    int32_t window_x1 = view_window_x;
    int32_t window_y1 = view_window_y;
    int32_t window_x2 = view_window_x + view_window_width;
    int32_t window_y2 = view_window_y + view_window_height;

    x1 = HMM_MAX(x1, window_x1);
    y1 = HMM_MAX(y1, window_y1);
    x2 = HMM_MIN(x2, window_x2);
    y2 = HMM_MIN(y2, window_y2);

    if (x1 >= x2 || y1 >= y2)
        return kLightScissorCull;

    if (x1 <= window_x1 && y1 <= window_y1 && x2 >= window_x2 && y2 >= window_y2)
        return kLightScissorFull;

    out->x      = x1;
    out->y      = y1;
    out->width  = x2 - x1;
    out->height = y2 - y1;

    return kLightScissorRect;
}

LightRectResult GetDynamicLightScreenRect(AbstractShader *shader, RendererScissor *out)
{
    DynamicLightParameters light;

    if (!shader->GetLightParameters(&light))
        return kLightRectFull;

    LightInfluenceBounds bounds;

    SetInfluenceBoundsInfinite(&bounds);

    ClipInfluenceToSlab(&bounds, 0, light.position.X - light.radius, light.position.X + light.radius);
    ClipInfluenceToSlab(&bounds, 1, light.position.Y - light.radius, light.position.Y + light.radius);
    ClipInfluenceToSlab(&bounds, 2, light.position.Z - light.radius, light.position.Z + light.radius);

    ClipInfluenceToSurface(&bounds);

    LightScissorResult result = ComputeScissorFromBounds(&bounds, out);

    if (result == kLightScissorCull)
        return kLightRectCulled;

    if (result == kLightScissorRect)
        return kLightRectBounded;

    return kLightRectFull;
}

static void ComputeSurfaceLightNormal(const HMM_Vec3 *normal, float *out_normal, bool *out_horizontal)
{
    float nx = normal->X;
    float ny = normal->Y;
    float nz = normal->Z;

    if (fabs(nz) > 50 * (fabs(nx) + fabs(ny)))
    {
        *out_horizontal = true;

        out_normal[0] = 0.0f;
        out_normal[1] = 0.0f;
        out_normal[2] = 1.0f;
        out_normal[3] = 1.0f;

        return;
    }

    *out_horizontal = false;

    float length = sqrt(nx * nx + ny * ny + nz * nz);

    out_normal[0] = nx / length;
    out_normal[1] = ny / length;
    out_normal[2] = nz / length;
    out_normal[3] = sqrt(out_normal[0] * out_normal[0] + out_normal[1] * out_normal[1]);
}

bool EmitMultiLightPass(AbstractShader **shaders, int count, GLuint shape, int num_vert, GLuint tex, float alpha,
                        int *pass_var, BlendingMode blending, bool masked, void *data,
                        ShaderCoordinateFunction func)
{
    if (count <= 0 || count > kMaximumLightsPerPass)
        return false;

    DynamicLightParameters lights[kMaximumLightsPerPass];

    for (int i = 0; i < count; i++)
    {
        if (!shaders[i]->GetLightParameters(&lights[i]))
            return false;
    }

    RendererLightPass light_pass;

    light_pass.count = count;

    for (int i = 0; i < count; i++)
    {
        light_pass.position_radius[i * 4 + 0] = lights[i].position.X;
        light_pass.position_radius[i * 4 + 1] = lights[i].position.Y;
        light_pass.position_radius[i * 4 + 2] = lights[i].position.Z;
        light_pass.position_radius[i * 4 + 3] = lights[i].radius;

        light_pass.color[i * 4 + 0] = lights[i].color.X / 255.0f;
        light_pass.color[i * 4 + 1] = lights[i].color.Y / 255.0f;
        light_pass.color[i * 4 + 2] = lights[i].color.Z / 255.0f;
        light_pass.color[i * 4 + 3] = 1.0f;
    }

    for (int i = count; i < kMaximumLightsPerPass; i++)
    {
        for (int e = 0; e < 4; e++)
        {
            light_pass.position_radius[i * 4 + e] = 0.0f;
            light_pass.color[i * 4 + e]           = 0.0f;
        }
    }

    bool is_additive = lights[0].additive;

    light_pass.surface_mode = is_additive ? (masked ? 2.0f : 1.0f) : 0.0f;
    light_pass.alpha        = alpha;
    light_pass.alpha_test   = (blending & kBlendingMasked) ? 0.01f : 0.0f;

    {
        HMM_Vec3  probe_position = {};
        HMM_Vec2  probe_texture  = {};
        HMM_Vec3  probe_normal   = {};
        HMM_Vec3  probe_lit_pos  = {};
        RGBAColor probe_rgba     = kRGBAWhite;

        (*func)(data, 0, &probe_position, &probe_rgba, &probe_texture, &probe_normal, &probe_lit_pos);

        ComputeSurfaceLightNormal(&probe_normal, light_pass.surface_normal, &light_pass.normal_is_horizontal);
    }

    RendererScissor scissor;

    bool have_scissor = false;

    for (int i = 0; i < count; i++)
    {
        RendererScissor light_rect;

        LightRectResult result = GetDynamicLightScreenRect(shaders[i], &light_rect);

        if (result == kLightRectCulled)
            continue;

        if (result == kLightRectFull)
        {
            have_scissor = false;
            break;
        }

        if (!have_scissor)
        {
            scissor      = light_rect;
            have_scissor = true;
        }
        else
        {
            int32_t x1 = HMM_MIN(scissor.x, light_rect.x);
            int32_t y1 = HMM_MIN(scissor.y, light_rect.y);
            int32_t x2 = HMM_MAX(scissor.x + scissor.width, light_rect.x + light_rect.width);
            int32_t y2 = HMM_MAX(scissor.y + scissor.height, light_rect.y + light_rect.height);

            scissor.x      = x1;
            scissor.y      = y1;
            scissor.width  = x2 - x1;
            scissor.height = y2 - y1;
        }
    }

    RendererVertex *glvert =
        BeginRenderUnit(shape, num_vert,
                        (is_additive && masked) ? (GLuint)kTextureEnvironmentSkipRGB
                        : is_additive           ? (GLuint)kTextureEnvironmentDisable
                                                : GL_MODULATE,
                        (is_additive && !masked) ? 0 : tex, GL_MODULATE, lights[0].image_texture, *pass_var, blending,
                        kRGBANoValue, 0.0f, nullptr, have_scissor ? &scissor : nullptr, &light_pass);

    for (int v_idx = 0; v_idx < num_vert; v_idx++)
    {
        RendererVertex *dest = glvert + v_idx;

        HMM_Vec3 lit_pos;
        HMM_Vec3 normal;

        (*func)(data, v_idx, &dest->position, &dest->rgba, &dest->texture_coordinates[0], &normal, &lit_pos);

        dest->rgba                   = kRGBAWhite;
        dest->texture_coordinates[1] = {{0.0f, 0.0f}};
    }

    EndRenderUnit(num_vert);

    (*pass_var) += 1;

    return true;
}

//----------------------------------------------------------------------------
//  DYNAMIC LIGHTS
//----------------------------------------------------------------------------

class dynlight_shader_c : public AbstractShader
{
  private:
    MapObject *mo;

    LightImage *lim;

    float radius;

    bool  normal_is_horizontal_;
    float normal_x_;
    float normal_y_;
    float normal_z_;
    float radius_xy_divisor_;

  public:
    dynlight_shader_c(MapObject *object, float r)
        : mo(object), radius(r), normal_is_horizontal_(false), normal_x_(0.0f), normal_y_(0.0f), normal_z_(1.0f),
          radius_xy_divisor_(1.0f)
    {
        // Note: this is shared, we must not delete it
        lim = GetLightImage(mo->info_);
    }

    ~dynlight_shader_c()
    { /* nothing to do */
    }

  private:
    inline void PrepareNormal(const HMM_Vec3 *normal)
    {
        float nx = normal->X;
        float ny = normal->Y;
        float nz = normal->Z;

        if (fabs(nz) > 50 * (fabs(nx) + fabs(ny)))
        {
            normal_is_horizontal_ = true;
            return;
        }

        normal_is_horizontal_ = false;

        float n_len = sqrt(nx * nx + ny * ny + nz * nz);

        normal_x_ = nx / n_len;
        normal_y_ = ny / n_len;
        normal_z_ = nz / n_len;

        radius_xy_divisor_ = sqrt(normal_x_ * normal_x_ + normal_y_ * normal_y_);
    }

    inline float TexCoord(HMM_Vec2 *texc, float r, const HMM_Vec3 *lit_pos)
    {
        float mx = mo->x;
        float my = mo->y;
        float mz = MapObjectMidZ(mo);

        float dx = lit_pos->X - mx;
        float dy = lit_pos->Y - my;
        float dz = lit_pos->Z - mz;

        if (normal_is_horizontal_)
        {
            texc->X = (1 + dx / r) / 2.0;
            texc->Y = (1 + dy / r) / 2.0;

            return fabs(dz) / r;
        }
        else
        {
            float dxy = normal_x_ * dy - normal_y_ * dx;

            r /= radius_xy_divisor_;

            texc->Y = (1 + dz / r) / 2.0;
            texc->X = (1 + dxy / r) / 2.0;

            return fabs(normal_x_ * dx + normal_y_ * dy + normal_z_ * dz) / r;
        }
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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }

    void WorldMix(GLuint shape, int num_vert, GLuint tex, float alpha, int *pass_var, BlendingMode blending,
                  bool masked, void *data, ShaderCoordinateFunction func)
    {
        if (WhatType() == kDynamicLightTypeNone)
            return;

        bool is_additive = (WhatType() == kDynamicLightTypeAdd);

        RGBAColor col = WhatColor();

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        float R = L * epi::GetRGBARed(col);
        float G = L * epi::GetRGBAGreen(col);
        float B = L * epi::GetRGBABlue(col);

        float light_x = mo->x;
        float light_y = mo->y;
        float light_z = MapObjectMidZ(mo);

        float light_radius = WhatRadius();

        LightInfluenceBounds bounds;

        SetInfluenceBoundsInfinite(&bounds);

        ClipInfluenceToSlab(&bounds, 0, light_x - light_radius, light_x + light_radius);
        ClipInfluenceToSlab(&bounds, 1, light_y - light_radius, light_y + light_radius);
        ClipInfluenceToSlab(&bounds, 2, light_z - light_radius, light_z + light_radius);

        ClipInfluenceToSurface(&bounds);

        RendererScissor scissor;

        LightScissorResult scissor_result = ComputeScissorFromBounds(&bounds, &scissor);

        if (scissor_result == kLightScissorCull)
            return;

        RendererVertex *glvert =
            BeginRenderUnit(shape, num_vert,
                            (is_additive && masked) ? (GLuint)kTextureEnvironmentSkipRGB
                            : is_additive           ? (GLuint)kTextureEnvironmentDisable
                                                    : GL_MODULATE,
                            (is_additive && !masked) ? 0 : tex, GL_MODULATE, lim->TextureId(), *pass_var, blending,
                            *pass_var > 0 ? kRGBANoValue : mo->subsector_->sector->properties.fog_color,
                            mo->subsector_->sector->properties.fog_density, nullptr,
                            (scissor_result == kLightScissorRect) ? &scissor : nullptr);

        for (int v_idx = 0; v_idx < num_vert; v_idx++)
        {
            RendererVertex *dest = glvert + v_idx;

            HMM_Vec3 lit_pos;
            HMM_Vec3 normal;

            (*func)(data, v_idx, &dest->position, &dest->rgba, &dest->texture_coordinates[0], &normal, &lit_pos);

            if (v_idx == 0)
                PrepareNormal(&normal);

            float dist = TexCoord(&dest->texture_coordinates[1], WhatRadius(), &lit_pos);

            float ity = exp(-5.44 * dist * dist);

            dest->rgba =
                epi::MakeRGBA((uint8_t)(R * ity), (uint8_t)(G * ity), (uint8_t)(B * ity), (uint8_t)(alpha * 255.0f));
        }

        EndRenderUnit(num_vert);

        (*pass_var) += 1;
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

        out->image_texture = lim->TextureId();
        out->additive      = (WhatType() == kDynamicLightTypeAdd);

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

    LightImage *lim;

    float radius;

  public:
    plane_glow_c(MapObject *_glower, float r) : mo(_glower), radius(r)
    {
        lim = GetLightImage(mo->info_);
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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }

    void WorldMix(GLuint shape, int num_vert, GLuint tex, float alpha, int *pass_var, BlendingMode blending,
                  bool masked, void *data, ShaderCoordinateFunction func)
    {
        const Sector *sec = mo->subsector_->sector;

        if (WhatType() == kDynamicLightTypeNone)
            return;

        bool is_additive = (WhatType() == kDynamicLightTypeAdd);

        RGBAColor col = WhatColor();

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        float R = L * epi::GetRGBARed(col);
        float G = L * epi::GetRGBAGreen(col);
        float B = L * epi::GetRGBABlue(col);

        float glow_radius = WhatRadius();

        LightInfluenceBounds bounds;

        SetInfluenceBoundsInfinite(&bounds);

        if (mo->info_->glow_type_ == kSectorGlowTypeFloor)
            ClipInfluenceToSlab(&bounds, 2, sec->floor_height, sec->floor_height + glow_radius);
        else
            ClipInfluenceToSlab(&bounds, 2, sec->ceiling_height - glow_radius, sec->ceiling_height);

        ClipInfluenceToSurface(&bounds);

        RendererScissor scissor;

        LightScissorResult scissor_result = ComputeScissorFromBounds(&bounds, &scissor);

        if (scissor_result == kLightScissorCull)
            return;

        RendererVertex *glvert =
            BeginRenderUnit(shape, num_vert,
                            (is_additive && masked) ? (GLuint)kTextureEnvironmentSkipRGB
                            : is_additive           ? (GLuint)kTextureEnvironmentDisable
                                                    : GL_MODULATE,
                            (is_additive && !masked) ? 0 : tex, GL_MODULATE, lim->TextureId(), *pass_var, blending,
                            *pass_var > 0 ? kRGBANoValue : mo->subsector_->sector->properties.fog_color,
                            mo->subsector_->sector->properties.fog_density, nullptr,
                            (scissor_result == kLightScissorRect) ? &scissor : nullptr);

        for (int v_idx = 0; v_idx < num_vert; v_idx++)
        {
            RendererVertex *dest = glvert + v_idx;

            HMM_Vec3 lit_pos;
            HMM_Vec3 normal;

            (*func)(data, v_idx, &dest->position, &dest->rgba, &dest->texture_coordinates[0], &normal, &lit_pos);

            TexCoord(&dest->texture_coordinates[1], WhatRadius(), sec, &lit_pos, &normal);

            dest->rgba = epi::MakeRGBA((uint8_t)R, (uint8_t)G, (uint8_t)B, (uint8_t)(alpha * 255.0f));
        }

        EndRenderUnit(num_vert);

        (*pass_var) += 1;
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

    LightImage *lim;

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
        // Note: this is shared, we must not delete it
        lim = GetLightImage(mo->info_);
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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

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

        RGBAColor new_col = lim->CurvePoint(dist / WhatRadius(), WhatColor());

        if (new_col != kRGBABlack && L > 1 / 256.0)
        {
            if (WhatType() == kDynamicLightTypeAdd)
                col->add_GIVE(new_col, L);
            else
                col->modulate_GIVE(new_col, L);
        }
    }

    void WorldMix(GLuint shape, int num_vert, GLuint tex, float alpha, int *pass_var, BlendingMode blending,
                  bool masked, void *data, ShaderCoordinateFunction func)
    {
        const Sector *sec = mo->subsector_->sector;

        if (WhatType() == kDynamicLightTypeNone)
            return;

        bool is_additive = (WhatType() == kDynamicLightTypeAdd);

        RGBAColor col = WhatColor();

        float L = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0;

        float R = L * epi::GetRGBARed(col);
        float G = L * epi::GetRGBAGreen(col);
        float B = L * epi::GetRGBABlue(col);

        RendererVertex *glvert =
            BeginRenderUnit(shape, num_vert,
                            (is_additive && masked) ? (GLuint)kTextureEnvironmentSkipRGB
                            : is_additive           ? (GLuint)kTextureEnvironmentDisable
                                                    : GL_MODULATE,
                            (is_additive && !masked) ? 0 : tex, GL_MODULATE, lim->TextureId(), *pass_var, blending,
                            *pass_var > 0 ? kRGBANoValue : mo->subsector_->sector->properties.fog_color,
                            mo->subsector_->sector->properties.fog_density);

        for (int v_idx = 0; v_idx < num_vert; v_idx++)
        {
            RendererVertex *dest = glvert + v_idx;

            HMM_Vec3 lit_pos;
            HMM_Vec3 normal;

            (*func)(data, v_idx, &dest->position, &dest->rgba, &dest->texture_coordinates[0], &normal, &lit_pos);

            TexCoord(&dest->texture_coordinates[1], WhatRadius(), sec, &lit_pos, &normal);

            dest->rgba =
                epi::MakeRGBA((uint8_t)(R * render_view_red_multiplier), (uint8_t)(G * render_view_green_multiplier),
                              (uint8_t)(B * render_view_blue_multiplier), (uint8_t)(alpha * 255.0f));
        }

        EndRenderUnit(num_vert);

        (*pass_var) += 1;
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

void DeleteAllLightImages()
{
    for (std::unordered_map<epi::StringHash, LightImage *>::iterator iter     = known_light_images.begin(),
                                                                     iter_end = known_light_images.end();
         iter != iter_end; ++iter)
    {
        delete iter->second;
    }
    known_light_images.clear();
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
