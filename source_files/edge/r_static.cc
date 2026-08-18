#include "r_static.h"

#include <math.h>
#include <stddef.h>

#include <vector>

#include "con_var.h"
#include "ddf_main.h"
#include "dm_state.h"
#include "epi.h"
#include "epi_color.h"
#include "epi_doomdefs.h"
#include "edge_profiling.h"
#include "g_game.h"
#include "i_defs_gl.h"
#include "r_colormap.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_misc.h"
#include "r_shader.h"
#include "r_state.h"
#include "r_units.h"

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_mesh, "1", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE(r_static_mesh_resident, "1", kConsoleVariableFlagArchive)

static constexpr size_t kMaximumStaticRun = 3 * 4096;

static inline BlendingMode StaticSurfaceBlending(float alpha, ImageOpacity opacity)
{
    BlendingMode blending;

    if (alpha >= 0.99f && opacity == kOpacitySolid)
        blending = kBlendingNone;
    else if (alpha < 0.11f || opacity == kOpacityComplex)
        blending = kBlendingMasked;
    else
        blending = kBlendingLess;

    if (alpha < 0.99f || opacity == kOpacityComplex)
        blending = (BlendingMode)(blending | kBlendingAlpha);

    return blending;
}

struct StaticSpan
{
    Sector *sector;
    int     start;
    int     count;
    int     baked_light;
    int     light_adjust;
    int     flag_slot;
    bool    is_wall;
    bool    mid_masked;
    bool    live;

    HMM_Vec3 normal;
    float    low[3];
    float    high[3];
    float    div_x, div_y, div_delta_x, div_delta_y;
};

struct SpanReference
{
    int batch;
    int span;
};

struct StaticBatch
{
    const Image      *image;
    const Colormap   *colormap;
    RegionProperties *properties;
    Sector           *sector;
    BlendingMode      blending;

    std::vector<RendererVertex> vertices;
    std::vector<StaticSpan>     spans;

    uint32_t gpu_handle = 0;
    bool     gpu_dirty  = true;
};

static std::vector<StaticBatch> static_batches;
static std::vector<uint8_t>     subsector_flat_baked;
static std::vector<uint8_t>     seg_wall_baked;
static bool                     static_mesh_built = false;

static std::vector<std::vector<SpanReference>> sector_spans;
static std::vector<int>                       sector_light_cache;

static int capture_dependencies[2 + kVertexSectorListMaximum * 2];
static int capture_dependency_count = 0;

static void AddCaptureDependency(const Sector *sec)
{
    if (!sec)
        return;

    int index = (int)(sec - level_sectors);

    for (int i = 0; i < capture_dependency_count; i++)
    {
        if (capture_dependencies[i] == index)
            return;
    }

    if (capture_dependency_count >= (int)(sizeof(capture_dependencies) / sizeof(capture_dependencies[0])))
        return;

    capture_dependencies[capture_dependency_count++] = index;
}

static void AddCaptureDependencyList(const VertexSectorList *seclist)
{
    if (!seclist)
        return;

    for (int k = 0; k < seclist->total; k++)
        AddCaptureDependency(level_sectors + seclist->sectors[k]);
}

static int  capture_batch      = -1;
static int  capture_flag_slot  = -1;
static bool capture_is_wall    = false;
static bool capture_mid_masked = false;
static HMM_Vec3 capture_normal = {{0, 0, 1}};
static float capture_div[4]    = {0, 0, 0, 0};
static int  capture_light      = 0;
static int  capture_adjust     = 0;
static Sector *capture_sector  = nullptr;
static const Seg        *capture_seg  = nullptr;
static const MapSurface *capture_surf = nullptr;

static int WallPartIndex(const Seg *seg, const MapSurface *surf)
{
    if (!seg || !seg->sidedef || !surf)
        return -1;

    if (surf == &seg->sidedef->bottom)
        return 0;
    if (surf == &seg->sidedef->middle)
        return 1;
    if (surf == &seg->sidedef->top)
        return 2;

    return -1;
}

bool StaticMeshEnabled(void)
{
    return r_static_mesh.d_ != 0;
}

bool StaticFlatBakeEligible(const Sector *sec, int face_dir)
{
    if (!r_static_mesh.d_ || !static_mesh_built || !sec || sec->bake_dynamic || sec->movement_suppressed)
    {
        return false;
    }

    if (sec->floor_slope || sec->ceiling_slope || sec->floor_vertex_slope || sec->ceiling_vertex_slope)
    {
        return false;
    }

    if (sec->height_sector || sec->bottom_extrafloor || sec->top_extrafloor)
    {
        return false;
    }

    const MapSurface &surf = (face_dir > 0) ? sec->floor : sec->ceiling;

    if (EDGE_IMAGE_IS_SKY(surf))
    {
        return false;
    }

    if (surf.override_properties || surf.boom_colormap)
    {
        return false;
    }

    const Image *image = surf.image;

    if (!image)
    {
        return false;
    }

    if (image->liquid_type_ != kLiquidImageNone)
    {
        return false;
    }

    if (image->animation_.speed != 0)
    {
        return false;
    }

    if ((ImageOpacity)image->opacity_ == kOpacityComplex)
    {
        return false;
    }

    if (surf.translucency < 0.99f)
    {
        return false;
    }

    if (sec->properties.special)
    {
        float bob = (face_dir > 0) ? sec->properties.special->floor_bob_ : sec->properties.special->ceiling_bob_;

        if (bob > 0)
        {
            return false;
        }
    }

    return true;
}

bool StaticMeshCoversFlat(const Subsector *sub, int face_dir)
{
    if (!r_static_mesh.d_ || !static_mesh_built || !sub)
        return false;

    size_t slot = (size_t)(sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1);

    if (slot >= subsector_flat_baked.size())
        return false;

    return subsector_flat_baked[slot] != 0;
}

static int FindBatch(const Image *image, const Colormap *colormap, RegionProperties *props, Sector *sec,
                     BlendingMode blending)
{
    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &b = static_batches[i];

        if (b.image == image && b.colormap == colormap && b.blending == blending &&
            b.properties->fog_color == props->fog_color && b.properties->fog_density == props->fog_density)
            return (int)i;
    }

    StaticBatch batch;

    batch.image      = image;
    batch.colormap   = colormap;
    batch.properties = props;
    batch.sector     = sec;
    batch.blending   = blending;

    static_batches.push_back(batch);

    return (int)static_batches.size() - 1;
}

static bool SectorListStatic(const VertexSectorList *seclist)
{
    if (!seclist)
        return true;

    for (int k = 0; k < seclist->total; k++)
    {
        const Sector *sec = level_sectors + seclist->sectors[k];

        if (sec->bake_dynamic || sec->movement_suppressed)
            return false;
    }

    return true;
}

static bool StaticImageEligible(const Image *image, float translucency)
{
    if (!image)
        return false;

    if (image->liquid_type_ != kLiquidImageNone)
        return false;

    if (image->animation_.speed != 0)
        return false;

    if ((ImageOpacity)image->opacity_ == kOpacityComplex)
        return false;

    if (translucency < 0.99f)
        return false;

    return true;
}

bool StaticWallBakeEligible(const Seg *seg, const MapSurface *surf, bool mid_masked, BlendingMode blending)
{
    if (!r_static_mesh.d_ || !static_mesh_built || !seg || seg->miniseg || !seg->sidedef || !surf)
    {
        return false;
    }

    if (mid_masked && (blending & kBlendingAlpha))
    {
        return false;
    }

    if (mid_masked && seg->linedef && seg->linedef->special && seg->linedef->special->glass_)
    {
        return false;
    }

    if (WallPartIndex(seg, surf) < 0)
    {
        return false;
    }

    const Side *side = seg->sidedef;

    if (side->bake_dynamic)
    {
        return false;
    }

    const Sector *front = seg->front_sector;
    const Sector *back  = seg->back_sector;

    if (!front || front->bake_dynamic || front->movement_suppressed)
    {
        return false;
    }

    if (front->floor_slope || front->ceiling_slope || front->floor_vertex_slope || front->ceiling_vertex_slope)
    {
        return false;
    }

    if (front->height_sector || front->bottom_extrafloor || front->top_extrafloor)
    {
        return false;
    }

    if (back)
    {
        if (back->bake_dynamic || back->movement_suppressed)
        {
            return false;
        }

        if (back->floor_slope || back->ceiling_slope || back->floor_vertex_slope || back->ceiling_vertex_slope)
        {
            return false;
        }

        if (back->height_sector || back->bottom_extrafloor || back->top_extrafloor)
        {
            return false;
        }
    }

    if (seg->linedef && (seg->linedef->flags & kLineFlagMirror))
    {
        return false;
    }

    if (seg->linedef && seg->linedef->portal_pair)
    {
        return false;
    }

    if (seg->linedef && seg->linedef->slide_door)
    {
        return false;
    }

    if (!SectorListStatic(seg->vertex_sectors[0]) || !SectorListStatic(seg->vertex_sectors[1]))
    {
        return false;
    }

    if (surf->override_properties || surf->boom_colormap)
    {
        return false;
    }

    if (EDGE_IMAGE_IS_SKY(*surf))
    {
        return false;
    }

    if (!StaticImageEligible(surf->image, surf->translucency))
    {
        return false;
    }

    return true;
}

bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf)
{
    if (!r_static_mesh.d_ || !static_mesh_built || !seg)
        return false;

    int part = WallPartIndex(seg, surf);

    if (part < 0)
        return false;

    size_t slot = (size_t)(seg - level_segs) * 3 + (size_t)part;

    if (slot >= seg_wall_baked.size())
        return false;

    return seg_wall_baked[slot] != 0;
}

void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked)
{
    capture_mid_masked = mid_masked;

    capture_normal = normal;
    capture_div[0] = div_x;
    capture_div[1] = div_y;
    capture_div[2] = div_delta_x;
    capture_div[3] = div_delta_y;

    capture_batch  = FindBatch(image, props->colourmap, props, sector, blending);
    capture_seg    = seg;
    capture_surf   = surf;
    capture_sector = sector;
    capture_adjust = light_adjust;
    capture_light  = sector->properties.light_level;

    capture_dependency_count = 0;

    AddCaptureDependency(sector);

    if (seg)
    {
        capture_is_wall = true;

        int part = WallPartIndex(seg, surf);

        capture_flag_slot = (part < 0) ? -1 : (int)((seg - level_segs) * 3 + part);

        AddCaptureDependency(seg->front_sector);
        AddCaptureDependency(seg->back_sector);
        AddCaptureDependencyList(seg->vertex_sectors[0]);
        AddCaptureDependencyList(seg->vertex_sectors[1]);
    }
}

void StaticCaptureBeginFlat(const Subsector *sub, int face_dir, const Image *image, RegionProperties *props,
                            Sector *sector, BlendingMode blending, const HMM_Vec3 &normal)
{
    capture_normal = normal;
    capture_div[0] = capture_div[1] = capture_div[2] = capture_div[3] = 0;

    capture_batch  = FindBatch(image, props->colourmap, props, sector, blending);
    capture_seg    = nullptr;
    capture_surf   = nullptr;
    capture_sector = sector;
    capture_adjust = 0;
    capture_light  = sector->properties.light_level;

    capture_is_wall    = false;
    capture_mid_masked = false;
    capture_flag_slot = (int)((sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1));

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
}

void StaticCaptureVertices(const RendererVertex *verts, int count)
{
    if (capture_batch < 0 || count < 3)
        return;

    StaticBatch &batch = static_batches[capture_batch];

    StaticSpan span;

    span.sector       = capture_sector;
    span.start        = (int)batch.vertices.size();
    span.baked_light  = capture_light;
    span.light_adjust = capture_adjust;
    span.flag_slot    = capture_flag_slot;
    span.is_wall      = capture_is_wall;
    span.mid_masked   = capture_mid_masked;
    span.live         = true;
    span.normal       = capture_normal;
    span.div_x        = capture_div[0];
    span.div_y        = capture_div[1];
    span.div_delta_x  = capture_div[2];
    span.div_delta_y  = capture_div[3];

    for (int axis = 0; axis < 3; axis++)
    {
        span.low[axis]  = verts[0].position.Elements[axis];
        span.high[axis] = verts[0].position.Elements[axis];
    }

    for (int v = 1; v < count; v++)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            span.low[axis]  = HMM_MIN(span.low[axis], verts[v].position.Elements[axis]);
            span.high[axis] = HMM_MAX(span.high[axis], verts[v].position.Elements[axis]);
        }
    }

    for (int t = 1; t < count - 1; t++)
    {
        batch.vertices.push_back(verts[0]);
        batch.vertices.push_back(verts[t]);
        batch.vertices.push_back(verts[t + 1]);
    }

    for (int v = span.start; v < (int)batch.vertices.size(); v++)
    {
        RendererVertex &dest = batch.vertices[v];

        dest.rgba = epi::MakeRGBA(255, 255, 255, epi::GetRGBAAlpha(dest.rgba));
    }

    span.count = (int)batch.vertices.size() - span.start;

    batch.gpu_dirty = true;

    SpanReference ref;

    ref.batch = capture_batch;
    ref.span  = (int)batch.spans.size();

    batch.spans.push_back(span);

    for (int i = 0; i < capture_dependency_count; i++)
        sector_spans[capture_dependencies[i]].push_back(ref);

    if (span.flag_slot >= 0)
    {
        if (span.is_wall)
        {
            if ((size_t)span.flag_slot < seg_wall_baked.size())
                seg_wall_baked[span.flag_slot] = 1;
        }
        else if ((size_t)span.flag_slot < subsector_flat_baked.size())
            subsector_flat_baked[span.flag_slot] = 1;
    }
}

void StaticMeshInvalidateSector(Sector *sec)
{
    if (!static_mesh_built || !sec)
        return;

    size_t index = (size_t)(sec - level_sectors);

    if (index >= sector_spans.size())
        return;

    std::vector<SpanReference> &refs = sector_spans[index];

    for (size_t i = 0; i < refs.size(); i++)
    {
        StaticBatch &batch = static_batches[refs[i].batch];
        StaticSpan  &span  = batch.spans[refs[i].span];

        if (!span.live)
            continue;

        span.live = false;

        if (span.flag_slot < 0)
            continue;

        if (span.is_wall)
        {
            if ((size_t)span.flag_slot < seg_wall_baked.size())
                seg_wall_baked[span.flag_slot] = 0;
        }
        else if ((size_t)span.flag_slot < subsector_flat_baked.size())
            subsector_flat_baked[span.flag_slot] = 0;
    }

    refs.clear();
}

void StaticCaptureEnd(void)
{
    capture_batch  = -1;
    capture_seg    = nullptr;
    capture_surf   = nullptr;
    capture_sector = nullptr;
}

static void RefreshStaticLighting(void)
{
    for (size_t i = 0; i < sector_light_cache.size(); i++)
    {
        Sector *sec = level_sectors + i;

        int current = sec->properties.light_level;

        if (current == sector_light_cache[i])
            continue;

        sector_light_cache[i] = current;

        const std::vector<SpanReference> &refs = sector_spans[i];

        for (size_t r = 0; r < refs.size(); r++)
        {
            StaticBatch &batch = static_batches[refs[r].batch];
            StaticSpan  &span  = batch.spans[refs[r].span];

            if (!span.live || span.sector != sec)
                continue;

            float light = ((float)((current + span.light_adjust) / 4) + 0.5f) / 64.0f;

            for (int v = span.start; v < span.start + span.count; v++)
                batch.vertices[v].texture_coordinates[1].Y = light;

            batch.gpu_dirty = true;
        }
    }
}

void BuildStaticMesh(void)
{
    DestroyStaticMesh();

    subsector_flat_baked.assign((size_t)total_level_subsectors * 2, 0);
    seg_wall_baked.assign((size_t)total_level_segs * 3, 0);
    sector_spans.assign((size_t)total_level_sectors, std::vector<SpanReference>());

    sector_light_cache.resize((size_t)total_level_sectors);

    for (int i = 0; i < total_level_sectors; i++)
        sector_light_cache[i] = level_sectors[i].properties.light_level;

    static_mesh_built = true;
}

void DestroyStaticMesh(void)
{
    for (size_t i = 0; i < static_batches.size(); i++)
    {
        if (static_batches[i].gpu_handle)
            DeleteStaticVertexBuffer(static_batches[i].gpu_handle);
    }

    static_batches.clear();
    subsector_flat_baked.clear();
    seg_wall_baked.clear();
    sector_spans.clear();
    sector_light_cache.clear();

    StaticCaptureEnd();

    static_mesh_built = false;
}

void StaticMeshStats(int *batches, int *live_spans, int *dead_spans, int *vertices)
{
    *batches    = (int)static_batches.size();
    *live_spans = 0;
    *dead_spans = 0;
    *vertices   = 0;

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        const StaticBatch &batch = static_batches[i];

        for (size_t k = 0; k < batch.spans.size(); k++)
        {
            if (batch.spans[k].live)
            {
                (*live_spans)++;
                *vertices += batch.spans[k].count;
            }
            else
                (*dead_spans)++;
        }
    }
}

void DrawStaticMesh(void)
{
    if (!r_static_mesh.d_ || !static_mesh_built)
        return;

    EDGE_ZoneScoped;

    {
        EDGE_ZoneScopedN("StaticMesh RefreshLighting");

        RefreshStaticLighting();
    }

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &batch = static_batches[i];

        if (batch.spans.empty())
            continue;

        GLuint tex_id = ImageCache(batch.image, true, render_view_effect_colormap);

        AbstractShader *shader = GetColormapShader(batch.properties, 0, batch.sector);

        bool resident = r_static_mesh_resident.d_ != 0;

        if (resident && batch.gpu_dirty)
        {
            EDGE_ZoneScopedN("StaticMesh upload");

            if (batch.gpu_handle)
                DeleteStaticVertexBuffer(batch.gpu_handle);

            batch.gpu_handle = CreateStaticVertexBuffer(batch.vertices.data(), (int)batch.vertices.size());
            batch.gpu_dirty  = false;
        }

        if (resident && !batch.gpu_handle)
            resident = false;

        int pass = 0;

        int run_start = -1;
        int run_end   = -1;

        for (size_t k = 0; k <= batch.spans.size(); k++)
        {
            bool live = (k < batch.spans.size()) && batch.spans[k].live;

            bool contiguous = live && run_start >= 0 && batch.spans[k].start == run_end &&
                              (size_t)(run_end + batch.spans[k].count - run_start) <= kMaximumStaticRun;

            if (contiguous)
            {
                run_end += batch.spans[k].count;
                continue;
            }

            if (run_start >= 0)
            {
                if (resident)
                    shader->WorldBakedResident(batch.gpu_handle, GL_TRIANGLES, run_start, run_end - run_start, tex_id,
                                               &pass, batch.blending);
                else
                    shader->WorldBaked(GL_TRIANGLES, batch.vertices.data() + run_start, run_end - run_start, tex_id,
                                       1.0f, &pass, batch.blending);

                run_start = -1;
                run_end   = -1;
            }

            if (live)
            {
                run_start = batch.spans[k].start;
                run_end   = batch.spans[k].start + batch.spans[k].count;
            }
        }

        if (!use_dynamic_lights)
            continue;

        EDGE_ZoneScopedN("StaticMesh lights");

        for (size_t k = 0; k < batch.spans.size(); k++)
        {
            const StaticSpan &span = batch.spans[k];

            if (!span.live)
                continue;

            StaticSpanLighting info;

            info.vertices = batch.vertices.data() + span.start;
            info.count    = span.count;
            info.normal   = span.normal;
            info.is_wall    = span.is_wall;
            info.mid_masked = span.mid_masked;
            info.sector   = span.sector;
            info.tex_id   = tex_id;
            info.blending = batch.blending;

            info.div_x       = span.div_x;
            info.div_y       = span.div_y;
            info.div_delta_x = span.div_delta_x;
            info.div_delta_y = span.div_delta_y;

            for (int axis = 0; axis < 3; axis++)
            {
                info.low[axis]  = span.low[axis];
                info.high[axis] = span.high[axis];
            }

            EmitStaticSpanLights(info);
        }
    }
}
