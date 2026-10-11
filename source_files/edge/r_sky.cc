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

#include <float.h>
#include <math.h>

#include <algorithm>

#include "dm_state.h"
#include "edge_profiling.h"
#include "epi.h"
#include "epi_str_util.h"
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
#include "r_polygon.h"
#include "r_sky.h"
#include "r_static.h"
#include "r_texgl.h"
#include "r_units.h"
#include "w_flat.h"
#include "w_wad.h"

const Image *sky_image;

// Reference for Boom sky transfer, if applicable
MapSurface *sky_ref = nullptr;

bool custom_skybox;

// needed for SKY
extern ImageData *ReadAsEpiBlock(Image *rim);

extern ConsoleVariable draw_culling;

static epi::Vec2 ddf_sky_scroll     = {0, 0};
static epi::Vec2 ddf_old_sky_scroll = {0, 0};
static int      ddf_scroll_tic     = -1;

SkyStretch current_sky_stretch = kSkyStretchUnset;

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

static void MergeSkyRings(SectorSkyRing *ring1, SectorSkyRing *ring2)
{
    SectorSkyRing *tmp_R;

    // we require sky on both sides
    if (ring1->group == 0 || ring2->group == 0)
        return;

    // already in the same group ?
    if (ring1->group == ring2->group)
        return;

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
        SectorSkyRing *ring1, *ring2;

        if (!ld->side[0] || !ld->side[1])
            continue;

        sec1 = ld->front_sector;
        sec2 = ld->back_sector;

        EPI_ASSERT(sec1 && sec2);

        if (sec1 == sec2)
            continue;

        ring1 = rings + (sec1 - level_sectors);
        ring2 = rings + (sec2 - level_sectors);

        MergeSkyRings(ring1, ring2);
    }

    const std::vector<SectorPolygonContainment> &containers = SectorPolygonSelfReferenceContainers();

    for (size_t k = 0; k < containers.size(); k++)
    {
        if (containers[k].owner < 0 || containers[k].owner >= total_level_sectors || containers[k].container < 0 ||
            containers[k].container >= total_level_sectors)
            continue;

        MergeSkyRings(rings + containers[k].owner, rings + containers[k].container);
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

    int face_size = 1;

    GLuint cubemap = 0;

    // face images are only present for custom skyboxes.
    // pseudo skyboxes are generated outside of the image system.
    const Image *face[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
};

static std::unordered_map<uint64_t, FakeSkybox> fake_box_cache;

static FakeSkybox *current_fake_box = nullptr;

static uint64_t MakeSkyboxCacheKey(const Image *base_sky)
{
    return (uint64_t)(uintptr_t)base_sky;
}

static void DeleteSkyTexGroup(FakeSkybox &box)
{
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


struct SkySpan
{
    int           start;
    int           count;
    int           flag_slot;
    int           height_key;
    const Sector *height_front;
    const Sector *height_back;
    float         view_z_minimum;
    float         view_z_maximum;
    bool          is_wall;
    bool          live;
    bool          entry_needed;
    int           varying_slot;
    int           cell;

    const LineSide *facing_side;
};

struct SkyRun
{
    int start;
    int count;
};

struct SkyCell
{
    float bounds[4] = {0, 0, 0, 0};

    std::vector<SkyRun> fixed_runs;
    std::vector<int>    varying_spans;
};

struct SkySpanReference
{
    int section;
    int span;
};

struct SkySection
{
    const Image *image   = nullptr;
    MapSurface  *ref     = nullptr;
    bool         flipped = false;

    std::vector<RendererVertex> vertices;

    std::vector<RendererVertex> resident_vertices;
    std::vector<SkySpan>        spans;

    uint32_t gpu_handle = 0;
    bool     gpu_dirty  = false;

    std::vector<SkyCell> cells;
    SkyCell              loose;
    float                fixed_view_z_low  = 0.0f;
    float                fixed_view_z_high = 0.0f;
    bool                 runs_dirty        = true;

    bool used = false;
};

static std::vector<SkySection> sky_sections;

struct SkyMirrorBucket
{
    const DrawMirror *mirror = nullptr;

    std::vector<std::vector<RendererVertex>> section_vertices;
};

static std::vector<SkyMirrorBucket> sky_mirror_buckets;

static int32_t sky_current_bucket = -1;

static bool     sky_mirror_active  = false;
static epi::Mat4 sky_mirror_inverse = {};

static bool sky_backdrop_pass = false;

static constexpr int kSkyWallParts = kSkyWallPartEntry + 1;

static int32_t SkyBucketFor(const DrawMirror *mir)
{
    if (!mir)
        return -1;

    for (size_t i = 0; i < sky_mirror_buckets.size(); i++)
    {
        if (sky_mirror_buckets[i].mirror == mir)
            return (int32_t)i;
    }

    SkyMirrorBucket bucket;

    bucket.mirror = mir;

    sky_mirror_buckets.push_back(bucket);

    return (int32_t)sky_mirror_buckets.size() - 1;
}

static std::vector<uint8_t> sky_plane_baked;
static std::vector<uint8_t> sky_wall_baked;

static std::vector<std::vector<SkySpanReference>> sky_sector_spans;

static constexpr size_t kMaximumSkyRun = 3 * 4096;

static bool sky_capture_active  = false;
static int  sky_current_section = 0;

static int           sky_capture_section        = -1;
static int           sky_capture_start          = 0;
static int           sky_capture_flag_slot      = -1;
static int           sky_capture_height_key     = 0;
static const Sector *sky_capture_height_front   = nullptr;
static const Sector *sky_capture_height_back    = nullptr;
static float         sky_capture_view_z_minimum = -FLT_MAX;
static float         sky_capture_view_z_maximum = FLT_MAX;
static bool          sky_capture_is_wall        = false;

static const LineSide *sky_capture_facing_side = nullptr;

static constexpr int kMaximumSkyDependencies = 8;

static int sky_capture_dependencies[kMaximumSkyDependencies];
static int sky_capture_dependency_count = 0;

static uint32_t sky_resident_generation = 0;

static std::vector<uint8_t>      sky_entry_tracked;
static std::vector<int>          sky_entry_light;
static std::vector<const Image *> sky_entry_image;
static std::vector<MapSurface *> sky_entry_ref;
static std::vector<uint8_t>      sky_entry_flipped;
static std::vector<int>          sky_entry_sectors;
static std::vector<uint8_t>      sky_entry_dirty_flag;
static std::vector<int>          sky_entry_dirty;

uint32_t SkyResidentGeneration(void)
{
    return sky_resident_generation;
}

static void SkyResidentReset(void)
{
    sky_resident_generation++;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        if (sky_sections[i].gpu_handle)
            DeleteStaticVertexBuffer(sky_sections[i].gpu_handle);
    }

    sky_sections.clear();

    sky_plane_baked.assign((size_t)total_level_sectors * 2 * kHeightKeyTotal, 0);
    sky_wall_baked.assign((size_t)total_level_lines * 2 * kSkyWallParts * kHeightKeyTotal, 0);
    sky_sector_spans.assign((size_t)total_level_sectors, std::vector<SkySpanReference>());

    sky_entry_tracked.assign((size_t)total_level_sectors, 0);
    sky_entry_light.assign((size_t)total_level_sectors, 0);
    sky_entry_image.assign((size_t)total_level_sectors, nullptr);
    sky_entry_ref.assign((size_t)total_level_sectors, nullptr);
    sky_entry_flipped.assign((size_t)total_level_sectors, 0);
    sky_entry_dirty_flag.assign((size_t)total_level_sectors, 0);
    sky_entry_sectors.clear();
    sky_entry_dirty.clear();

    sky_capture_active = false;
}

static void SkyEntryTrack(const Sector *sec)
{
    if (!sec)
        return;

    size_t index = (size_t)(sec - level_sectors);

    if (index >= sky_entry_tracked.size() || sky_entry_tracked[index])
        return;

    sky_entry_tracked[index] = 1;
    sky_entry_light[index]   = sec->properties.light_level;
    sky_entry_image[index]   = sec->sky_image;
    sky_entry_ref[index]     = sec->sky_ref;
    sky_entry_flipped[index] = sec->sky_flipped ? 1 : 0;

    sky_entry_sectors.push_back((int)index);
}

void SkyEntryNoteSectorChanged(int index)
{
    if (index < 0 || (size_t)index >= sky_entry_dirty_flag.size() || sky_entry_dirty_flag[(size_t)index])
        return;

    sky_entry_dirty_flag[(size_t)index] = 1;

    sky_entry_dirty.push_back(index);
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

static int SkyHeightKey(const Sector *front, const Sector *back)
{
    return SectorHeightState(front) * kHeightStateTotal + SectorHeightState(back);
}

static size_t SkyPlaneSlot(const Sector *sector, int face, int height_key)
{
    return ((size_t)(sector - level_sectors) * 2 + (size_t)(face ? 1 : 0)) * (size_t)kHeightKeyTotal +
           (size_t)height_key;
}

static size_t SkyWallSlot(const LineSide *line_side, int part, int height_key)
{
    int clamped = (part < 0) ? 0 : ((part >= kSkyWallParts) ? kSkyWallParts - 1 : part);

    return ((size_t)(line_side - level_line_sides) * kSkyWallParts + (size_t)clamped) * (size_t)kHeightKeyTotal +
           (size_t)height_key;
}

static void SkyCaptureBegin(int section, int flag_slot, int height_key, const Sector *height_front,
                            const Sector *height_back, float view_z_minimum, float view_z_maximum, bool is_wall,
                            const LineSide *facing_side)
{
    sky_capture_active           = true;
    sky_capture_section          = section;
    sky_capture_height_key       = height_key;
    sky_capture_height_front     = height_front;
    sky_capture_height_back      = height_back;
    sky_capture_view_z_minimum   = view_z_minimum;
    sky_capture_view_z_maximum   = view_z_maximum;
    sky_capture_start            = (int)sky_sections[section].resident_vertices.size();
    sky_capture_flag_slot        = flag_slot;
    sky_capture_is_wall          = is_wall;
    sky_capture_facing_side      = facing_side;
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

    span.start          = sky_capture_start;
    span.count          = count;
    span.flag_slot      = sky_capture_flag_slot;
    span.height_key     = sky_capture_height_key;
    span.height_front   = sky_capture_height_front;
    span.height_back    = sky_capture_height_back;
    span.view_z_minimum = sky_capture_view_z_minimum;
    span.view_z_maximum = sky_capture_view_z_maximum;
    span.is_wall        = sky_capture_is_wall;
    span.live           = true;
    span.facing_side    = sky_capture_facing_side;
    span.entry_needed   = false;
    span.varying_slot   = -1;
    span.cell           = -1;

    if (span.facing_side)
    {
        span.entry_needed = SkyEntryClipNeeded(span.facing_side->back_sector, span.facing_side->front_sector);

        SkyEntryTrack(span.facing_side->front_sector);
        SkyEntryTrack(span.facing_side->back_sector);
    }

    section.spans.push_back(span);

    section.runs_dirty = true;

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

        section.runs_dirty = true;

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

static bool SkyEntryFacesView(const LineSide *line_side)
{
    float x1 = line_side->vertex_1->x;
    float y1 = line_side->vertex_1->y;
    float x2 = line_side->vertex_2->x;
    float y2 = line_side->vertex_2->y;

    return (view_x - x1) * (y2 - y1) - (view_y - y1) * (x2 - x1) > 0.0f;
}

static void PushSkyVertex(int section, const epi::Vec3 &position)
{
    RendererVertex vertex;

    vertex.rgba     = kRGBAWhite;
    vertex.position = position;

    if (sky_current_bucket >= 0)
    {
        SkyMirrorBucket &bucket = sky_mirror_buckets[(size_t)sky_current_bucket];

        if (bucket.section_vertices.size() <= (size_t)section)
            bucket.section_vertices.resize((size_t)section + 1);

        bucket.section_vertices[(size_t)section].push_back(vertex);
        return;
    }

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
    const Image *image   = (sky_owner && sky_owner->sky_image) ? sky_owner->sky_image : sky_image;
    MapSurface  *ref     = sky_owner ? sky_owner->sky_ref : nullptr;
    bool         flipped = ref && sky_owner->sky_flipped;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        if (sky_sections[i].image == image && sky_sections[i].ref == ref && sky_sections[i].flipped == flipped)
        {
            sky_sections[i].used = true;
            return (int)i;
        }
    }

    SkySection section;

    section.image   = image;
    section.ref     = ref;
    section.flipped = flipped;
    section.used    = true;

    sky_sections.push_back(section);

    return (int)sky_sections.size() - 1;
}

void BeginSky(void)
{
    need_to_draw_sky = false;

    sky_mirror_buckets.clear();
    sky_current_bucket = -1;

    for (size_t i = 0; i < sky_sections.size(); i++)
    {
        sky_sections[i].vertices.clear();
        sky_sections[i].used = false;
    }
}

static bool SkySpanHeightVaries(const SkySpan &span)
{
    return (span.height_front && span.height_front->height_sector) ||
           (span.height_back && span.height_back->height_sector);
}

static bool SkySpanWantsVarying(const SkySpan &span)
{
    if (!span.live)
        return false;

    if (span.facing_side)
        return span.entry_needed;

    return SkySpanHeightVaries(span);
}

static bool SkySpanVisible(const SkySpan &span)
{
    if (!span.live)
        return false;

    if (span.height_key != SkyHeightKey(span.height_front, span.height_back))
        return false;

    if (view_z <= span.view_z_minimum || view_z >= span.view_z_maximum)
        return false;

    if (span.facing_side && !(span.entry_needed && SkyEntryFacesView(span.facing_side)))
        return false;

    return true;
}

static void AppendSkyRun(std::vector<SkyRun> &runs, int start, int count)
{
    if (!runs.empty())
    {
        SkyRun &last = runs.back();

        if (last.start + last.count == start && (size_t)(last.count + count) <= kMaximumSkyRun)
        {
            last.count += count;
            return;
        }
    }

    runs.push_back(SkyRun{start, count});
}

static std::vector<SkyRun> sky_fixed_frame_runs;
static std::vector<SkyRun> sky_varying_runs;

static SkyCell &SkySpanCell(SkySection &section, const SkySpan &span)
{
    if (span.cell >= 0 && (size_t)span.cell < section.cells.size())
        return section.cells[(size_t)span.cell];

    return section.loose;
}

static void SkyUpdateVaryingMembership(SkySection &section, int span_index)
{
    SkySpan &span = section.spans[(size_t)span_index];
    SkyCell &cell = SkySpanCell(section, span);

    bool want = SkySpanWantsVarying(span);

    if (want && span.varying_slot < 0)
    {
        span.varying_slot = (int)cell.varying_spans.size();
        cell.varying_spans.push_back(span_index);
    }
    else if (!want && span.varying_slot >= 0)
    {
        int last = cell.varying_spans.back();

        cell.varying_spans[(size_t)span.varying_slot] = last;
        section.spans[(size_t)last].varying_slot      = span.varying_slot;
        cell.varying_spans.pop_back();

        span.varying_slot = -1;
    }
}

static constexpr float kSkyCellSize = 1024.0f;

struct SkySpanSortEntry
{
    int64_t key;
    int     group;
    float   angle;
    int     index;
};

static bool SkySpanSortLess(const SkySpanSortEntry &a, const SkySpanSortEntry &b)
{
    if (a.key != b.key)
        return a.key < b.key;

    if (a.group != b.group)
        return a.group < b.group;

    return a.angle < b.angle;
}

void SkyResidentOrganize(void)
{
    for (size_t s = 0; s < sky_sections.size(); s++)
    {
        SkySection &section = sky_sections[s];

        size_t total = section.spans.size();

        if (total == 0)
            continue;

        std::vector<float> span_bounds(total * 4);

        float origin_x = FLT_MAX;
        float origin_y = FLT_MAX;
        float limit_x  = -FLT_MAX;

        for (size_t k = 0; k < total; k++)
        {
            const SkySpan &span = section.spans[k];

            float *b = &span_bounds[k * 4];

            b[0] = FLT_MAX;
            b[1] = FLT_MAX;
            b[2] = -FLT_MAX;
            b[3] = -FLT_MAX;

            for (int v = span.start; v < span.start + span.count; v++)
            {
                const epi::Vec3 &p = section.resident_vertices[(size_t)v].position;

                b[0] = epi::Min(b[0], p.x);
                b[1] = epi::Min(b[1], p.y);
                b[2] = epi::Max(b[2], p.x);
                b[3] = epi::Max(b[3], p.y);
            }

            origin_x = epi::Min(origin_x, b[0]);
            origin_y = epi::Min(origin_y, b[1]);
            limit_x  = epi::Max(limit_x, b[2]);
        }

        int64_t columns = (int64_t)((limit_x - origin_x) / kSkyCellSize) + 1;

        std::vector<int64_t>          keys(total);
        std::vector<SkySpanSortEntry> sort_entries(total);

        for (size_t k = 0; k < total; k++)
        {
            const float   *b    = &span_bounds[k * 4];
            const SkySpan &span = section.spans[k];

            int64_t column = (int64_t)(((b[0] + b[2]) * 0.5f - origin_x) / kSkyCellSize);
            int64_t row    = (int64_t)(((b[1] + b[3]) * 0.5f - origin_y) / kSkyCellSize);

            keys[k] = row * columns + column;

            SkySpanSortEntry &entry = sort_entries[k];

            entry.key   = keys[k];
            entry.angle = 0.0f;
            entry.index = (int)k;

            if (span.facing_side)
            {
                entry.group = span.entry_needed ? 2 : 3;
                entry.angle = atan2f(span.facing_side->vertex_2->y - span.facing_side->vertex_1->y,
                                     span.facing_side->vertex_2->x - span.facing_side->vertex_1->x);
            }
            else
                entry.group = SkySpanHeightVaries(span) ? 1 : 0;
        }

        std::stable_sort(sort_entries.begin(), sort_entries.end(), SkySpanSortLess);

        std::vector<int> order(total);

        for (size_t k = 0; k < total; k++)
            order[k] = sort_entries[k].index;

        std::vector<RendererVertex> vertices;
        std::vector<SkySpan>        spans;
        std::vector<int>            remap(total);

        vertices.reserve(section.resident_vertices.size());
        spans.reserve(total);

        section.cells.clear();

        int64_t current_key = 0;

        for (size_t i = 0; i < total; i++)
        {
            int      old  = order[i];
            SkySpan  span = section.spans[(size_t)old];
            const float *b = &span_bounds[(size_t)old * 4];

            if (section.cells.empty() || keys[(size_t)old] != current_key)
            {
                current_key = keys[(size_t)old];

                SkyCell cell;

                cell.bounds[0] = b[0];
                cell.bounds[1] = b[1];
                cell.bounds[2] = b[2];
                cell.bounds[3] = b[3];

                section.cells.push_back(cell);
            }
            else
            {
                SkyCell &cell = section.cells.back();

                cell.bounds[0] = epi::Min(cell.bounds[0], b[0]);
                cell.bounds[1] = epi::Min(cell.bounds[1], b[1]);
                cell.bounds[2] = epi::Max(cell.bounds[2], b[2]);
                cell.bounds[3] = epi::Max(cell.bounds[3], b[3]);
            }

            int start = (int)vertices.size();

            vertices.insert(vertices.end(), section.resident_vertices.begin() + span.start,
                            section.resident_vertices.begin() + span.start + span.count);

            span.start        = start;
            span.cell         = (int)section.cells.size() - 1;
            span.varying_slot = -1;

            remap[(size_t)old] = (int)spans.size();

            spans.push_back(span);
        }

        section.resident_vertices.swap(vertices);
        section.spans.swap(spans);

        section.gpu_dirty  = true;
        section.runs_dirty = true;

        for (size_t i = 0; i < sky_sector_spans.size(); i++)
        {
            std::vector<SkySpanReference> &refs = sky_sector_spans[i];

            for (size_t r = 0; r < refs.size(); r++)
            {
                if (refs[r].section == (int)s)
                    refs[r].span = remap[(size_t)refs[r].span];
            }
        }
    }
}

struct SkyViewWedge
{
    float apex_x;
    float apex_y;
    float left_x;
    float left_y;
    float left_sign;
    float right_x;
    float right_y;
    float right_sign;
};

static bool SkyComputeViewWedge(SkyViewWedge *wedge)
{
    if (draw_culling.d_ || clip_scope >= kBAMAngle180)
        return false;

    float forward = epi::RadiansFromBAM(view_angle);
    float left    = epi::RadiansFromBAM(view_angle + clip_left);
    float right   = epi::RadiansFromBAM(view_angle + clip_right);

    float forward_x = cosf(forward);
    float forward_y = sinf(forward);

    wedge->apex_x  = view_x - 32.0f * forward_x;
    wedge->apex_y  = view_y - 32.0f * forward_y;
    wedge->left_x  = cosf(left);
    wedge->left_y  = sinf(left);
    wedge->right_x = cosf(right);
    wedge->right_y = sinf(right);

    wedge->left_sign  = wedge->left_x * forward_y - wedge->left_y * forward_x;
    wedge->right_sign = wedge->right_x * forward_y - wedge->right_y * forward_x;

    return true;
}

static bool SkyBoundsOutsideEdge(const float *bounds, float apex_x, float apex_y, float edge_x, float edge_y, float sign)
{
    for (int corner = 0; corner < 4; corner++)
    {
        float x = bounds[(corner & 1) ? 2 : 0] - apex_x;
        float y = bounds[(corner & 2) ? 3 : 1] - apex_y;

        if ((edge_x * y - edge_y * x) * sign >= 0.0f)
            return false;
    }

    return true;
}

static bool SkyCellVisible(const SkyCell &cell, const SkyViewWedge &wedge)
{
    if (SkyBoundsOutsideEdge(cell.bounds, wedge.apex_x, wedge.apex_y, wedge.left_x, wedge.left_y, wedge.left_sign))
        return false;

    if (SkyBoundsOutsideEdge(cell.bounds, wedge.apex_x, wedge.apex_y, wedge.right_x, wedge.right_y, wedge.right_sign))
        return false;

    return true;
}

static void SkyRefreshEntryClips(void)
{
    for (size_t i = 0; i < sky_entry_sectors.size(); i++)
    {
        size_t        index = (size_t)sky_entry_sectors[i];
        const Sector *sec   = level_sectors + index;

        uint8_t flipped = sec->sky_flipped ? 1 : 0;

        if (sec->properties.light_level == sky_entry_light[index] && sec->sky_image == sky_entry_image[index] &&
            sec->sky_ref == sky_entry_ref[index] && flipped == sky_entry_flipped[index])
        {
            continue;
        }

        sky_entry_light[index]   = sec->properties.light_level;
        sky_entry_image[index]   = sec->sky_image;
        sky_entry_ref[index]     = sec->sky_ref;
        sky_entry_flipped[index] = flipped;

        SkyEntryNoteSectorChanged((int)index);
    }

    for (size_t i = 0; i < sky_entry_dirty.size(); i++)
    {
        size_t index = (size_t)sky_entry_dirty[i];

        sky_entry_dirty_flag[index] = 0;

        if (index >= sky_sector_spans.size())
            continue;

        const std::vector<SkySpanReference> &refs = sky_sector_spans[index];

        for (size_t r = 0; r < refs.size(); r++)
        {
            SkySection &section = sky_sections[(size_t)refs[r].section];
            SkySpan    &span    = section.spans[(size_t)refs[r].span];

            if (!span.live || !span.facing_side)
                continue;

            bool needed = SkyEntryClipNeeded(span.facing_side->back_sector, span.facing_side->front_sector);

            if (needed == span.entry_needed)
                continue;

            span.entry_needed = needed;

            if (!section.runs_dirty)
                SkyUpdateVaryingMembership(section, refs[r].span);
        }
    }

    sky_entry_dirty.clear();
}

static void RebuildSkyRuns(SkySection &section)
{
    for (size_t c = 0; c < section.cells.size(); c++)
    {
        section.cells[c].fixed_runs.clear();
        section.cells[c].varying_spans.clear();
    }

    section.loose.fixed_runs.clear();
    section.loose.varying_spans.clear();

    float low  = -FLT_MAX;
    float high = FLT_MAX;

    for (size_t k = 0; k < section.spans.size(); k++)
    {
        SkySpan &span = section.spans[k];
        SkyCell &cell = SkySpanCell(section, span);

        span.varying_slot = -1;

        if (!span.live)
            continue;

        if (span.facing_side || SkySpanHeightVaries(span))
        {
            if (SkySpanWantsVarying(span))
            {
                span.varying_slot = (int)cell.varying_spans.size();
                cell.varying_spans.push_back((int)k);
            }

            continue;
        }

        if (span.view_z_minimum <= view_z)
            low = epi::Max(low, span.view_z_minimum);
        else
            high = epi::Min(high, span.view_z_minimum);

        if (span.view_z_maximum <= view_z)
            low = epi::Max(low, span.view_z_maximum);
        else
            high = epi::Min(high, span.view_z_maximum);

        if (view_z <= span.view_z_minimum || view_z >= span.view_z_maximum)
            continue;

        AppendSkyRun(cell.fixed_runs, span.start, span.count);
    }

    section.fixed_view_z_low  = low;
    section.fixed_view_z_high = high;
    section.runs_dirty        = false;
}

static void EmitSkyGeometry(const SkySection &section, GLuint texture, BlendingMode blend,
                            RGBAColor fog_color, float fog_density, const SkyPassInfo *sky_pass_info)
{
    if (sky_backdrop_pass)
    {
        SkyPassInfo backdrop_info = *sky_pass_info;
        backdrop_info.is_geometry = 0;

        static constexpr float kBackdropCorners[6][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
                                                         {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f}};

        RendererVertex *glvert = BeginRenderUnit(GL_TRIANGLES, 6, GL_MODULATE, texture,
                                                 (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fog_color,
                                                 fog_density, &backdrop_info);

        for (int i = 0; i < 6; i++)
        {
            glvert[i]                        = RendererVertex{};
            glvert[i].rgba                   = kRGBAWhite;
            glvert[i].position               = {kBackdropCorners[i][0], kBackdropCorners[i][1], 1.0f};
            glvert[i].texture_coordinates[0] = {0.0f, 0.0f};
            glvert[i].texture_coordinates[1] = {0.0f, 0.0f};
        }

        EndRenderUnit(6);
        return;
    }

    SkySection &resident = sky_sections[sky_current_section];

    if (!sky_mirror_active && !resident.resident_vertices.empty())
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
            if (resident.runs_dirty || !(view_z > resident.fixed_view_z_low && view_z < resident.fixed_view_z_high))
                RebuildSkyRuns(resident);

            sky_fixed_frame_runs.clear();
            sky_varying_runs.clear();

            SkyViewWedge wedge;

            bool cull = SkyComputeViewWedge(&wedge);

            for (size_t c = 0; c <= resident.cells.size(); c++)
            {
                const SkyCell &cell = (c < resident.cells.size()) ? resident.cells[c] : resident.loose;

                if (cull && c < resident.cells.size() && !SkyCellVisible(cell, wedge))
                    continue;

                for (size_t r = 0; r < cell.fixed_runs.size(); r++)
                    AppendSkyRun(sky_fixed_frame_runs, cell.fixed_runs[r].start, cell.fixed_runs[r].count);

                for (size_t v = 0; v < cell.varying_spans.size(); v++)
                {
                    const SkySpan &span = resident.spans[(size_t)cell.varying_spans[v]];

                    if (SkySpanVisible(span))
                        AppendSkyRun(sky_varying_runs, span.start, span.count);
                }
            }

            for (int pass = 0; pass < 2; pass++)
            {
                const std::vector<SkyRun> &runs = (pass == 0) ? sky_fixed_frame_runs : sky_varying_runs;

                for (size_t r = 0; r < runs.size(); r++)
                {
                    AddStaticRenderUnit(resident.gpu_handle, GL_TRIANGLES, runs[r].start, runs[r].count, GL_MODULATE,
                                        texture, (GLuint)kTextureEnvironmentDisable, 0, 0, blend, fog_color,
                                        fog_density, sky_pass_info);
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

static void UpdateSkyStretch(void)
{
    if (current_map->forced_skystretch_ > kSkyStretchUnset)
        current_sky_stretch = current_map->forced_skystretch_;
    else if (!level_flags.mouselook)
        current_sky_stretch = kSkyStretchVanilla;
    else
        current_sky_stretch = (SkyStretch)sky_stretch_mode.d_;
}

static void RenderSkyEquirect(const SkySection &section)
{
    GLuint sky_tex_id = ImageCache(sky_image, true);

    UpdateSkyStretch();

    SkyPassInfo sky_pass_info;

    SetupSkyMatrices();
    GetSkyInverseMatrices(sky_pass_info.inverse_projection, sky_pass_info.inverse_view);
    RendererRevertSkyMatrices();

    if (sky_mirror_active)
        sky_pass_info.inverse_view = epi::MultiplyMatrices(sky_mirror_inverse, sky_pass_info.inverse_view);

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

    float sky_rotation = 0.0f;

    if (sky_ref)
    {
        bool sky_ref_frozen = console_active || paused || menu_active || time_stop_active || erraticism_active;

        if (!epi::AlmostEquals(sky_ref->old_offset.y, sky_ref->offset.y) && !sky_ref_frozen)
            offy = epi::Lerp(sky_ref->old_offset.y, sky_ref->offset.y, fractional_tic) - sky_ref->base_offset.y;
        else
            offy = sky_ref->offset.y - sky_ref->base_offset.y;

        offy /= sky_image->ScaledHeight();

        if (!epi::AlmostEquals(sky_ref->old_offset.x, sky_ref->offset.x) && !sky_ref_frozen)
            sky_rotation = epi::Lerp(sky_ref->old_offset.x, sky_ref->offset.x, fractional_tic);
        else
            sky_rotation = sky_ref->offset.x;

        sky_rotation /= 65536.0f;
    }
    else
    {
        if (ddf_scroll_tic != game_tic)
        {
            ddf_old_sky_scroll = ddf_sky_scroll;
            ddf_sky_scroll.x += current_map->sky_scroll_x_;
            ddf_sky_scroll.y += current_map->sky_scroll_y_;
            ddf_scroll_tic = game_tic;
        }
        if (!epi::AlmostEquals(current_map->sky_scroll_x_, 0.0f))
        {
            if (!console_active && !paused && !menu_active && !time_stop_active && !erraticism_active)
                offx = epi::Lerp(ddf_old_sky_scroll.x, ddf_sky_scroll.x, fractional_tic);
            else
                offx = ddf_sky_scroll.x;
        }
        if (!epi::AlmostEquals(current_map->sky_scroll_y_, 0.0f))
        {
            if (!console_active && !paused && !menu_active && !time_stop_active && !erraticism_active)
                offy = epi::Lerp(ddf_old_sky_scroll.y, ddf_sky_scroll.y, fractional_tic);
            else
                offy = ddf_sky_scroll.y;
        }

    }

    float sky_horizontal_tilings = 4.0f;

    if (sky_image->ScaledWidth() > 256)
        sky_horizontal_tilings = epi::Max(roundf(1024.0f / (float)sky_image->ScaledWidth()), 1.0f);

    float sky_u_scale  = -sky_horizontal_tilings;
    float sky_u_offset = (0.75f - sky_rotation) * sky_horizontal_tilings - offx;

    if (section.flipped)
    {
        sky_u_scale  = sky_horizontal_tilings;
        sky_u_offset = offx + (sky_rotation - 0.75f) * sky_horizontal_tilings;
    }

    float horizon_shift = -0.15f;

    if (current_sky_stretch == kSkyStretchStretch)
        horizon_shift = 0.15f;
    else if (current_sky_stretch == kSkyStretchVanilla)
    {
        float band_fraction = (sky_image->ScaledHeight() > 128) ? 0.30f : 0.18f;

        horizon_shift = (1.0f - 2.0f * band_fraction) * view_y_slope;
    }

    sky_pass_info.viewport_origin    = {(float)view_window_x, (float)view_window_y};
    sky_pass_info.viewport_size      = {(float)view_window_width, (float)view_window_height};
    sky_pass_info.stretch_mode       = (int)current_sky_stretch;
    sky_pass_info.ty                 = ty;
    sky_pass_info.u_scale            = sky_u_scale;
    sky_pass_info.u_offset           = sky_u_offset;
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

    UpdateSkyStretch();

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

    if (sky_mirror_active)
        sky_pass_info.inverse_view = epi::MultiplyMatrices(sky_mirror_inverse, sky_pass_info.inverse_view);

    sky_pass_info.viewport_origin = {(float)view_window_x, (float)view_window_y};
    sky_pass_info.viewport_size   = {(float)view_window_width, (float)view_window_height};
    sky_pass_info.fog_depth       = renderer_far_clip.f_ / 2.0f;
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

    SkyRefreshEntryClips();

    if (draw_culling.d_)
        render_state->Disable(GL_DEPTH_TEST);

    EPI_UNUSED(use_depth_mask);

    const Image *saved_sky_image = sky_image;
    MapSurface  *saved_sky_ref   = sky_ref;

    if (draw_culling.d_)
    {
        int backdrop = -1;

        for (size_t i = 0; i < sky_sections.size(); i++)
        {
            const SkySection &section = sky_sections[i];

            if (!section.used && section.resident_vertices.empty())
                continue;

            if (backdrop < 0 || (section.image == saved_sky_image && section.ref == nullptr && !section.flipped))
                backdrop = (int)i;
        }

        if (backdrop >= 0)
        {
            SkySection &section = sky_sections[backdrop];

            sky_current_section = backdrop;

            sky_image = section.image ? section.image : saved_sky_image;
            sky_ref   = section.ref;

            sky_backdrop_pass = true;

            StartUnitBatch(false);

            UpdateSkyboxTextures();

            if (custom_skybox)
                RenderSkybox(section);
            else
                RenderSkyEquirect(section);

            FinishUnitBatch();

            sky_backdrop_pass = false;
        }
    }

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

void FinishSkyForMirror(const DrawMirror *mir)
{
    SkyMirrorBucket *bucket = nullptr;

    for (size_t i = 0; i < sky_mirror_buckets.size(); i++)
    {
        if (sky_mirror_buckets[i].mirror == mir)
        {
            bucket = &sky_mirror_buckets[i];
            break;
        }
    }

    if (!bucket)
        return;

    const Image *saved_sky_image = sky_image;
    MapSurface  *saved_sky_ref   = sky_ref;

    sky_mirror_inverse = epi::InverseMatrix(mir->view_matrix);

    sky_mirror_inverse.elements[3][0] = 0.0f;
    sky_mirror_inverse.elements[3][1] = 0.0f;
    sky_mirror_inverse.elements[3][2] = 0.0f;
    sky_mirror_inverse.elements[3][3] = 1.0f;

    sky_mirror_active = true;

    for (size_t i = 0; i < bucket->section_vertices.size() && i < sky_sections.size(); i++)
    {
        if (bucket->section_vertices[i].empty())
            continue;

        SkySection &section = sky_sections[i];

        SkySection view_section;

        view_section.image    = section.image;
        view_section.ref      = section.ref;
        view_section.flipped  = section.flipped;
        view_section.vertices = bucket->section_vertices[i];

        sky_current_section = (int)i;

        sky_image = section.image ? section.image : saved_sky_image;
        sky_ref   = section.ref;

        StartUnitBatch(false);

        UpdateSkyboxTextures();

        if (custom_skybox)
            RenderSkybox(view_section);
        else
            RenderSkyEquirect(view_section);

        FinishUnitBatch();
    }

    sky_mirror_active = false;

    sky_image = saved_sky_image;
    sky_ref   = saved_sky_ref;
}

void SkyNoteResidentVisible(void)
{
    sky_current_bucket = SkyBucketFor(nullptr);
    need_to_draw_sky   = true;
}

bool SkyResidentEnabled(void)
{
    return true;
}

bool SkyWallBakeable(const LineSide *line_side, const Sector *sky_owner)
{
    if (!line_side || !line_side->back_sector || !line_side->front_sector)
        return true;

    const Sector *other = (sky_owner == line_side->front_sector) ? line_side->back_sector : line_side->front_sector;

    const Image *owner_sky = (sky_owner && sky_owner->sky_image) ? sky_owner->sky_image : sky_image;
    const Image *other_sky = (other && other->sky_image) ? other->sky_image : sky_image;

    return owner_sky == other_sky;
}

void RenderSkyPlane(Sector *sector, float h, Sector *sky_owner, int face, DrawMirror *mir)
{
    sky_current_bucket = SkyBucketFor(mir);

    if (!mir)
        need_to_draw_sky = true;

    const SectorPolygon *poly = SectorPolygonForSector((int)(sector - level_sectors));

    if (!poly || poly->status != kSectorPolygonOk || poly->indices.size() < 3)
        return;

    int    plane_key  = SkyHeightKey(sector, nullptr);
    size_t plane_slot = SkyPlaneSlot(sector, face, plane_key);

    bool bake = !mir && plane_slot < sky_plane_baked.size();

    if (bake && sky_plane_baked[plane_slot])
        return;

    int group = MarkSkySection(sky_owner);

    if (bake)
    {
        SkyCaptureBegin(group, (int)plane_slot, plane_key, sector, nullptr, face ? h : -FLT_MAX, face ? FLT_MAX : h,
                        false, nullptr);

        SkyAddCaptureDependency(sector);
        SkyAddCaptureDependency(sky_owner);
        SkyAddCaptureDependency(sector->deep_water_reference);
    }

    for (size_t i = 0; i + 2 < poly->indices.size(); i += 3)
    {
        for (int k = 0; k < 3; k++)
        {
            const Vertex *point = poly->points[poly->indices[i + k]];

            PushSkyVertex(group, {point->x, point->y, h});
        }
    }

    SkyCaptureEnd();
}

void RenderSkyWall(LineSide *line_side, float h1, float h2, Sector *sky_owner, int part, DrawMirror *mir)
{
    sky_current_bucket = SkyBucketFor(mir);

    if (!mir)
        need_to_draw_sky = true;

    int    wall_key  = SkyHeightKey(line_side->front_sector, line_side->back_sector);
    size_t wall_slot = SkyWallSlot(line_side, part, wall_key);

    bool bake = !mir && wall_slot < sky_wall_baked.size() && SkyWallBakeable(line_side, sky_owner);

    if (bake && sky_wall_baked[wall_slot])
        return;

    bool entry = (part == kSkyWallPartEntry);

    if (entry && (mir || !bake) && !SkyEntryClipNeeded(line_side->back_sector, line_side->front_sector))
    {
        return;
    }

    int group = MarkSkySection(sky_owner);

    if (bake)
    {
        SkyCaptureBegin(group, (int)wall_slot, wall_key, line_side->front_sector, line_side->back_sector, -FLT_MAX,
                        FLT_MAX, true, entry ? line_side : nullptr);

        SkyAddCaptureDependency(sky_owner);
        SkyAddCaptureDependency(line_side->front_sector);
        SkyAddCaptureDependency(line_side->back_sector);
    }

    float x1 = line_side->vertex_1->x;
    float y1 = line_side->vertex_1->y;
    float x2 = line_side->vertex_2->x;
    float y2 = line_side->vertex_2->y;

    PushSkyVertex(group, {x1, y1, h1});
    PushSkyVertex(group, {x1, y1, h2});
    PushSkyVertex(group, {x2, y2, h2});
    PushSkyVertex(group, {x2, y2, h1});
    PushSkyVertex(group, {x2, y2, h2});
    PushSkyVertex(group, {x1, y1, h1});

    SkyCaptureEnd();
}

//----------------------------------------------------------------------------

static const char *UserSkyFaceName(const char *base, int face)
{
    static char       buffer[64];
    static const char letters[] = "NESWTB";

    epi::FormatToBufferSized(buffer, sizeof(buffer), "%s_%c", base, letters[face]);
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

    static const int kCubeFaceOrder[6] = {kSkyboxEast,  kSkyboxWest,  kSkyboxBottom,
                                          kSkyboxTop,   kSkyboxNorth, kSkyboxSouth};

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
    FakeSkybox *info = &fake_box_cache[MakeSkyboxCacheKey(sky_image)];

    current_fake_box = info;

    if (info->base_sky == sky_image)
    {
        custom_skybox = (info->face[kSkyboxNorth] != nullptr);
        return;
    }

    info->base_sky = sky_image;

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
    ddf_sky_scroll     = {0, 0};
    ddf_old_sky_scroll = {0, 0};
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
