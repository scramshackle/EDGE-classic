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
static void SkyResidentReset(void);

void ComputeSkyHeights(void)
{
    SkyResidentReset();

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

    for (i = 0, sec = level_sectors; i < total_level_sectors; i++, sec++)
    {
        if (rings[i].group > 0)
            sec->sky_height = rings[i].maximum_height;
    }

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

    GLuint cubemap = 0;

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

    if (box.cubemap != 0)
    {
        DeleteSkyCubemap(box.cubemap);
        box.cubemap = 0;
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

EDGE_DEFINE_CONSOLE_VARIABLE(r_sky_resident, "1", kConsoleVariableFlagArchive)

struct SkySpan
{
    int  start;
    int  count;
    int  flag_slot;
    bool is_wall;
    bool live;
};

struct SkySpanReference
{
    int section;
    int span;
};

struct SkySection
{
    const Image *image = nullptr;
    MapSurface  *ref   = nullptr;

    std::vector<RendererVertex> vertices;

    std::vector<RendererVertex> resident_vertices;
    std::vector<SkySpan>        spans;

    uint32_t gpu_handle = 0;
    bool     gpu_dirty  = false;

    bool used = false;
};

static std::vector<SkySection> sky_sections;

static std::vector<uint8_t> sky_plane_baked;
static std::vector<uint8_t> sky_wall_baked;

static std::vector<std::vector<SkySpanReference>> sky_sector_spans;

static constexpr size_t kMaximumSkyRun = 3 * 4096;

static bool sky_capture_active  = false;
static int  sky_current_section = 0;

static int sky_capture_section   = -1;
static int sky_capture_start     = 0;
static int sky_capture_flag_slot = -1;
static bool sky_capture_is_wall  = false;

static constexpr int kMaximumSkyDependencies = 8;

static int sky_capture_dependencies[kMaximumSkyDependencies];
static int sky_capture_dependency_count = 0;

static void SkyResidentReset(void)
{
    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        if (sky_sections[i].gpu_handle)
            DeleteStaticVertexBuffer(sky_sections[i].gpu_handle);
    }

    sky_sections.clear();

    sky_plane_baked.assign((size_t)total_level_subsectors * 2, 0);
    sky_wall_baked.assign((size_t)total_level_segs * 3, 0);
    sky_sector_spans.assign((size_t)total_level_sectors, std::vector<SkySpanReference>());

    sky_capture_active = false;
}

static void SkyAddCaptureDependency(const Sector *sec)
{
    if (!sec)
        return;

    int index = (int)(sec - level_sectors);

    for (int i = 0; i < sky_capture_dependency_count; i++)
    {
        if (sky_capture_dependencies[i] == index)
            return;
    }

    if (sky_capture_dependency_count >= kMaximumSkyDependencies)
        return;

    sky_capture_dependencies[sky_capture_dependency_count++] = index;
}

static void SkyCaptureBegin(int section, int flag_slot, bool is_wall)
{
    sky_capture_active           = true;
    sky_capture_section          = section;
    sky_capture_start            = (int)sky_sections[section].resident_vertices.size();
    sky_capture_flag_slot        = flag_slot;
    sky_capture_is_wall          = is_wall;
    sky_capture_dependency_count = 0;
}

static void SkyCaptureEnd(void)
{
    if (!sky_capture_active)
        return;

    sky_capture_active = false;

    SkySection &section = sky_sections[sky_capture_section];

    int count = (int)section.resident_vertices.size() - sky_capture_start;

    if (count <= 0)
        return;

    SkySpan span;

    span.start     = sky_capture_start;
    span.count     = count;
    span.flag_slot = sky_capture_flag_slot;
    span.is_wall   = sky_capture_is_wall;
    span.live      = true;

    section.spans.push_back(span);

    SkySpanReference reference;

    reference.section = sky_capture_section;
    reference.span    = (int)section.spans.size() - 1;

    for (int i = 0; i < sky_capture_dependency_count; i++)
    {
        size_t index = (size_t)sky_capture_dependencies[i];

        if (index < sky_sector_spans.size())
            sky_sector_spans[index].push_back(reference);
    }

    if (sky_capture_is_wall)
    {
        if ((size_t)sky_capture_flag_slot < sky_wall_baked.size())
            sky_wall_baked[sky_capture_flag_slot] = 1;
    }
    else if ((size_t)sky_capture_flag_slot < sky_plane_baked.size())
        sky_plane_baked[sky_capture_flag_slot] = 1;
}

void SkyResidentInvalidateSector(Sector *sec)
{
    if (!sec)
        return;

    size_t index = (size_t)(sec - level_sectors);

    if (index >= sky_sector_spans.size())
        return;

    std::vector<SkySpanReference> &refs = sky_sector_spans[index];

    for (size_t i = 0; i < refs.size(); i++)
    {
        SkySection &section = sky_sections[refs[i].section];
        SkySpan     &span   = section.spans[refs[i].span];

        if (!span.live)
            continue;

        span.live = false;

        if (span.flag_slot < 0)
            continue;

        if (span.is_wall)
        {
            if ((size_t)span.flag_slot < sky_wall_baked.size())
                sky_wall_baked[span.flag_slot] = 0;
        }
        else if ((size_t)span.flag_slot < sky_plane_baked.size())
            sky_plane_baked[span.flag_slot] = 0;
    }


    refs.clear();
}

static void PushSkyVertex(int section, const HMM_Vec3 &position)
{
    RendererVertex vertex;

    vertex.rgba     = kRGBAWhite;
    vertex.position = position;

    if (sky_capture_active)
    {
        sky_sections[section].resident_vertices.push_back(vertex);
        sky_sections[section].gpu_dirty = true;
        return;
    }

    sky_sections[section].vertices.push_back(vertex);
}

static int MarkSkySection(Sector *sky_owner)
{
    const Image *image = (sky_owner && sky_owner->sky_image) ? sky_owner->sky_image : sky_image;
    MapSurface  *ref   = sky_owner ? sky_owner->sky_ref : nullptr;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        if (sky_sections[i].image == image && sky_sections[i].ref == ref)
        {
            sky_sections[i].used = true;
            return (int)i;
        }
    }

    SkySection section;

    section.image = image;
    section.ref   = ref;
    section.used  = true;

    sky_sections.push_back(section);

    return (int)sky_sections.size() - 1;
}

void BeginSky(void)
{
    need_to_draw_sky = false;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        sky_sections[i].vertices.clear();
        sky_sections[i].used = false;
    }
}

static void EmitSkyGeometry(const SkySection &section, GLuint texture, BlendingMode blend,
                            RGBAColor fog_color, float fog_density, const SkyPassInfo *sky_pass_info)
{
    SkySection &resident = sky_sections[sky_current_section];

    if (!resident.resident_vertices.empty())
    {
        if (resident.gpu_dirty)
        {
            if (resident.gpu_handle)
                DeleteStaticVertexBuffer(resident.gpu_handle);

            resident.gpu_handle =
                CreateStaticVertexBuffer(resident.resident_vertices.data(), (int)resident.resident_vertices.size());
            resident.gpu_dirty  = false;
        }

        if (resident.gpu_handle)
        {
            int run_start = -1;
            int run_end   = -1;

            for (size_t k = 0; k <= resident.spans.size(); k++)
            {
                bool live = (k < resident.spans.size()) && resident.spans[k].live;

                bool contiguous = live && run_start >= 0 && resident.spans[k].start == run_end &&
                                  (size_t)(run_end + resident.spans[k].count - run_start) <= kMaximumSkyRun;

                if (contiguous)
                {
                    run_end += resident.spans[k].count;
                    continue;
                }

                if (run_start >= 0)
                {
                    AddStaticRenderUnit(resident.gpu_handle, GL_TRIANGLES, run_start, run_end - run_start,
                                        GL_MODULATE, texture, (GLuint)kTextureEnvironmentDisable, 0, 0, blend,
                                        fog_color, fog_density, sky_pass_info);

                    run_start = -1;
                    run_end   = -1;
                }

                if (live)
                {
                    run_start = resident.spans[k].start;
                    run_end   = resident.spans[k].start + resident.spans[k].count;
                }
            }
        }
    }

    size_t offset = 0;

    while (offset < section.vertices.size())
    {
        size_t chunk = section.vertices.size() - offset;

        if (chunk > kMaximumLocalVertices)
            chunk = kMaximumLocalVertices;

        chunk -= chunk % 3;

        if (chunk == 0)
            break;

        RendererVertex *glvert = BeginRenderUnit(GL_TRIANGLES, (int)chunk, GL_MODULATE, texture,
                                                 (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fog_color,
                                                 fog_density, sky_pass_info);

        for (size_t i = 0; i < chunk; i++)
        {
            glvert[i]      = section.vertices[offset + i];
            glvert[i].rgba = kRGBAWhite;
        }

        EndRenderUnit((int)chunk);

        offset += chunk;
    }
}

static void RenderSkyEquirect(const SkySection &section)
{
    GLuint sky_tex_id = ImageCache(sky_image, true, render_view_effect_colormap);

    if (current_map->forced_skystretch_ > kSkyStretchUnset)
        current_sky_stretch = current_map->forced_skystretch_;
    else if (!level_flags.mouselook)
        current_sky_stretch = kSkyStretchVanilla;
    else
        current_sky_stretch = (SkyStretch)sky_stretch_mode.d_;

    SkyPassInfo sky_pass_info;

    SetupSkyMatrices();
    GetSkyInverseMatrices(sky_pass_info.inverse_projection, sky_pass_info.inverse_view);
    RendererRevertSkyMatrices();

    float ty = 2.0f;

    if (current_sky_stretch == kSkyStretchStretch || current_sky_stretch == kSkyStretchVanilla)
        ty = 1.0f;

    RGBAColor    fc_to_use = current_map->outdoor_fog_color_;
    float        fd_to_use = 0.01f * current_map->outdoor_fog_density_;
    BlendingMode blend     = kBlendingNone;

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
    sky_pass_info.is_geometry        = 1;

    EmitSkyGeometry(section, sky_tex_id, blend, fc_to_use, fd_to_use, &sky_pass_info);
}

static void RenderSkybox(const SkySection &section)
{
    EPI_ASSERT(current_fake_box);

    RGBAColor    fc_to_use = current_map->outdoor_fog_color_;
    float        fd_to_use = 0.01f * current_map->outdoor_fog_density_;
    BlendingMode blend     = kBlendingNone;

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
    }

    SkyPassInfo sky_pass_info;

    SetupSkyMatrices();
    GetSkyInverseMatrices(sky_pass_info.inverse_projection, sky_pass_info.inverse_view);
    RendererRevertSkyMatrices();

    sky_pass_info.viewport_origin = {{(float)view_window_x, (float)view_window_y}};
    sky_pass_info.viewport_size   = {{(float)view_window_width, (float)view_window_height}};
    sky_pass_info.fog_depth       = renderer_far_clip.f_ * 2.0f;
    sky_pass_info.cube_texture    = current_fake_box->cubemap;
    sky_pass_info.is_box          = 1;
    sky_pass_info.is_geometry     = 1;

    EmitSkyGeometry(section, 0, blend, fc_to_use, fd_to_use, &sky_pass_info);
}

void FinishSky(bool use_depth_mask)
{
    EDGE_ZoneScoped;


    render_state->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (!need_to_draw_sky)
    {
        for (size_t i = 0; i < sky_sections.size(); i++)
        {
            if (!sky_sections[i].resident_vertices.empty())
            {
                need_to_draw_sky = true;
                break;
            }
        }
    }

    if (!need_to_draw_sky)
        return;

    if (draw_culling.d_)
        render_state->Disable(GL_DEPTH_TEST);

    EPI_UNUSED(use_depth_mask);

    const Image *saved_sky_image = sky_image;
    MapSurface  *saved_sky_ref   = sky_ref;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        SkySection &section = sky_sections[i];

        if (!section.used && section.resident_vertices.empty())
            continue;

        sky_current_section = (int)i;

        sky_image = section.image ? section.image : saved_sky_image;
        sky_ref   = section.ref;

        StartUnitBatch(false);

        UpdateSkyboxTextures();

        if (custom_skybox)
            RenderSkybox(section);
        else
            RenderSkyEquirect(section);

        FinishUnitBatch();
    }


    sky_image = saved_sky_image;
    sky_ref   = saved_sky_ref;

    if (draw_culling.d_)
        render_state->Enable(GL_DEPTH_TEST);
}

bool SkyPlaneIsBaked(const Subsector *sub, int face)
{
    if (!r_sky_resident.d_ || !sub || face < 0 || face > 1)
        return false;

    size_t slot = (size_t)(sub - level_subsectors) * 2 + (size_t)face;

    if (slot >= sky_plane_baked.size())
        return false;

    return sky_plane_baked[slot] != 0;
}

bool SkyWallIsBaked(const Seg *seg, int part)
{
    if (!r_sky_resident.d_ || !seg || part < 0 || part > 2)
        return false;

    size_t slot = (size_t)(seg - level_segs) * 3 + (size_t)part;

    if (slot >= sky_wall_baked.size())
        return false;

    return sky_wall_baked[slot] != 0;
}

void RenderSkyPlane(Subsector *sub, float h, Sector *sky_owner, int face)
{
    need_to_draw_sky = true;

    Seg *seg = sub->segs;
    if (!seg)
        return;

    size_t plane_slot = (size_t)(sub - level_subsectors) * 2 + (size_t)(face ? 1 : 0);

    bool bake = r_sky_resident.d_ && plane_slot < sky_plane_baked.size() && render_mirror_set.TotalActive() == 0;

    if (bake && sky_plane_baked[plane_slot])
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

    int group = MarkSkySection(sky_owner);

    if (bake)
    {
        SkyCaptureBegin(group, (int)plane_slot, false);

        SkyAddCaptureDependency(sub->sector);
        SkyAddCaptureDependency(sky_owner);
        SkyAddCaptureDependency(sub->deep_water_reference);
    }

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

    SkyCaptureEnd();
}

void RenderSkyWall(Seg *seg, float h1, float h2, Sector *sky_owner, int part)
{
    need_to_draw_sky = true;

    size_t wall_slot = (size_t)(seg - level_segs) * 3 + (size_t)(part < 0 ? 0 : (part > 2 ? 2 : part));

    bool bake = r_sky_resident.d_ && wall_slot < sky_wall_baked.size() && render_mirror_set.TotalActive() == 0;

    if (bake && seg->back_sector && seg->front_sector)
    {
        const Sector *other = (sky_owner == seg->front_sector) ? seg->back_sector : seg->front_sector;

        const Image *owner_sky = (sky_owner && sky_owner->sky_image) ? sky_owner->sky_image : sky_image;
        const Image *other_sky = (other && other->sky_image) ? other->sky_image : sky_image;

        if (owner_sky != other_sky)
            bake = false;
    }

    if (bake && sky_wall_baked[wall_slot])
        return;

    int group = MarkSkySection(sky_owner);

    if (bake)
    {
        SkyCaptureBegin(group, (int)wall_slot, true);

        SkyAddCaptureDependency(sky_owner);
        SkyAddCaptureDependency(seg->front_sector);
        SkyAddCaptureDependency(seg->back_sector);

        if (seg->front_subsector)
            SkyAddCaptureDependency(seg->front_subsector->sector);

        if (seg->back_subsector)
            SkyAddCaptureDependency(seg->back_subsector->sector);
    }

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

    SkyCaptureEnd();
}

//----------------------------------------------------------------------------

static const char *UserSkyFaceName(const char *base, int face)
{
    static char       buffer[64];
    static const char letters[] = "NESWTB";

    stbsp_sprintf(buffer, "%s_%c", base, letters[face]);
    return buffer;
}

static ImageData *SkyFaceAsRGBA(const Image *face)
{
    if (!face)
        return nullptr;

    const uint8_t *face_palette = nullptr;

    if (face->source_palette_ >= 0)
        face_palette = (const uint8_t *)LoadLumpIntoMemory(face->source_palette_);

    ImageData *data = ReadAsEpiBlock((Image *)face);

    if (data->depth_ == 1)
    {
        ImageData *rgb = RGBFromPalettised(data, face_palette ? face_palette : (const uint8_t *)&playpal_data[0],
                                           face->opacity_);
        delete data;
        data = rgb;
    }

    if (face_palette)
        delete[] face_palette;

    if (data->depth_ == 3)
        data->SetAlpha(255);

    return data;
}

static void BuildSkyCubemap(FakeSkybox *info)
{
    if (info->cubemap)
    {
        DeleteSkyCubemap(info->cubemap);
        info->cubemap = 0;
    }

    static const int kCubeFaceOrder[6] = {kSkyboxEast, kSkyboxWest,  kSkyboxTop,
                                          kSkyboxBottom, kSkyboxSouth, kSkyboxNorth};

    ImageData *faces[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    bool complete = true;

    for (int i = 0; i < 6; i++)
    {
        faces[i] = SkyFaceAsRGBA(info->face[kCubeFaceOrder[i]]);

        if (!faces[i])
            complete = false;
    }

    if (complete)
        info->cubemap = CreateSkyCubemap(faces, info->face_size);

    for (int i = 0; i < 6; i++)
        delete faces[i];
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

        BuildSkyCubemap(info);
    }
    else
    {
        info->face_size = 256;
        custom_skybox   = false;
    }
}

void ShutdownSky(void)
{
    SkyResidentReset();

    sky_ref            = nullptr;
    ddf_scroll_tic     = -1;
    ddf_sky_scroll     = {{0, 0}};
    ddf_old_sky_scroll = {{0, 0}};
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
