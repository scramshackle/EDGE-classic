#include "r_static.h"
#include "r_backend.h"
#include "r_misc.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#include <unordered_map>
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
#include "i_system.h"
#include "r_colormap.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_lightgrid.h"
#include "r_image.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_shader.h"
#include "r_state.h"
#include "r_units.h"

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_mesh_resident, "1", kConsoleVariableFlagArchive)

extern ConsoleVariable sector_brightness_correction;

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
    Sector *back_sector;
    Sector *light_sector;
    int     height_key;
    int     start;
    int     count;
    int     baked_light;
    int     light_adjust;
    int      flag_slot;
    uint64_t hash_key;
    bool    is_wall;
    bool    mid_masked;
    bool    live;

    HMM_Vec3 normal;
    float    low[3];
    float    high[3];
    float    div_x, div_y, div_delta_x, div_delta_y;
};

struct StaticRun
{
    int start;
    int count;
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
    OitPass           draw_pass;

    const MapSurface *scroll_surface;
    HMM_Vec2          uv_scale;

    std::vector<RendererVertex> vertices;
    std::vector<StaticSpan>     spans;

    uint32_t gpu_handle   = 0;
    int      gpu_capacity = 0;
    int      gpu_count    = 0;
    int      dirty_low    = INT_MAX;
    int      dirty_high   = -1;
    bool     is_wall      = false;
    int      face_dir     = 0;

    std::vector<StaticRun> runs;
    bool                   runs_dirty       = true;
    bool                   height_sensitive = false;
};

static std::vector<StaticBatch> static_batches;
static std::vector<uint8_t>     sector_flat_baked;
static int                      static_light_correction = 5;
static std::vector<uint8_t>     line_side_wall_baked;
static std::unordered_map<uint64_t, uint8_t> region_surface_baked;
static bool                     static_mesh_built = false;

static std::vector<std::vector<SpanReference>> sector_spans;
static std::vector<int>                       sector_light_cache;
static std::vector<RGBAColor>                 sector_fog_color_cache;
static std::vector<float>                     sector_fog_density_cache;
static std::vector<const Colormap *>          sector_colormap_cache;
static std::vector<StaticSectorChange>        static_sector_changes;
static std::vector<uint8_t>                   sector_height_state;

int SectorHeightState(const Sector *sec)
{
    if (!sec || !sec->height_sector)
        return kHeightStateNormal;

    if (view_height_zone == kHeightZoneA && view_z > sec->height_sector->interpolated_ceiling_height)
        return kHeightStateAbove;

    if (view_height_zone == kHeightZoneC && view_z < sec->height_sector->interpolated_floor_height)
        return kHeightStateBelow;

    return kHeightStateNormal;
}

static bool RefreshSectorHeightStates(void)
{
    bool changed = false;

    for (int i = 0; i < total_level_sectors; i++)
    {
        uint8_t state = (uint8_t)SectorHeightState(level_sectors + i);

        if (state != sector_height_state[i])
        {
            sector_height_state[i] = state;
            changed                = true;
        }
    }

    return changed;
}

static int CachedHeightState(const Sector *sec)
{
    if (!sec)
        return kHeightStateNormal;

    size_t index = (size_t)(sec - level_sectors);

    if (index >= sector_height_state.size())
        return kHeightStateNormal;

    return sector_height_state[index];
}

int StaticHeightKey(const Sector *front, const Sector *back)
{
    return CachedHeightState(front) * kHeightStateTotal + CachedHeightState(back);
}

static int LiveHeightKey(const Sector *front, const Sector *back)
{
    return SectorHeightState(front) * kHeightStateTotal + SectorHeightState(back);
}

static const MapSurface *capture_flat_surface = nullptr;
static HMM_Vec2          capture_scroll_uv      = {{0, 0}};
static HMM_Vec2          capture_uv_scale       = {{0, 0}};

static void SetCaptureScrollOffset(const MapSurface *surf, const HMM_Vec2 &uv_scale)
{
    capture_scroll_uv = {{0, 0}};
    capture_uv_scale  = uv_scale;

    if (!surf)
        return;

    capture_scroll_uv.X = (surf->offset.X - surf->base_offset.X) * uv_scale.X;
    capture_scroll_uv.Y = (surf->offset.Y - surf->base_offset.Y) * uv_scale.Y;
}

static Sector *capture_back_sector = nullptr;
static int      capture_height_key  = 0;
static int capture_dependencies[6 + kVertexSectorListMaximum * 2];
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

static std::unordered_map<const RegionProperties *, Sector *> properties_owner;

static void BuildPropertiesOwnerMap(void)
{
    properties_owner.clear();

    for (int i = 0; i < total_level_sectors; i++)
        properties_owner[&level_sectors[i].properties] = level_sectors + i;
}

static Sector *LookupPropertiesOwner(const RegionProperties *props)
{
    std::unordered_map<const RegionProperties *, Sector *>::const_iterator it = properties_owner.find(props);

    return (it == properties_owner.end()) ? nullptr : it->second;
}

bool StaticPropertiesResolvable(const Sector *sec, const RegionProperties *props)
{
    if (!sec || !props || props == &sec->properties)
        return true;

    return LookupPropertiesOwner(props) != nullptr;
}

static Sector *ResolvePropertiesOwner(Sector *sec, const RegionProperties *props)
{
    if (!sec || !props || props == &sec->properties)
        return sec;

    for (int pass = 0; pass < 2; pass++)
    {
        const Extrafloor *C = (pass == 0) ? sec->bottom_extrafloor : sec->bottom_liquid;

        for (; C; C = C->higher)
        {
            if (!C->extrafloor_line || !C->extrafloor_line->front_sector)
                continue;

            if (props == &C->extrafloor_line->front_sector->properties)
                return C->extrafloor_line->front_sector;
        }
    }

    if (sec->height_sector && props == &sec->height_sector->properties)
        return sec->height_sector;

    Sector *owner = LookupPropertiesOwner(props);

    return owner ? owner : sec;
}

static void AddCaptureDependencyList(const VertexSectorList *seclist)
{
    if (!seclist)
        return;

    for (int k = 0; k < seclist->total; k++)
        AddCaptureDependency(level_sectors + seclist->sectors[k]);
}

static int  capture_batch      = -1;
static int      capture_flag_slot = -1;
static uint64_t capture_hash_key  = 0;
static bool capture_is_wall       = false;
static bool capture_mid_masked    = false;

static HMM_Vec3 capture_normal = {{0, 0, 1}};
static float capture_div[4]    = {0, 0, 0, 0};
static int  capture_light      = 0;
static Sector *capture_light_sector = nullptr;
static int  capture_adjust     = 0;
static Sector *capture_sector  = nullptr;
static const LineSide   *capture_line_side = nullptr;
static const MapSurface *capture_surf = nullptr;

static int WallPartIndex(const LineSide *line_side, const MapSurface *surf)
{
    if (!line_side || !line_side->sidedef || !surf)
        return -1;

    if (surf == &line_side->sidedef->bottom)
        return 0;
    if (surf == &line_side->sidedef->middle)
        return 1;
    if (surf == &line_side->sidedef->top)
        return 2;

    return -1;
}

static int ExtrafloorIndex(const Extrafloor *ef)
{
    if (!ef || !level_extrafloors)
        return -1;

    return (int)(ef - level_extrafloors);
}

static bool BuildRegionWallKey(const LineSide *line_side, int part, const Extrafloor *region_ef,
                               const Extrafloor *surface_ef, int height_key, uint64_t *out)
{
    uint64_t index = (uint64_t)(line_side - level_line_sides);

    if (index >= ((uint64_t)1 << 24))
        return false;

    uint64_t region  = (uint64_t)(ExtrafloorIndex(region_ef) + 1);
    uint64_t surface = (uint64_t)(ExtrafloorIndex(surface_ef) + 1);

    if (region >= ((uint64_t)1 << 16) || surface >= ((uint64_t)1 << 16))
        return false;

    uint64_t slot = (part < 0) ? 3 : (uint64_t)part;

    *out = ((uint64_t)1 << 63) | (index << 38) | (region << 22) | (surface << 6) | (slot << 4) | (uint64_t)height_key;

    return true;
}

static bool BuildRegionFlatKey(const Sector *sec, int face_dir, const Extrafloor *plane_ef, int height_key,
                               uint64_t *out)
{
    uint64_t index = (uint64_t)(sec - level_sectors);

    if (index >= ((uint64_t)1 << 24))
        return false;

    uint64_t plane = (uint64_t)(ExtrafloorIndex(plane_ef) + 1);

    if (plane >= ((uint64_t)1 << 16))
        return false;

    uint64_t face = (face_dir > 0) ? 0 : 1;

    *out = ((uint64_t)1 << 63) | ((uint64_t)1 << 62) | (index << 37) | (plane << 21) | (face << 20) |
           (uint64_t)height_key;

    return true;
}

bool StaticMeshBuilt(void)
{
    return static_mesh_built;
}

static bool static_bake_active = false;
static bool static_bake_deferred = false;

void StaticBakeSetDeferred(bool deferred)
{
    static_bake_deferred = deferred;
}

bool StaticBakeDeferred(void)
{
    return static_bake_deferred;
}

static std::vector<uint8_t>  sector_bake_clean;
static std::vector<uint8_t>  sector_bake_pending;
static std::vector<uint32_t> sector_bake_epoch;
static std::vector<int>     sector_pending_list;

void StaticBakeSectorBegin(const Sector *sec)
{
    size_t index = (size_t)(sec - level_sectors);

    if (index < sector_bake_clean.size())
        sector_bake_clean[index] = 1;
}

void StaticBakeSectorEnd(const Sector *sec)
{
    size_t index = (size_t)(sec - level_sectors);

    if (index < sector_bake_pending.size())
        sector_bake_pending[index] = 0;
}

void StaticMarkSectorDeclined(const Sector *sec)
{
    if (!sec)
        return;

    size_t index = (size_t)(sec - level_sectors);

    if (index < sector_bake_clean.size())
        sector_bake_clean[index] = 0;
}

void StaticMarkSectorPending(const Sector *sec)
{
    if (!sec)
        return;

    size_t index = (size_t)(sec - level_sectors);

    if (index < sector_bake_epoch.size())
        sector_bake_epoch[index]++;

    if (index >= sector_bake_pending.size() || sector_bake_pending[index])
        return;

    sector_bake_pending[index] = 1;
    sector_pending_list.push_back((int)index);
}

bool StaticSectorReady(const Sector *sec)
{
    size_t index = (size_t)(sec - level_sectors);

    if (!static_mesh_built || index >= sector_bake_clean.size())
        return false;

    return sector_bake_clean[index] && !sector_bake_pending[index];
}

uint32_t StaticSectorEpoch(const Sector *sec)
{
    size_t index = (size_t)(sec - level_sectors);

    return (index < sector_bake_epoch.size()) ? sector_bake_epoch[index] : 0;
}

void StaticTakeSettledPendingSectors(std::vector<Sector *> &out)
{
    out.clear();

    size_t keep = 0;

    for (size_t i = 0; i < sector_pending_list.size(); i++)
    {
        int     index = sector_pending_list[i];
        Sector *sec   = level_sectors + index;

        if (!sector_bake_pending[(size_t)index] || sec->bake_dynamic)
            continue;

        if (sec->movement_suppressed)
        {
            sector_pending_list[keep++] = index;
            continue;
        }

        out.push_back(sec);
    }

    sector_pending_list.resize(keep);
}

static void MarkSectorNeighboursPending(const Sector *sec)
{
    StaticMarkSectorPending(sec);

    for (int i = 0; i < sec->line_count; i++)
    {
        const Line *ld = sec->lines[i];

        StaticMarkSectorPending(ld->front_sector);
        StaticMarkSectorPending(ld->back_sector);

        for (int side = 0; side < 2; side++)
        {
            const LineSide *line_side = &level_line_sides[(ld - level_lines) * 2 + side];

            for (int v = 0; v < 2; v++)
            {
                const VertexSectorList *seclist = line_side->vertex_sectors[v];

                if (!seclist)
                    continue;

                for (int k = 0; k < seclist->total; k++)
                    StaticMarkSectorPending(level_sectors + seclist->sectors[k]);
            }
        }
    }
}

void StaticBakeBegin(void)
{
    static_bake_active = true;
}

void StaticBakeEnd(void)
{
    static_bake_active = false;
}

bool StaticBakeActive(void)
{
    return static_bake_active;
}

static bool StaticAnimationUniformSize(const Image *image)
{
    if (!image || image->animation_.speed == 0)
        return true;

    float width  = image->ScaledWidth();
    float height = image->ScaledHeight();

    const Image *frame = image->animation_.next;

    for (int guard = 0; frame && frame != image && guard < 64; guard++)
    {
        if (!epi::AlmostEquals(frame->ScaledWidth(), width) || !epi::AlmostEquals(frame->ScaledHeight(), height))
            return false;

        frame = frame->animation_.next;
    }

    return true;
}

static HMM_Vec2 BatchScrollOffset(const StaticBatch &batch)
{
    HMM_Vec2 offset = {{0, 0}};

    const MapSurface *surf = batch.scroll_surface;

    if (!surf)
        return offset;

    offset.X = (surf->offset.X - surf->base_offset.X) * batch.uv_scale.X;
    offset.Y = (surf->offset.Y - surf->base_offset.Y) * batch.uv_scale.Y;

    return offset;
}

static bool HeightSectorStatic(const Sector *sec)
{
    if (!sec || !sec->height_sector)
        return true;

    return !sec->height_sector->bake_dynamic && !sec->height_sector->movement_suppressed;
}

static bool SurfaceScrolls(const MapSurface *surf)
{
    return surf && surf->scrolls;
}

static int SectorDeclineReason(const Sector *sec, bool back)
{
    bool dynamic    = sec->bake_dynamic;
    bool suppressed = sec->movement_suppressed;
    bool height     = !HeightSectorStatic(sec);

    if (!dynamic && !suppressed && !height)
        return kStaticBakeAccepted;

    if (height)
        return back ? kStaticBakeBackHeightSector : kStaticBakeHeightSector;

    if (dynamic && sec->properties.special &&
        (sec->properties.special->f_.scroll_speed_ > 0 || sec->properties.special->c_.scroll_speed_ > 0))
        return back ? kStaticBakeBackScrolls : kStaticBakeSectorScrolls;

    if (dynamic && (sec->bottom_extrafloor || sec->top_extrafloor))
        return back ? kStaticBakeBackExtrafloor : kStaticBakeExtrafloor;

    if (dynamic)
        return back ? kStaticBakeBackDynamic : kStaticBakeSectorDynamic;

    return back ? kStaticBakeBackSuppressed : kStaticBakeSectorSuppressed;
}

static int FlatSurfaceDecline(const MapSurface &surf, const Sector *owner, int face_dir)
{
    if (EDGE_IMAGE_IS_SKY(surf))
    {
        return kStaticBakeSky;
    }

    if (surf.override_properties && !StaticPropertiesResolvable(owner, surf.override_properties))
    {
        return kStaticBakeOverrideProperties;
    }

    const Image *image = surf.image;

    if (!image)
    {
        return kStaticBakeNoImage;
    }

    if (surf.rotation && SurfaceScrolls(&surf))
    {
        return kStaticBakeRotatedScroll;
    }

    if (!StaticAnimationUniformSize(image))
    {
        return kStaticBakeAnimationSize;
    }

    if ((ImageOpacity)image->opacity_ == kOpacityComplex)
    {
        return kStaticBakeComplexOpacity;
    }

    if (owner && owner->properties.special)
    {
        float bob = (face_dir > 0) ? owner->properties.special->floor_bob_ : owner->properties.special->ceiling_bob_;

        if (bob > 0)
        {
            return kStaticBakeSurfaceBob;
        }
    }

    return kStaticBakeAccepted;
}

int StaticFlatBakeDeclineSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir)
{
    if (!static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!sec || !surf)
    {
        return kStaticBakeNoSurface;
    }

    int sector_reason = SectorDeclineReason(sec, false);

    if (sector_reason != kStaticBakeAccepted)
    {
        return sector_reason;
    }

    if (surf_owner && surf_owner != sec)
    {
        int owner_reason = SectorDeclineReason(surf_owner, true);

        if (owner_reason != kStaticBakeAccepted)
        {
            return owner_reason;
        }
    }

    return FlatSurfaceDecline(*surf, surf_owner, face_dir);
}

bool StaticFlatBakeEligibleSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir)
{
    return StaticFlatBakeDeclineSurface(sec, surf, surf_owner, face_dir) == kStaticBakeAccepted;
}

int StaticFlatBakeDecline(const Sector *sec, int face_dir)
{
    if (!sec)
    {
        return kStaticBakeNoSurface;
    }

    return StaticFlatBakeDeclineSurface(sec, (face_dir > 0) ? &sec->floor : &sec->ceiling, sec, face_dir);
}

static Sector *ExtrafloorControlSector(const Extrafloor *ef)
{
    if (!ef || !ef->extrafloor_line)
        return nullptr;

    return ef->extrafloor_line->front_sector;
}

int StaticExtrafloorPlaneDecline(const Sector *sec, const Extrafloor *plane_ef, int face_dir)
{
    if (!static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!sec || !plane_ef)
    {
        return kStaticBakeNoSurface;
    }

    int sector_reason = SectorDeclineReason(sec, false);

    if (sector_reason != kStaticBakeAccepted)
    {
        return sector_reason;
    }

    Sector *control = ExtrafloorControlSector(plane_ef);

    if (!control)
    {
        return kStaticBakeNoSurface;
    }

    int control_reason = SectorDeclineReason(control, true);

    if (control_reason != kStaticBakeAccepted)
    {
        return control_reason;
    }

    const MapSurface *surf = (face_dir > 0) ? plane_ef->top : plane_ef->bottom;

    if (!surf)
    {
        return kStaticBakeNoSurface;
    }

    int surface_reason = FlatSurfaceDecline(*surf, control, face_dir);

    if (surface_reason != kStaticBakeAccepted)
    {
        return surface_reason;
    }

    uint64_t key;

    if (!BuildRegionFlatKey(sec, face_dir, plane_ef, LiveHeightKey(sec, nullptr), &key))
    {
        return kStaticBakeKeyOverflow;
    }

    return kStaticBakeAccepted;
}

bool StaticExtrafloorPlaneEligible(const Sector *sec, const Extrafloor *plane_ef, int face_dir)
{
    return StaticExtrafloorPlaneDecline(sec, plane_ef, face_dir) == kStaticBakeAccepted;
}
bool StaticFlatBakeEligible(const Sector *sec, int face_dir)
{
    return StaticFlatBakeDecline(sec, face_dir) == kStaticBakeAccepted;
}

bool StaticMeshCoversFlat(const Sector *sec, int face_dir, const Extrafloor *plane_ef)
{
    if (!static_mesh_built || !sec)
        return false;

    int key = LiveHeightKey(sec, nullptr);

    if (plane_ef)
    {
        uint64_t hash_key;

        if (!BuildRegionFlatKey(sec, face_dir, plane_ef, key, &hash_key))
            return false;

        return region_surface_baked.find(hash_key) != region_surface_baked.end();
    }

    size_t slot = ((size_t)(sec - level_sectors) * 2 + (face_dir > 0 ? 0 : 1)) * kHeightKeyTotal + (size_t)key;

    if (slot >= sector_flat_baked.size())
        return false;

    return sector_flat_baked[slot] != 0;
}

static int FindBatch(const Image *image, const Colormap *colormap, RegionProperties *props, Sector *sec,
                     BlendingMode blending, OitPass draw_pass, const MapSurface *scroll_surf, bool is_wall,
                     int face_dir)
{
    const MapSurface *scroller = SurfaceScrolls(scroll_surf) ? scroll_surf : nullptr;

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &b = static_batches[i];

        if (b.image == image && b.colormap == colormap && b.blending == blending && b.draw_pass == draw_pass &&
            b.scroll_surface == scroller && b.is_wall == is_wall && b.face_dir == face_dir &&
            b.properties->fog_color == props->fog_color && b.properties->fog_density == props->fog_density)
            return (int)i;
    }

    StaticBatch batch;

    batch.image      = image;
    batch.colormap   = colormap;
    batch.properties = props;
    batch.sector     = sec;
    batch.blending   = blending;
    batch.draw_pass  = draw_pass;
    batch.is_wall    = is_wall;
    batch.face_dir   = face_dir;

    batch.uv_scale       = capture_uv_scale;
    batch.scroll_surface = scroller;

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

int StaticWallBakeDecline(const LineSide *line_side, const MapSurface *surf, bool mid_masked,
                          const Extrafloor *region_ef, const Extrafloor *surface_ef)
{
    if (!static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!line_side || !surf)
    {
        return kStaticBakeNoSurface;
    }

    if (!line_side->sidedef)
    {
        return kStaticBakeNoSidedef;
    }

    if (mid_masked && line_side->linedef->special && line_side->linedef->special->glass_)
    {
        return kStaticBakeGlass;
    }

    if (WallPartIndex(line_side, surf) < 0 && !surface_ef)
    {
        return kStaticBakeNotSidedefPart;
    }

    if ((line_side->linedef->flags & kLineFlagMirror))
    {
        return kStaticBakeMirrorLine;
    }

    if (line_side->linedef->portal_pair)
    {
        return kStaticBakePortalLine;
    }

    if (line_side->linedef->slide_door)
    {
        return kStaticBakeSlideDoor;
    }

    const Side *side = line_side->sidedef;

    if (side->bake_dynamic)
    {
        return kStaticBakeSideDynamic;
    }

    const Sector *front = line_side->front_sector;
    const Sector *back  = line_side->back_sector;

    if (!front)
    {
        return kStaticBakeNoSurface;
    }

    int front_reason = SectorDeclineReason(front, false);

    if (front_reason != kStaticBakeAccepted)
    {
        return front_reason;
    }

    if (back)
    {
        int back_reason = SectorDeclineReason(back, true);

        if (back_reason != kStaticBakeAccepted)
        {
            return back_reason;
        }
    }

    if (!SectorListStatic(line_side->vertex_sectors[0]) || !SectorListStatic(line_side->vertex_sectors[1]))
    {
        return kStaticBakeVertexSector;
    }

    if (surface_ef)
    {
        Sector *control = ExtrafloorControlSector(surface_ef);

        if (!control)
        {
            return kStaticBakeNoSurface;
        }

        int control_reason = SectorDeclineReason(control, true);

        if (control_reason != kStaticBakeAccepted)
        {
            return control_reason;
        }
    }

    if (surf->override_properties && !StaticPropertiesResolvable(line_side->front_sector, surf->override_properties))
    {
        return kStaticBakeOverrideProperties;
    }

    if (EDGE_IMAGE_IS_SKY(*surf))
    {
        return kStaticBakeSky;
    }

    if (!surf->image)
    {
        return kStaticBakeNoImage;
    }

    if (!StaticAnimationUniformSize(surf->image))
    {
        return kStaticBakeAnimationSize;
    }

    if ((ImageOpacity)surf->image->opacity_ == kOpacityComplex)
    {
        return kStaticBakeComplexOpacity;
    }

    if (region_ef || surface_ef)
    {
        uint64_t key;

        if (!BuildRegionWallKey(line_side, WallPartIndex(line_side, surf), region_ef, surface_ef,
                                LiveHeightKey(front, back), &key))
        {
            return kStaticBakeKeyOverflow;
        }
    }

    return kStaticBakeAccepted;
}

bool StaticWallBakeEligible(const LineSide *line_side, const MapSurface *surf, bool mid_masked,
                            const Extrafloor *region_ef, const Extrafloor *surface_ef)
{
    return StaticWallBakeDecline(line_side, surf, mid_masked, region_ef, surface_ef) == kStaticBakeAccepted;
}

static const char *const static_bake_decline_names[kStaticBakeDeclineTotal] = {"resident",
                                                                              "mesh disabled",
                                                                              "no surface",
                                                                              "no sidedef",
                                                                              "not a sidedef part",
                                                                              "glass linedef",
                                                                              "side is dynamic",
                                                                              "sector is dynamic",
                                                                              "sector scrolls",
                                                                              "sector movement suppressed",
                                                                              "sector has extrafloors",
                                                                              "height sector not static",
                                                                              "back sector is dynamic",
                                                                              "back sector scrolls",
                                                                              "back movement suppressed",
                                                                              "back has extrafloors",
                                                                              "back height sector not static",
                                                                              "mirror linedef",
                                                                              "portal linedef",
                                                                              "slide door",
                                                                              "vertex sector not static",
                                                                              "override properties",
                                                                              "sky",
                                                                              "no image",
                                                                              "animation frame sizes differ",
                                                                              "complex opacity",
                                                                              "rotated and scrolling",
                                                                              "surface bob",
                                                                              "not the sector own plane",
                                                                              "residency key overflow"};

const char *StaticBakeDeclineName(int reason)
{
    if (reason < 0 || reason >= kStaticBakeDeclineTotal)
        return "unknown";

    return static_bake_decline_names[reason];
}



bool StaticMeshCoversWall(const LineSide *line_side, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef)
{
    if (!static_mesh_built || !line_side)
        return false;

    int part = WallPartIndex(line_side, surf);

    if (part < 0 && !surface_ef)
        return false;

    int key = LiveHeightKey(line_side->front_sector, line_side->back_sector);

    if (region_ef || surface_ef)
    {
        uint64_t hash_key;

        if (!BuildRegionWallKey(line_side, part, region_ef, surface_ef, key, &hash_key))
            return false;

        return region_surface_baked.find(hash_key) != region_surface_baked.end();
    }

    size_t slot = ((size_t)(line_side - level_line_sides) * 3 + (size_t)part) * kHeightKeyTotal + (size_t)key;

    if (slot >= line_side_wall_baked.size())
        return false;

    return line_side_wall_baked[slot] != 0;
}

void StaticCaptureBegin(const LineSide *line_side, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale, const Extrafloor *region_ef,
                        const Extrafloor *surface_ef)
{
    SetCaptureScrollOffset(surf, uv_scale);

    capture_mid_masked = mid_masked;

    capture_normal = normal;
    capture_div[0] = div_x;
    capture_div[1] = div_y;
    capture_div[2] = div_delta_x;
    capture_div[3] = div_delta_y;

    capture_back_sector = line_side ? line_side->back_sector : nullptr;
    capture_height_key  = LiveHeightKey(line_side ? line_side->front_sector : sector, capture_back_sector);

    capture_batch =
        FindBatch(image, props->colourmap, props, sector, blending, draw_pass, surf, line_side != nullptr, 0);
    capture_line_side = line_side;
    capture_surf      = surf;
    capture_sector    = sector;
    capture_adjust    = light_adjust;

    capture_light_sector = ResolvePropertiesOwner(sector, props);
    capture_light        = capture_light_sector->properties.light_level;

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
    AddCaptureDependency(sector->height_sector);
    AddCaptureDependency(capture_light_sector);

    if (line_side)
    {
        capture_is_wall = true;

        int part = WallPartIndex(line_side, surf);

        capture_flag_slot = (part < 0) ? -1 : (int)((line_side - level_line_sides) * 3 + part);
        capture_hash_key  = 0;

        if (region_ef || surface_ef)
        {
            capture_flag_slot = -1;

            if (!BuildRegionWallKey(line_side, part, region_ef, surface_ef, capture_height_key, &capture_hash_key))
                capture_hash_key = 0;
        }

        AddCaptureDependency(ExtrafloorControlSector(region_ef));
        AddCaptureDependency(ExtrafloorControlSector(surface_ef));

        AddCaptureDependency(line_side->front_sector);
        AddCaptureDependency(line_side->back_sector);

        if (line_side->front_sector)
            AddCaptureDependency(line_side->front_sector->height_sector);

        if (line_side->back_sector)
            AddCaptureDependency(line_side->back_sector->height_sector);
        AddCaptureDependencyList(line_side->vertex_sectors[0]);
        AddCaptureDependencyList(line_side->vertex_sectors[1]);
    }
}

void StaticCaptureBeginFlat(Sector *sector, int face_dir, const Image *image, RegionProperties *props,
                            BlendingMode blending, const HMM_Vec3 &normal, OitPass draw_pass, const MapSurface *surf,
                            const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef)
{
    capture_flat_surface = surf;

    SetCaptureScrollOffset(surf, uv_scale);

    capture_normal = normal;
    capture_div[0] = capture_div[1] = capture_div[2] = capture_div[3] = 0;

    capture_back_sector = nullptr;
    capture_height_key  = LiveHeightKey(sector, nullptr);

    capture_batch =
        FindBatch(image, props->colourmap, props, sector, blending, draw_pass, capture_flat_surface, false,
                  (face_dir > 0) ? 1 : -1);
    capture_line_side = nullptr;
    capture_surf      = nullptr;
    capture_sector    = sector;
    capture_adjust    = 0;

    capture_light_sector = ResolvePropertiesOwner(sector, props);
    capture_light        = capture_light_sector->properties.light_level;

    capture_is_wall    = false;
    capture_mid_masked = false;
    capture_flag_slot  = (int)((sector - level_sectors) * 2 + (face_dir > 0 ? 0 : 1));
    capture_hash_key   = 0;

    if (plane_ef)
    {
        capture_flag_slot = -1;

        if (!BuildRegionFlatKey(sector, face_dir, plane_ef, capture_height_key, &capture_hash_key))
            capture_hash_key = 0;
    }

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
    AddCaptureDependency(sector->height_sector);
    AddCaptureDependency(sector->deep_water_reference);
    AddCaptureDependency(capture_light_sector);
    AddCaptureDependency(ExtrafloorControlSector(plane_ef));
}

static void MarkBakedSlot(const StaticSpan &span, uint8_t value)
{
    if (span.flag_slot < 0)
        return;

    size_t slot = (size_t)span.flag_slot * kHeightKeyTotal + (size_t)span.height_key;

    std::vector<uint8_t> &baked = span.is_wall ? line_side_wall_baked : sector_flat_baked;

    if (slot < baked.size())
        baked[slot] = value;
}

static float StaticLightRow(int light, int adjust)
{
    int lit = light + adjust + (sector_brightness_correction.d_ - 5) * 10;

    return (floorf((float)lit / 4.0f) + 0.5f) / 64.0f;
}

void StaticCaptureVertices(GLuint shape, const RendererVertex *verts, int count)
{
    if (capture_batch < 0 || count < 3)
        return;

    StaticBatch &batch = static_batches[capture_batch];

    StaticSpan span;

    span.sector       = capture_sector;
    span.back_sector  = capture_back_sector;
    span.light_sector = capture_light_sector;
    span.height_key   = capture_height_key;
    span.start        = (int)batch.vertices.size();
    span.baked_light  = capture_light;
    span.light_adjust = capture_adjust;
    span.flag_slot    = capture_flag_slot;
    span.hash_key     = capture_hash_key;
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

    if (shape == GL_TRIANGLES)
    {
        for (int v = 0, total = (count / 3) * 3; v < total; v++)
            batch.vertices.push_back(verts[v]);
    }
    else
    {
        for (int t = 1; t < count - 1; t++)
        {
            batch.vertices.push_back(verts[0]);
            batch.vertices.push_back(verts[t]);
            batch.vertices.push_back(verts[t + 1]);
        }
    }

    float span_light_row = StaticLightRow(capture_light, capture_adjust);

    for (int v = span.start; v < (int)batch.vertices.size(); v++)
    {
        RendererVertex &dest = batch.vertices[v];

        dest.rgba = epi::MakeRGBA(255, 255, 255, epi::GetRGBAAlpha(dest.rgba));

        dest.texture_coordinates[1].Y = span_light_row;

        dest.texture_coordinates[0].X -= capture_scroll_uv.X;
        dest.texture_coordinates[0].Y -= capture_scroll_uv.Y;
    }

    span.count = (int)batch.vertices.size() - span.start;

    batch.runs_dirty = true;

    if ((span.sector && span.sector->height_sector) || (span.back_sector && span.back_sector->height_sector))
        batch.height_sensitive = true;

    SpanReference ref;

    ref.batch = capture_batch;
    ref.span  = (int)batch.spans.size();

    batch.spans.push_back(span);

    for (int i = 0; i < capture_dependency_count; i++)
        sector_spans[capture_dependencies[i]].push_back(ref);

    if (span.hash_key != 0)
        region_surface_baked[span.hash_key] = 1;
    else
        MarkBakedSlot(span, 1);
}

static std::vector<Sector *> dynamic_sector_list;
static std::vector<uint8_t>  dynamic_sector_mark;

static void NoteDynamicSector(Sector *sec)
{
    size_t index = (size_t)(sec - level_sectors);

    if ((int)dynamic_sector_mark.size() != total_level_sectors)
        dynamic_sector_mark.assign((size_t)total_level_sectors, 0);

    if (index >= dynamic_sector_mark.size() || dynamic_sector_mark[index])
        return;

    dynamic_sector_mark[index] = 1;

    dynamic_sector_list.push_back(sec);
}

void StaticPruneDynamicSectors(void)
{
    size_t keep = 0;

    for (size_t i = 0; i < dynamic_sector_list.size(); i++)
    {
        Sector *sec = dynamic_sector_list[i];

        if (sec->bake_dynamic || sec->movement_suppressed)
        {
            dynamic_sector_list[keep++] = sec;
            continue;
        }

        dynamic_sector_mark[(size_t)(sec - level_sectors)] = 0;

        MarkSectorNeighboursPending(sec);
    }

    dynamic_sector_list.resize(keep);
}

int StaticDynamicSectorCount(void)
{
    return (int)dynamic_sector_list.size();
}

Sector *StaticDynamicSector(int index)
{
    if (index < 0 || index >= (int)dynamic_sector_list.size())
        return nullptr;

    return dynamic_sector_list[(size_t)index];
}

void StaticMeshInvalidateSector(Sector *sec)
{
    if (!static_mesh_built || !sec)
        return;

    NoteDynamicSector(sec);

    StaticMarkSectorPending(sec);

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

        batch.runs_dirty = true;

        StaticMarkSectorPending(span.sector);

        if (span.hash_key != 0)
        {
            region_surface_baked.erase(span.hash_key);
            continue;
        }

        MarkBakedSlot(span, 0);
    }

    refs.clear();
}

void StaticCaptureEnd(void)
{
    capture_batch        = -1;
    capture_hash_key     = 0;
    capture_line_side    = nullptr;
    capture_surf         = nullptr;
    capture_sector       = nullptr;
    capture_light_sector = nullptr;
}

static void RefreshStaticLighting(void)
{
    if (sector_brightness_correction.d_ != static_light_correction)
    {
        static_light_correction = sector_brightness_correction.d_;

        for (size_t i = 0; i < sector_light_cache.size(); i++)
            sector_light_cache[i] = INT_MIN;
    }

    static_sector_changes.clear();

    for (size_t i = 0; i < sector_light_cache.size(); i++)
    {
        Sector *sec = level_sectors + i;

        int current = sec->properties.light_level;

        RGBAColor       fog_color   = sec->properties.fog_color;
        float           fog_density = sec->properties.fog_density;
        const Colormap *colormap    = sec->properties.colourmap;

        bool appearance = fog_color != sector_fog_color_cache[i] ||
                          !epi::AlmostEquals(fog_density, sector_fog_density_cache[i]) ||
                          colormap != sector_colormap_cache[i];

        if (current == sector_light_cache[i] && !appearance)
            continue;

        StaticSectorChange change;

        change.sector     = (int)i;
        change.appearance = appearance;

        static_sector_changes.push_back(change);

        sector_fog_color_cache[i]   = fog_color;
        sector_fog_density_cache[i] = fog_density;
        sector_colormap_cache[i]    = colormap;

        if (current == sector_light_cache[i])
            continue;

        sector_light_cache[i] = current;

        const std::vector<SpanReference> &refs = sector_spans[i];

        for (size_t r = 0; r < refs.size(); r++)
        {
            StaticBatch &batch = static_batches[refs[r].batch];
            StaticSpan  &span  = batch.spans[refs[r].span];

            if (!span.live || span.light_sector != sec)
                continue;

            float light = StaticLightRow(current, span.light_adjust);

            for (int v = span.start; v < span.start + span.count; v++)
                batch.vertices[v].texture_coordinates[1].Y = light;

            batch.dirty_low  = HMM_MIN(batch.dirty_low, span.start);
            batch.dirty_high = HMM_MAX(batch.dirty_high, span.start + span.count);
        }
    }
}

const std::vector<StaticSectorChange> &StaticSectorChanges(void)
{
    return static_sector_changes;
}

void SnapshotSurfaceBaseOffsets(void)
{
    for (int i = 0; i < total_level_sides; i++)
    {
        level_sides[i].top.base_offset    = level_sides[i].top.offset;
        level_sides[i].middle.base_offset = level_sides[i].middle.offset;
        level_sides[i].bottom.base_offset = level_sides[i].bottom.offset;
    }

    for (int i = 0; i < total_level_sectors; i++)
    {
        level_sectors[i].floor.base_offset   = level_sectors[i].floor.offset;
        level_sectors[i].ceiling.base_offset = level_sectors[i].ceiling.offset;
    }
}

void BuildStaticMesh(void)
{
    DestroyStaticMesh();

    BuildPropertiesOwnerMap();

    dynamic_sector_list.clear();
    dynamic_sector_mark.assign((size_t)total_level_sectors, 0);

    for (int i = 0; i < total_level_sectors; i++)
    {
        if (level_sectors[i].bake_dynamic || level_sectors[i].movement_suppressed)
            NoteDynamicSector(level_sectors + i);
    }

    sector_flat_baked.assign((size_t)total_level_sectors * 2 * kHeightKeyTotal, 0);
    line_side_wall_baked.assign((size_t)total_level_lines * 2 * 3 * kHeightKeyTotal, 0);

    region_surface_baked.clear();
    sector_spans.assign((size_t)total_level_sectors, std::vector<SpanReference>());

    sector_height_state.assign((size_t)total_level_sectors, 0);

    sector_light_cache.resize((size_t)total_level_sectors);

    for (int i = 0; i < total_level_sectors; i++)
        sector_light_cache[i] = level_sectors[i].properties.light_level;

    sector_fog_color_cache.resize((size_t)total_level_sectors);
    sector_fog_density_cache.resize((size_t)total_level_sectors);
    sector_colormap_cache.resize((size_t)total_level_sectors);

    for (int i = 0; i < total_level_sectors; i++)
    {
        sector_fog_color_cache[i]   = level_sectors[i].properties.fog_color;
        sector_fog_density_cache[i] = level_sectors[i].properties.fog_density;
        sector_colormap_cache[i]    = level_sectors[i].properties.colourmap;
    }

    static_sector_changes.clear();

    static_light_correction = sector_brightness_correction.d_;

    sector_bake_clean.assign((size_t)total_level_sectors, 0);
    sector_bake_pending.assign((size_t)total_level_sectors, 0);
    sector_bake_epoch.assign((size_t)total_level_sectors, 0);
    sector_pending_list.clear();

    static_mesh_built = true;

    for (int i = 0; i < total_level_sectors; i++)
        StaticMarkSectorPending(level_sectors + i);

}

void DestroyStaticMesh(void)
{
    for (size_t i = 0; i < static_batches.size(); i++)
    {
        if (static_batches[i].gpu_handle)
            DeleteStaticVertexBuffer(static_batches[i].gpu_handle);
    }

    static_batches.clear();
    sector_flat_baked.clear();
    line_side_wall_baked.clear();
    region_surface_baked.clear();
    sector_spans.clear();
    sector_light_cache.clear();
    sector_fog_color_cache.clear();
    sector_fog_density_cache.clear();
    sector_colormap_cache.clear();
    static_sector_changes.clear();
    sector_bake_clean.clear();
    sector_bake_pending.clear();
    sector_bake_epoch.clear();
    sector_pending_list.clear();

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

static void UploadStaticBatch(StaticBatch &batch)
{
    int total = (int)batch.vertices.size();

    if (total == 0)
        return;

    if (!batch.gpu_handle || total > batch.gpu_capacity)
    {
        EDGE_ZoneScopedN("StaticMesh upload");

        if (batch.gpu_handle)
            DeleteStaticVertexBuffer(batch.gpu_handle);

        int capacity = HMM_MAX(total + total / 2, 4096);

        batch.gpu_handle   = CreateStaticVertexBufferWithCapacity(batch.vertices.data(), total, capacity);
        batch.gpu_capacity = batch.gpu_handle ? capacity : 0;
        batch.gpu_count    = batch.gpu_handle ? total : 0;
        batch.dirty_low    = INT_MAX;
        batch.dirty_high   = -1;

        return;
    }

    if (total > batch.gpu_count)
    {
        EDGE_ZoneScopedN("StaticMesh append");

        UpdateStaticVertexBuffer(batch.gpu_handle, batch.gpu_count, batch.vertices.data() + batch.gpu_count,
                                 total - batch.gpu_count);

        batch.gpu_count = total;
    }

    if (batch.dirty_high > batch.dirty_low)
    {
        EDGE_ZoneScopedN("StaticMesh range update");

        int low  = batch.dirty_low;
        int high = HMM_MIN(batch.dirty_high, batch.gpu_count);

        if (high > low)
            UpdateStaticVertexBuffer(batch.gpu_handle, low, batch.vertices.data() + low, high - low);

        batch.dirty_low  = INT_MAX;
        batch.dirty_high = -1;
    }
}

static void RebuildStaticRuns(StaticBatch &batch)
{
    batch.runs.clear();

    int run_start = -1;
    int run_end   = -1;

    for (size_t k = 0; k < batch.spans.size(); k++)
    {
        const StaticSpan &span = batch.spans[k];

        bool live = span.live && span.height_key == StaticHeightKey(span.sector, span.back_sector);

        if (!live)
            continue;

        if (run_start >= 0 && span.start == run_end)
        {
            run_end += span.count;
            continue;
        }

        if (run_start >= 0)
            batch.runs.push_back(StaticRun{run_start, run_end - run_start});

        run_start = span.start;
        run_end   = span.start + span.count;
    }

    if (run_start >= 0)
        batch.runs.push_back(StaticRun{run_start, run_end - run_start});

    batch.runs_dirty = false;
}

void DrawStaticMesh(OitPass draw_pass, bool refresh)
{
    if (!static_mesh_built)
        return;

    EDGE_ZoneScoped;

    if (refresh && draw_pass == kOitPassNone)
    {
        EDGE_ZoneScopedN("StaticMesh RefreshLighting");


        StaticPruneDynamicSectors();

        if (RefreshSectorHeightStates())
        {
            for (size_t i = 0; i < static_batches.size(); i++)
            {
                if (static_batches[i].height_sensitive)
                    static_batches[i].runs_dirty = true;
            }
        }


        RefreshStaticLighting();

        if (r_static_mesh_resident.d_ != 0)
        {
            for (size_t i = 0; i < static_batches.size(); i++)
            {
                if (!static_batches[i].spans.empty())
                    UploadStaticBatch(static_batches[i]);
            }

            FlushStaticVertexUploads();
        }

    }

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &batch = static_batches[i];

        bool wanted = (batch.draw_pass == draw_pass) ||
                      (batch.draw_pass == kOitPassAccumulate && draw_pass == kOitPassRevealage);

        if (batch.spans.empty() || !wanted)
            continue;


        GLuint tex_id = ImageCache(batch.image, true);

        static_batch_texture_offset = BatchScrollOffset(batch);

        int extra_light = render_view_extra_light;

        if (batch.properties->colourmap && (batch.properties->colourmap->special_ & kColorSpecialNoFlash) &&
            extra_light <= 250)
            extra_light = 0;

        static_batch_light_row_offset = (float)(extra_light / 4) / 64.0f;

        render_unit_liquid = LiquidShaderParameters(batch.image, LiquidLevelSeconds());

        AbstractShader *shader = GetColormapShader(batch.properties, 0, batch.sector);

        bool resident = r_static_mesh_resident.d_ != 0;

        if (resident && !batch.gpu_handle)
            continue;

        if (batch.runs_dirty)
            RebuildStaticRuns(batch);

        int pass = 0;

        BlendingMode blending = batch.blending;

        bool inverted   = mirror_view.reflective != (fliplevels.d_ != 0);
        bool cull_front = (batch.face_dir > 0) != inverted;

        blending = (BlendingMode)(blending | (cull_front ? kBlendingCullFront : kBlendingCullBack));

        for (size_t r = 0; r < batch.runs.size(); r++)
        {
            const StaticRun &run = batch.runs[r];

            if (resident)
            {
                int count = HMM_MIN(run.count, batch.gpu_count - run.start);

                if (count > 0)
                    shader->WorldBakedResident(batch.gpu_handle, GL_TRIANGLES, run.start, count, tex_id, &pass,
                                               blending);
                continue;
            }

            for (int offset = 0; offset < run.count; offset += (int)kMaximumStaticRun)
            {
                int count = HMM_MIN((int)kMaximumStaticRun, run.count - offset);

                shader->WorldBaked(GL_TRIANGLES, batch.vertices.data() + run.start + offset, count, tex_id, 1.0f,
                                   &pass, blending);
            }
        }
    }

    render_unit_liquid = {{0, 0, 0, 0}};

    static_batch_light_row_offset = 0;
}
