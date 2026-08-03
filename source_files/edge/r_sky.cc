//----------------------------------------------------------------------------
//  EDGE OpenGL Rendering (Skies)
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

#include "r_sky.h"

#include <math.h>

#include "dm_state.h"
#include "epi.h"
#include "edge_profiling.h"
#include "g_game.h" // current_map
#include "i_defs_gl.h"
#include "im_data.h"
#include "m_math.h"
#include "n_network.h"
#include "p_tick.h"
#include "r_colormap.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_sky.h"
#include "r_texgl.h"
#include "r_units.h"
#include "stb_sprintf.h"
#include "w_flat.h"
#include "w_wad.h"

const Image *sky_image;

// Reference for Boom sky transfer, if applicable
MapSurface *sky_ref = nullptr;

bool custom_skybox;

// needed for SKY
extern ImageData *ReadAsEpiBlock(Image *rim);

extern ConsoleVariable draw_culling;

static HMM_Vec2 ddf_sky_scroll     = {{0, 0}};
static HMM_Vec2 ddf_old_sky_scroll = {{0, 0}};
static int      ddf_scroll_tic     = -1;

SkyStretch current_sky_stretch = kSkyStretchUnset;

static constexpr uint8_t kMBFSkyYShift = 28;

static constexpr int kMaximumSkyGroups = 8;

EDGE_DEFINE_CONSOLE_VARIABLE_CLAMPED(sky_stretch_mode, "0", kConsoleVariableFlagArchive, 0, 2);

struct SectorSkyRing
{
    // which group of connected skies (0 if none)
    int group;

    // link of sector in RING
    SectorSkyRing *next;
    SectorSkyRing *previous;

    // maximal sky height of group
    float maximum_height;
};

static std::unordered_map<const Image *, int> sky_image_group_lookup;

static int overflowed_sky_group_count = 0;

static int SkyGroupForImage(const Image *image)
{
    if (!render_state->HasStencilBuffer())
        return 0;

    std::unordered_map<const Image *, int>::iterator it = sky_image_group_lookup.find(image);

    if (it != sky_image_group_lookup.end())
        return it->second;

    if ((int)sky_image_group_lookup.size() >= kMaximumSkyGroups)
    {
        overflowed_sky_group_count++;
        sky_image_group_lookup.emplace(image, kMaximumSkyGroups - 1);
        return kMaximumSkyGroups - 1;
    }

    int group = (int)sky_image_group_lookup.size();

    sky_image_group_lookup.emplace(image, group);

    return group;
}

//
// ComputeSkyHeights
//
// This routine computes the sky height field in sector_t, which is
// the maximal sky height over all sky sectors (ceiling only) which
// are joined by 2S linedefs.
//
// Algorithm: Initially all sky sectors are in individual groups.  Now
// we scan the linedef list.  For each 2-sectored line with sky on
// both sides, merge the two groups into one.  Simple :).  We can
// compute the maximal height of the group as we go.
//
void ComputeSkyHeights(void)
{
    int     i;
    Line   *ld;
    Sector *sec;

    // --- initialise ---

    SectorSkyRing *rings = new SectorSkyRing[total_level_sectors];

    EPI_CLEAR_MEMORY(rings, SectorSkyRing, total_level_sectors);

    for (i = 0, sec = level_sectors; i < total_level_sectors; i++, sec++)
    {
        if (!EDGE_IMAGE_IS_SKY(sec->ceiling))
            continue;

        // leave some room for tall sprites
        static const float SPR_H_MAX = 256.0f;

        rings[i].group = (i + 1);
        rings[i].next = rings[i].previous = rings + i;
        rings[i].maximum_height           = sec->ceiling_height + SPR_H_MAX;
    }

    // --- make the pass over linedefs ---

    for (i = 0, ld = level_lines; i < total_level_lines; i++, ld++)
    {
        const Sector  *sec1, *sec2;
        SectorSkyRing *ring1, *ring2, *tmp_R;

        if (!ld->side[0] || !ld->side[1])
            continue;

        sec1 = ld->front_sector;
        sec2 = ld->back_sector;

        EPI_ASSERT(sec1 && sec2);

        if (sec1 == sec2)
            continue;

        ring1 = rings + (sec1 - level_sectors);
        ring2 = rings + (sec2 - level_sectors);

        // we require sky on both sides
        if (ring1->group == 0 || ring2->group == 0)
            continue;

        // already in the same group ?
        if (ring1->group == ring2->group)
            continue;

        // swap sectors to ensure the lower group is added to the higher
        // group, since we don't need to update the `max_h' fields of the
        // highest group.

        if (ring1->maximum_height < ring2->maximum_height)
        {
            tmp_R = ring1;
            ring1 = ring2;
            ring2 = tmp_R;
        }

        // update the group numbers in the second group

        ring2->group          = ring1->group;
        ring2->maximum_height = ring1->maximum_height;

        for (tmp_R = ring2->next; tmp_R != ring2; tmp_R = tmp_R->next)
        {
            tmp_R->group          = ring1->group;
            tmp_R->maximum_height = ring1->maximum_height;
        }

        // merge 'em baby...

        ring1->next->previous = ring2;
        ring2->next->previous = ring1;

        tmp_R       = ring1->next;
        ring1->next = ring2->next;
        ring2->next = tmp_R;
    }

    // --- now store the results, and free up ---

    sky_image_group_lookup.clear();
    overflowed_sky_group_count = 0;

    for (i = 0, sec = level_sectors; i < total_level_sectors; i++, sec++)
    {
        if (rings[i].group > 0)
        {
            sec->sky_height = rings[i].maximum_height;
            sec->sky_group  = SkyGroupForImage(sec->sky_image);
        }
        else if (EDGE_IMAGE_IS_SKY(sec->floor))
            sec->sky_group = SkyGroupForImage(sec->sky_image);
        else
            sec->sky_group = -1;
    }

    if (overflowed_sky_group_count > 0)
        LogWarning("Level uses more than %d distinct skies; %d had to share a sky group.\n", kMaximumSkyGroups,
                   overflowed_sky_group_count);

    delete[] rings;
}

//----------------------------------------------------------------------------

bool need_to_draw_sky = false;

struct FakeSkybox
{
    const Image *base_sky = nullptr;

    const Colormap *effect_colormap = nullptr;

    int face_size = 1;

    GLuint texture[6] = {0, 0, 0, 0, 0, 0};

    // face images are only present for custom skyboxes.
    // pseudo skyboxes are generated outside of the image system.
    const Image *face[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
};

static std::unordered_map<uint64_t, FakeSkybox> fake_box_cache;

static FakeSkybox *current_fake_box = nullptr;

static uint64_t MakeSkyboxCacheKey(const Image *base_sky, const Colormap *effect_colormap)
{
    return (uint64_t)(uintptr_t)base_sky ^ ((uint64_t)(uintptr_t)effect_colormap * 0x9E3779B97F4A7C15ull);
}

static void DeleteSkyTexGroup(FakeSkybox &box)
{
    for (int i = 0; i < 6; i++)
    {
        if (box.texture[i] != 0)
        {
            render_state->DeleteTexture(&box.texture[i]);
            box.texture[i] = 0;
        }
    }
}

void DeleteSkyTextures(void)
{
    for (std::unordered_map<uint64_t, FakeSkybox>::iterator it = fake_box_cache.begin(); it != fake_box_cache.end();
         ++it)
        DeleteSkyTexGroup(it->second);

    fake_box_cache.clear();

    current_fake_box = nullptr;
}

struct SkyGroupAccumulator
{
    std::vector<RendererVertex> vertices;

    const Image *image = nullptr;
    MapSurface  *ref   = nullptr;

    bool used = false;
};

static SkyGroupAccumulator sky_group_accumulator[kMaximumSkyGroups];

static void PushSkyVertex(int group, const HMM_Vec3 &position)
{
    RendererVertex vertex;

    vertex.rgba     = kRGBAWhite;
    vertex.position = position;

    sky_group_accumulator[group].vertices.push_back(vertex);
}

static int MarkSkyGroup(Sector *sky_owner)
{
    int group = (sky_owner && sky_owner->sky_group >= 0) ? sky_owner->sky_group : 0;

    if (group >= kMaximumSkyGroups)
        group = kMaximumSkyGroups - 1;

    SkyGroupAccumulator &accumulator = sky_group_accumulator[group];

    accumulator.image = (sky_owner && sky_owner->sky_image) ? sky_owner->sky_image : sky_image;
    accumulator.ref   = sky_owner ? sky_owner->sky_ref : nullptr;
    accumulator.used  = true;

    return group;
}

void BeginSky(void)
{
    need_to_draw_sky = false;

    for (int group = 0; group < kMaximumSkyGroups; group++)
    {
        sky_group_accumulator[group].vertices.clear();
        sky_group_accumulator[group].used = false;
    }

    render_state->ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

static void RenderSkyEquirect(void)
{
    GLuint sky_tex_id = ImageCache(sky_image, true, render_view_effect_colormap);

    if (current_map->forced_skystretch_ > kSkyStretchUnset)
        current_sky_stretch = current_map->forced_skystretch_;
    else if (!level_flags.mouselook)
        current_sky_stretch = kSkyStretchVanilla;
    else
        current_sky_stretch = (SkyStretch)sky_stretch_mode.d_;

    SetupSkyMatrices();

    float ty = 2.0f;

    if (current_sky_stretch == kSkyStretchStretch || current_sky_stretch == kSkyStretchVanilla)
        ty = 1.0f;

    RGBAColor    fc_to_use = current_map->outdoor_fog_color_;
    float        fd_to_use = 0.01f * current_map->outdoor_fog_density_;
    BlendingMode blend     = kBlendingNoZBuffer;

    if (fc_to_use == kRGBANoValue)
    {
        fc_to_use = view_properties->fog_color;
        fd_to_use = view_properties->fog_density;
    }
    if (draw_culling.d_)
    {
        fc_to_use = kRGBANoValue;
        fd_to_use = 0.0f;
        blend     = (BlendingMode)(blend | kBlendingNoFog);
    }
    else if (fc_to_use != kRGBANoValue)
    {
        fd_to_use *= (current_sky_stretch == kSkyStretchVanilla ? 0.03f : 0.010f);
    }

    float offx = 0.0f;
    float offy = 0.0f;

    if (sky_ref)
    {
        if (!epi::AlmostEquals(sky_ref->old_offset.Y, sky_ref->offset.Y) && !console_active && !paused &&
            !menu_active && !time_stop_active && !erraticism_active)
            offy = HMM_Lerp(sky_ref->old_offset.Y, fractional_tic, sky_ref->offset.Y) - kMBFSkyYShift;
        else
            offy = sky_ref->offset.Y - kMBFSkyYShift;

        offy /= sky_image->ScaledHeight();
    }
    else
    {
        if (ddf_scroll_tic != game_tic)
        {
            ddf_old_sky_scroll = ddf_sky_scroll;
            ddf_sky_scroll.X += current_map->sky_scroll_x_;
            ddf_sky_scroll.Y += current_map->sky_scroll_y_;
            ddf_scroll_tic = game_tic;
        }
        if (!epi::AlmostEquals(current_map->sky_scroll_x_, 0.0f))
        {
            if (!console_active && !paused && !menu_active && !time_stop_active && !erraticism_active)
                offx = HMM_Lerp(ddf_old_sky_scroll.X, fractional_tic, ddf_sky_scroll.X);
            else
                offx = ddf_sky_scroll.X;
        }
        if (!epi::AlmostEquals(current_map->sky_scroll_y_, 0.0f))
        {
            if (!console_active && !paused && !menu_active && !time_stop_active && !erraticism_active)
                offy = HMM_Lerp(ddf_old_sky_scroll.Y, fractional_tic, ddf_sky_scroll.Y);
            else
                offy = ddf_sky_scroll.Y;
        }

    }

    float sky_horizontal_tilings = 4.0f;

    if (sky_image->ScaledWidth() > 256)
        sky_horizontal_tilings = HMM_MAX(1024.0f / (float)sky_image->ScaledWidth(), 1.0f);

    float horizon_shift = -0.15f;

    if (current_sky_stretch == kSkyStretchStretch)
        horizon_shift = 0.15f;
    else if (current_sky_stretch == kSkyStretchVanilla)
    {
        float band_fraction = (sky_image->ScaledHeight() > 128) ? 0.30f : 0.18f;

        horizon_shift = (1.0f - 2.0f * band_fraction) * view_y_slope;
    }

    SkyPassInfo sky_pass_info;

    GetSkyInverseMatrices(sky_pass_info.inverse_projection, sky_pass_info.inverse_view);

    sky_pass_info.viewport_origin    = {{(float)view_window_x, (float)view_window_y}};
    sky_pass_info.viewport_size      = {{(float)view_window_width, (float)view_window_height}};
    sky_pass_info.stretch_mode       = (int)current_sky_stretch;
    sky_pass_info.ty                 = ty;
    sky_pass_info.u_scale            = sky_horizontal_tilings;
    sky_pass_info.u_offset           = offx;
    sky_pass_info.v_offset           = offy;
    sky_pass_info.fog_depth          = renderer_far_clip.f_ * 2.0f;
    sky_pass_info.vertical_fov_slope = view_y_slope;
    sky_pass_info.horizon_shift      = horizon_shift;

    RendererVertex *glvert = BeginRenderUnit(GL_TRIANGLES, 6, GL_MODULATE, sky_tex_id,
                                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use,
                                             &sky_pass_info);

    static const HMM_Vec2 kFullscreenQuad[6] = {
        {{-1.0f, -1.0f}}, {{1.0f, -1.0f}}, {{1.0f, 1.0f}}, {{-1.0f, -1.0f}}, {{1.0f, 1.0f}}, {{-1.0f, 1.0f}}};

    for (int i = 0; i < 6; i++)
    {
        glvert->rgba       = kRGBAWhite;
        glvert++->position = {{kFullscreenQuad[i].X, kFullscreenQuad[i].Y, 0.0f}};
    }

    EndRenderUnit(6);
}

static void RenderSkybox(void)
{
    float dist = renderer_far_clip.f_ / 2.0f;

    EPI_ASSERT(current_fake_box);

    SetupSkyMatrices();

    float v0 = 0.0f;
    float v1 = 1.0f;

    RGBAColor fc_to_use = current_map->outdoor_fog_color_;
    float     fd_to_use = 0.01f * current_map->outdoor_fog_density_;
    BlendingMode blend     = kBlendingNoZBuffer;
    // check for sector fog
    if (fc_to_use == kRGBANoValue)
    {
        fc_to_use = view_properties->fog_color;
        fd_to_use = view_properties->fog_density;
    }
    if (draw_culling.d_)
    {
        fc_to_use = kRGBANoValue;
        fd_to_use = 0.0f;
        blend     = (BlendingMode)(blend | kBlendingNoFog);
    }
    else if (fc_to_use != kRGBANoValue)
    {
        fd_to_use *= (current_sky_stretch == kSkyStretchVanilla ? 0.015f : 0.045f);
        //fd_to_use *= (current_sky_stretch == kSkyStretchVanilla ? 0.015f : 0.005f);
    }

    RGBAColor unit_col =
        epi::MakeRGBA((uint8_t)(render_view_red_multiplier * 255.0f), (uint8_t)(render_view_green_multiplier * 255.0f),
                      (uint8_t)(render_view_blue_multiplier * 255.0f));
    // top
    RendererVertex *glvert =
        BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxTop], (GLuint)kTextureEnvironmentDisable,
                        0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{-dist, dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{-dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{dist, dist, +dist}};

    EndRenderUnit(4);

    // bottom
    glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxBottom],
                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{-dist, -dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{-dist, dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{dist, dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{dist, -dist, -dist}};

    EndRenderUnit(4);

    // north
    glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxNorth],
                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{-dist, dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{-dist, dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{dist, dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{dist, dist, -dist}};

    EndRenderUnit(4);

    // east
    glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxEast],
                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{dist, dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{dist, dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{dist, -dist, -dist}};

    EndRenderUnit(4);

    // south
    glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxSouth],
                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{dist, -dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{-dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{-dist, -dist, -dist}};

    EndRenderUnit(4);

    // west
    glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, current_fake_box->texture[kSkyboxWest],
                             (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fc_to_use, fd_to_use);

    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v0}};
    glvert++->position             = {{-dist, -dist, -dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v0, v1}};
    glvert++->position             = {{-dist, -dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v1}};
    glvert++->position             = {{-dist, dist, +dist}};
    glvert->rgba                   = unit_col;
    glvert->texture_coordinates[0] = {{v1, v0}};
    glvert++->position             = {{-dist, dist, -dist}};

    EndRenderUnit(4);
}

static void FlushSkyGroupPunch(int group)
{
    EDGE_ZoneScoped;

    SkyGroupAccumulator &accumulator = sky_group_accumulator[group];

    if (accumulator.vertices.empty())
        return;

    if (render_state->HasStencilBuffer())
    {
        render_state->Enable(GL_STENCIL_TEST);
        render_state->StencilFunction(GL_ALWAYS, group + 1, 0xFF);
        render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_REPLACE);
        render_state->StencilWriteMask(0xFF);
    }

    size_t offset = 0;

    while (offset < accumulator.vertices.size())
    {
        size_t chunk = accumulator.vertices.size() - offset;

        if (chunk > kMaximumLocalVertices)
            chunk = kMaximumLocalVertices;

        StartUnitBatch(false);

        RendererVertex *glvert = BeginRenderUnit(GL_TRIANGLES, (int)chunk, GL_MODULATE, 0,
                                                 (GLuint)kTextureEnvironmentDisable, 0, 0, kBlendingNone);

        for (size_t i = 0; i < chunk; i++)
            glvert[i] = accumulator.vertices[offset + i];

        EndRenderUnit((int)chunk);
        FinishUnitBatch();

        offset += chunk;
    }
}

void FlushSky(void)
{
    EDGE_ZoneScoped;

    for (int group = 0; group < kMaximumSkyGroups; group++)
        FlushSkyGroupPunch(group);
}

void FinishSky(bool use_depth_mask)
{
    EDGE_ZoneScoped;

    render_state->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (!need_to_draw_sky)
        return;

    if (draw_culling.d_)
        render_state->Disable(GL_DEPTH_TEST);

    if (use_depth_mask)
        render_state->DepthFunction(GL_GREATER);
    else
        render_state->DepthMask(false);

    if (render_state->HasStencilBuffer())
    {
        render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_KEEP);
        render_state->StencilWriteMask(0x00);
    }

    const Image *saved_sky_image = sky_image;
    MapSurface  *saved_sky_ref   = sky_ref;

    for (int group = 0; group < kMaximumSkyGroups; group++)
    {
        SkyGroupAccumulator &accumulator = sky_group_accumulator[group];

        if (!accumulator.used)
            continue;

        if (render_state->HasStencilBuffer())
            render_state->StencilFunction(GL_EQUAL, group + 1, 0xFF);

        sky_image = accumulator.image ? accumulator.image : saved_sky_image;
        sky_ref   = accumulator.ref;

        StartUnitBatch(false);

        UpdateSkyboxTextures();

        if (custom_skybox)
            RenderSkybox();
        else
            RenderSkyEquirect();

        FinishUnitBatch();

        RendererRevertSkyMatrices();
    }

    sky_image = saved_sky_image;
    sky_ref   = saved_sky_ref;

    if (render_state->HasStencilBuffer())
    {
        render_state->Disable(GL_STENCIL_TEST);
        render_state->StencilWriteMask(0xFF);
    }

    if (draw_culling.d_)
        render_state->Enable(GL_DEPTH_TEST);

    if (use_depth_mask)
        render_state->DepthFunction(GL_LEQUAL);
    else
        render_state->DepthMask(true);
}

void RenderSkyPlane(Subsector *sub, float h, Sector *sky_owner)
{
    need_to_draw_sky = true;

    Seg *seg = sub->segs;
    if (!seg)
        return;

    float x0 = seg->vertex_1->X;
    float y0 = seg->vertex_1->Y;
    render_mirror_set.Coordinate(x0, y0);
    seg = seg->subsector_next;
    if (!seg)
        return;

    float x1 = seg->vertex_1->X;
    float y1 = seg->vertex_1->Y;
    render_mirror_set.Coordinate(x1, y1);
    seg = seg->subsector_next;
    if (!seg)
        return;

    int group = MarkSkyGroup(sky_owner);

    render_mirror_set.Height(h);

    while (seg)
    {
        float x2 = seg->vertex_1->X;
        float y2 = seg->vertex_1->Y;
        render_mirror_set.Coordinate(x2, y2);

        PushSkyVertex(group, {{x0, y0, h}});
        PushSkyVertex(group, {{x1, y1, h}});
        PushSkyVertex(group, {{x2, y2, h}});

        x1  = x2;
        y1  = y2;
        seg = seg->subsector_next;
    }
}

void RenderSkyWall(Seg *seg, float h1, float h2, Sector *sky_owner)
{
    need_to_draw_sky = true;

    int group = MarkSkyGroup(sky_owner);

    float x1 = seg->vertex_1->X;
    float y1 = seg->vertex_1->Y;
    float x2 = seg->vertex_2->X;
    float y2 = seg->vertex_2->Y;

    render_mirror_set.Coordinate(x1, y1);
    render_mirror_set.Coordinate(x2, y2);

    render_mirror_set.Height(h1);
    render_mirror_set.Height(h2);

    PushSkyVertex(group, {{x1, y1, h1}});
    PushSkyVertex(group, {{x1, y1, h2}});
    PushSkyVertex(group, {{x2, y2, h2}});
    PushSkyVertex(group, {{x2, y2, h1}});
    PushSkyVertex(group, {{x2, y2, h2}});
    PushSkyVertex(group, {{x1, y1, h1}});
}

//----------------------------------------------------------------------------

static const char *UserSkyFaceName(const char *base, int face)
{
    static char       buffer[64];
    static const char letters[] = "NESWTB";

    stbsp_sprintf(buffer, "%s_%c", base, letters[face]);
    return buffer;
}

void UpdateSkyboxTextures(void)
{
    FakeSkybox *info = &fake_box_cache[MakeSkyboxCacheKey(sky_image, render_view_effect_colormap)];

    current_fake_box = info;

    if (info->base_sky == sky_image && info->effect_colormap == render_view_effect_colormap)
    {
        custom_skybox = (info->face[kSkyboxNorth] != nullptr);
        return;
    }

    info->base_sky        = sky_image;
    info->effect_colormap = render_view_effect_colormap;

    // check for custom sky boxes
    info->face[kSkyboxNorth] =
        ImageLookup(UserSkyFaceName(sky_image->name_.c_str(), kSkyboxNorth), kImageNamespaceTexture, kImageLookupNull);

    // LOBO 2022:
    // If we do nothing, our EWAD skybox will be used for all maps.
    // So we need to disable it if we have a pwad that contains it's
    // own sky.
    if (DisableStockSkybox(sky_image->name_.c_str()))
    {
        info->face[kSkyboxNorth] = nullptr;
        // LogPrint("Skybox turned OFF\n");
    }

    // Set colors for culling fog and faux skybox caps - Dasho
    const uint8_t *what_palette = nullptr;
    if (sky_image->source_palette_ >= 0)
        what_palette = (const uint8_t *)LoadLumpIntoMemory(sky_image->source_palette_);
    ImageData *tmp_img_data = ReadAsEpiBlock((Image *)sky_image);
    if (tmp_img_data->depth_ == 1)
    {
        ImageData *rgb_img_data = RGBFromPalettised(
            tmp_img_data, what_palette ? what_palette : (const uint8_t *)&playpal_data[0], sky_image->opacity_);
        delete tmp_img_data;
        tmp_img_data = rgb_img_data;
    }
    culling_fog_color = tmp_img_data->AverageColor(0, sky_image->width_, 0, sky_image->height_ / 2);
    delete tmp_img_data;

    if (what_palette)
        delete[] what_palette;

    if (info->face[kSkyboxNorth])
    {
        custom_skybox = true;

        info->face_size = info->face[kSkyboxNorth]->width_;

        for (int i = kSkyboxEast; i < 6; i++)
            info->face[i] = ImageLookup(UserSkyFaceName(sky_image->name_.c_str(), i), kImageNamespaceTexture);

        for (int k = 0; k < 6; k++)
            MarkImageAsSky(info->face[k]);

        for (int k = 0; k < 6; k++)
            info->texture[k] = ImageCache(info->face[k], true, render_view_effect_colormap);
    }
    else
    {
        info->face_size = 256;
        custom_skybox   = false;
    }
}

void ShutdownSky(void)
{
    sky_ref            = nullptr;
    ddf_scroll_tic     = -1;
    ddf_sky_scroll     = {{0, 0}};
    ddf_old_sky_scroll = {{0, 0}};
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
