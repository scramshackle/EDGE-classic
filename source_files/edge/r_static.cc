#include "r_static.h"
#include "r_backend.h"
#include "r_misc.h"

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
#include "r_misc.h"
#include "r_shader.h"
#include "r_state.h"
#include "r_units.h"

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
    bool    covers_sector;
    bool    covers_line;

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
    OitPass           draw_pass;

    const MapSurface *scroll_surface;
    HMM_Vec2          uv_scale;
    float             liquid_amplitude;

    std::vector<RendererVertex> vertices;
    std::vector<StaticSpan>     spans;

    uint32_t gpu_handle = 0;
    bool     gpu_dirty  = true;
};

static std::vector<StaticBatch> static_batches;
static std::vector<uint8_t>     subsector_flat_baked;
static std::vector<uint8_t>     seg_wall_baked;
static std::unordered_map<uint64_t, uint8_t> region_surface_baked;
static bool                     static_mesh_built = false;

static std::vector<std::vector<SpanReference>> sector_spans;
static std::vector<int>                       sector_light_cache;
static std::vector<uint8_t>                   sector_height_state;
static int                                    swirl_cache = -1;

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

static void RefreshSectorHeightStates(void)
{
    for (int i = 0; i < total_level_sectors; i++)
        sector_height_state[i] = (uint8_t)SectorHeightState(level_sectors + i);
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

static float capture_liquid_amplitude = 0.0f;

static void SetCaptureScrollOffset(const MapSurface *surf, const HMM_Vec2 &uv_scale)
{
    capture_liquid_amplitude = LiquidTurbulenceAmplitude();

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
static bool capture_covers_sector = false;
static bool capture_covers_line   = false;

static std::vector<int> line_side_starts;
static std::vector<int> line_side_segs;
static HMM_Vec3 capture_normal = {{0, 0, 1}};
static float capture_div[4]    = {0, 0, 0, 0};
static int  capture_light      = 0;
static Sector *capture_light_sector = nullptr;
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

static int ExtrafloorIndex(const Extrafloor *ef)
{
    if (!ef || !level_extrafloors)
        return -1;

    return (int)(ef - level_extrafloors);
}

static bool BuildRegionWallKey(const Seg *seg, int part, const Extrafloor *region_ef, const Extrafloor *surface_ef,
                               int height_key, uint64_t *out)
{
    uint64_t index = (uint64_t)(seg - level_segs);

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

static bool BuildRegionFlatKey(const Subsector *sub, int face_dir, const Extrafloor *plane_ef, int height_key,
                               uint64_t *out)
{
    uint64_t index = (uint64_t)(sub - level_subsectors);

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

int StaticExtrafloorPlaneDecline(const Subsector *sub, const Extrafloor *plane_ef, int face_dir)
{
    if (!static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!sub || !plane_ef)
    {
        return kStaticBakeNoSurface;
    }

    int sector_reason = SectorDeclineReason(sub->sector, false);

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

    if (!BuildRegionFlatKey(sub, face_dir, plane_ef, LiveHeightKey(sub->sector, nullptr), &key))
    {
        return kStaticBakeKeyOverflow;
    }

    return kStaticBakeAccepted;
}

bool StaticExtrafloorPlaneEligible(const Subsector *sub, const Extrafloor *plane_ef, int face_dir)
{
    return StaticExtrafloorPlaneDecline(sub, plane_ef, face_dir) == kStaticBakeAccepted;
}
bool StaticFlatBakeEligible(const Sector *sec, int face_dir)
{
    return StaticFlatBakeDecline(sec, face_dir) == kStaticBakeAccepted;
}

bool StaticMeshCoversFlat(const Subsector *sub, int face_dir, const Extrafloor *plane_ef)
{
    if (!static_mesh_built || !sub)
        return false;

    int key = LiveHeightKey(sub->sector, nullptr);

    if (plane_ef)
    {
        uint64_t hash_key;

        if (!BuildRegionFlatKey(sub, face_dir, plane_ef, key, &hash_key))
            return false;

        return region_surface_baked.find(hash_key) != region_surface_baked.end();
    }

    size_t slot = ((size_t)(sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1)) * kHeightKeyTotal + (size_t)key;

    if (slot >= subsector_flat_baked.size())
        return false;

    return subsector_flat_baked[slot] != 0;
}

static int FindBatch(const Image *image, const Colormap *colormap, RegionProperties *props, Sector *sec,
                     BlendingMode blending, OitPass draw_pass, const MapSurface *scroll_surf)
{
    const MapSurface *scroller = SurfaceScrolls(scroll_surf) ? scroll_surf : nullptr;

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &b = static_batches[i];

        if (b.image == image && b.colormap == colormap && b.blending == blending && b.draw_pass == draw_pass &&
            b.scroll_surface == scroller &&
            epi::AlmostEquals(b.liquid_amplitude, capture_liquid_amplitude) &&
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

    batch.uv_scale         = capture_uv_scale;
    batch.liquid_amplitude = capture_liquid_amplitude;
    batch.scroll_surface   = scroller;

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

int StaticWallBakeDecline(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef)
{
    if (!static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!seg || !surf)
    {
        return kStaticBakeNoSurface;
    }

    if (seg->miniseg || !seg->sidedef)
    {
        return kStaticBakeMiniseg;
    }

    if (mid_masked && seg->linedef && seg->linedef->special && seg->linedef->special->glass_)
    {
        return kStaticBakeGlass;
    }

    if (WallPartIndex(seg, surf) < 0 && !surface_ef)
    {
        return kStaticBakeNotSidedefPart;
    }

    if (seg->linedef && (seg->linedef->flags & kLineFlagMirror))
    {
        return kStaticBakeMirrorLine;
    }

    if (seg->linedef && seg->linedef->portal_pair)
    {
        return kStaticBakePortalLine;
    }

    if (seg->linedef && seg->linedef->slide_door)
    {
        return kStaticBakeSlideDoor;
    }

    const Side *side = seg->sidedef;

    if (side->bake_dynamic)
    {
        return kStaticBakeSideDynamic;
    }

    const Sector *front = seg->front_sector;
    const Sector *back  = seg->back_sector;

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

    if (!SectorListStatic(seg->vertex_sectors[0]) || !SectorListStatic(seg->vertex_sectors[1]))
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

    if (surf->override_properties && !StaticPropertiesResolvable(seg->front_sector, surf->override_properties))
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

        if (!BuildRegionWallKey(seg, WallPartIndex(seg, surf), region_ef, surface_ef, LiveHeightKey(front, back), &key))
        {
            return kStaticBakeKeyOverflow;
        }
    }

    return kStaticBakeAccepted;
}

bool StaticWallBakeEligible(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                            const Extrafloor *surface_ef)
{
    return StaticWallBakeDecline(seg, surf, mid_masked, region_ef, surface_ef) == kStaticBakeAccepted;
}

static const char *const static_bake_decline_names[kStaticBakeDeclineTotal] = {"resident",
                                                                              "mesh disabled",
                                                                              "no surface",
                                                                              "miniseg",
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



bool StaticMeshCoversSubsector(const Subsector *sub)
{
    if (!static_mesh_built || !sub)
        return false;

    const Sector *sec = sub->sector;

    if (!sec || sec->extrafloor_used > 0 || sec->height_sector || sub->deep_water_reference)
        return false;

    if (EDGE_IMAGE_IS_SKY(sec->ceiling) || EDGE_IMAGE_IS_SKY(sec->floor))
        return false;

    int key = LiveHeightKey(sec, nullptr);

    size_t base = (size_t)(sub - level_subsectors) * 2;

    for (size_t face = 0; face < 2; face++)
    {
        size_t slot = (base + face) * kHeightKeyTotal + (size_t)key;

        if (slot >= subsector_flat_baked.size() || !subsector_flat_baked[slot])
            return false;
    }

    for (const Seg *seg = sub->segs; seg; seg = seg->subsector_next)
    {
        if (seg->miniseg || !seg->sidedef || !seg->linedef)
            continue;

        const Sector *front = seg->front_sector;
        const Sector *back  = seg->back_sector;

        if (!front)
            return false;

        if (back && (back->extrafloor_used > 0 || back->height_sector))
            return false;

        if (EDGE_IMAGE_IS_SKY(front->ceiling) || EDGE_IMAGE_IS_SKY(front->floor))
            return false;

        if (back && (EDGE_IMAGE_IS_SKY(back->ceiling) || EDGE_IMAGE_IS_SKY(back->floor)))
            return false;

        int wall_key = LiveHeightKey(front, back);

        const MapSurface *parts[3] = {&seg->sidedef->bottom, &seg->sidedef->middle, &seg->sidedef->top};

        for (size_t part = 0; part < 3; part++)
        {
            if (!parts[part]->image)
                continue;

            size_t slot = ((size_t)(seg - level_segs) * 3 + part) * kHeightKeyTotal + (size_t)wall_key;

            if (slot >= seg_wall_baked.size() || !seg_wall_baked[slot])
                return false;
        }
    }

    return true;
}

bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef)
{
    if (!static_mesh_built || !seg)
        return false;

    int part = WallPartIndex(seg, surf);

    if (part < 0 && !surface_ef)
        return false;

    int key = LiveHeightKey(seg->front_sector, seg->back_sector);

    if (region_ef || surface_ef)
    {
        uint64_t hash_key;

        if (!BuildRegionWallKey(seg, part, region_ef, surface_ef, key, &hash_key))
            return false;

        return region_surface_baked.find(hash_key) != region_surface_baked.end();
    }

    size_t slot = ((size_t)(seg - level_segs) * 3 + (size_t)part) * kHeightKeyTotal + (size_t)key;

    if (slot >= seg_wall_baked.size())
        return false;

    return seg_wall_baked[slot] != 0;
}

static void BuildLineSegIndex(void)
{
    line_side_starts.clear();
    line_side_segs.clear();

    if (total_level_lines <= 0)
        return;

    size_t keys = (size_t)total_level_lines * 2;

    line_side_starts.assign(keys + 1, 0);

    for (int i = 0; i < total_level_segs; i++)
    {
        const Seg *seg = level_segs + i;

        if (seg->miniseg || !seg->linedef)
            continue;

        size_t key = (size_t)(seg->linedef - level_lines) * 2 + (size_t)seg->side;

        if (key < keys)
            line_side_starts[key + 1]++;
    }

    for (size_t i = 1; i < line_side_starts.size(); i++)
        line_side_starts[i] += line_side_starts[i - 1];

    line_side_segs.assign((size_t)line_side_starts.back(), 0);

    std::vector<int> cursor(line_side_starts.begin(), line_side_starts.end() - 1);

    for (int i = 0; i < total_level_segs; i++)
    {
        const Seg *seg = level_segs + i;

        if (seg->miniseg || !seg->linedef)
            continue;

        size_t key = (size_t)(seg->linedef - level_lines) * 2 + (size_t)seg->side;

        if (key < keys)
            line_side_segs[(size_t)cursor[key]++] = i;
    }
}

bool StaticWallCoversLine(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef)
{
    if (!static_mesh_built)
        return false;

    if (region_ef || surface_ef)
        return false;

    if (!seg || seg->miniseg || !seg->linedef || !seg->sidedef)
        return false;

    size_t key = (size_t)(seg->linedef - level_lines) * 2 + (size_t)seg->side;

    if (key + 1 >= line_side_starts.size())
        return false;

    int first = line_side_starts[key];
    int last  = line_side_starts[key + 1];

    if (last - first < 1)
        return false;

    for (int i = first; i < last; i++)
    {
        const Seg *other = level_segs + line_side_segs[i];

        if (!StaticWallBakeEligible(other, surf, mid_masked, region_ef, surface_ef))
            return false;
    }

    return true;
}

void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale, const Extrafloor *region_ef,
                        const Extrafloor *surface_ef, bool covers_line)
{
    SetCaptureScrollOffset(surf, uv_scale);

    capture_mid_masked    = mid_masked;
    capture_covers_sector = false;
    capture_covers_line   = covers_line;

    capture_normal = normal;
    capture_div[0] = div_x;
    capture_div[1] = div_y;
    capture_div[2] = div_delta_x;
    capture_div[3] = div_delta_y;

    capture_back_sector = seg ? seg->back_sector : nullptr;
    capture_height_key  = LiveHeightKey(seg ? seg->front_sector : sector, capture_back_sector);

    capture_batch  = FindBatch(image, props->colourmap, props, sector, blending, draw_pass, surf);
    capture_seg    = seg;
    capture_surf   = surf;
    capture_sector = sector;
    capture_adjust = light_adjust;

    capture_light_sector = ResolvePropertiesOwner(sector, props);
    capture_light        = capture_light_sector->properties.light_level;

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
    AddCaptureDependency(sector->height_sector);
    AddCaptureDependency(capture_light_sector);

    if (seg)
    {
        capture_is_wall = true;

        int part = WallPartIndex(seg, surf);

        capture_flag_slot = (part < 0) ? -1 : (int)((seg - level_segs) * 3 + part);
        capture_hash_key  = 0;

        if (region_ef || surface_ef)
        {
            capture_flag_slot = -1;

            if (!BuildRegionWallKey(seg, part, region_ef, surface_ef, capture_height_key, &capture_hash_key))
                capture_hash_key = 0;
        }

        AddCaptureDependency(ExtrafloorControlSector(region_ef));
        AddCaptureDependency(ExtrafloorControlSector(surface_ef));

        AddCaptureDependency(seg->front_sector);
        AddCaptureDependency(seg->back_sector);

        if (seg->front_sector)
            AddCaptureDependency(seg->front_sector->height_sector);

        if (seg->back_sector)
            AddCaptureDependency(seg->back_sector->height_sector);
        AddCaptureDependencyList(seg->vertex_sectors[0]);
        AddCaptureDependencyList(seg->vertex_sectors[1]);
    }
}

void StaticCaptureBeginFlat(const Subsector *sub, int face_dir, const Image *image, RegionProperties *props,
                            Sector *sector, BlendingMode blending, const HMM_Vec3 &normal, OitPass draw_pass,
                            const MapSurface *surf, const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef,
                            bool covers_sector)
{
    capture_flat_surface = surf;

    SetCaptureScrollOffset(surf, uv_scale);

    capture_normal = normal;
    capture_div[0] = capture_div[1] = capture_div[2] = capture_div[3] = 0;

    capture_back_sector = nullptr;
    capture_height_key  = LiveHeightKey(sector, nullptr);

    capture_batch  = FindBatch(image, props->colourmap, props, sector, blending, draw_pass, capture_flat_surface);
    capture_seg    = nullptr;
    capture_surf   = nullptr;
    capture_sector = sector;
    capture_adjust = 0;

    capture_light_sector = ResolvePropertiesOwner(sector, props);
    capture_light        = capture_light_sector->properties.light_level;

    capture_is_wall       = false;
    capture_mid_masked    = false;
    capture_covers_line   = false;
    capture_covers_sector = covers_sector;
    capture_flag_slot = (int)((sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1));
    capture_hash_key  = 0;

    if (plane_ef)
    {
        capture_flag_slot     = -1;
        capture_covers_sector = false;

        if (!BuildRegionFlatKey(sub, face_dir, plane_ef, capture_height_key, &capture_hash_key))
            capture_hash_key = 0;
    }

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
    AddCaptureDependency(sector->height_sector);
    AddCaptureDependency(sub->deep_water_reference);
    AddCaptureDependency(capture_light_sector);
    AddCaptureDependency(ExtrafloorControlSector(plane_ef));
}

static void MarkLineWallSlots(const StaticSpan &span, uint8_t value)
{
    if (span.flag_slot < 0)
        return;

    int part      = span.flag_slot % 3;
    int seg_index = span.flag_slot / 3;

    if (seg_index < 0 || seg_index >= total_level_segs)
        return;

    const Seg *seg = level_segs + seg_index;

    if (!seg->linedef)
        return;

    size_t key = (size_t)(seg->linedef - level_lines) * 2 + (size_t)seg->side;

    if (key + 1 >= line_side_starts.size())
        return;

    for (int i = line_side_starts[key]; i < line_side_starts[key + 1]; i++)
    {
        size_t slot = ((size_t)line_side_segs[i] * 3 + (size_t)part) * kHeightKeyTotal + (size_t)span.height_key;

        if (slot < seg_wall_baked.size())
            seg_wall_baked[slot] = value;
    }
}

static void MarkSectorFlatSlots(const StaticSpan &span, uint8_t value)
{
    if (span.flag_slot < 0 || !span.sector)
        return;

    size_t parity = (size_t)(span.flag_slot & 1);

    size_t own = (size_t)span.flag_slot * kHeightKeyTotal + (size_t)span.height_key;

    if (own < subsector_flat_baked.size())
        subsector_flat_baked[own] = value;

    for (const Subsector *sub = span.sector->subsectors; sub; sub = sub->sector_next)
    {
        size_t slot = ((size_t)(sub - level_subsectors) * 2 + parity) * kHeightKeyTotal + (size_t)span.height_key;

        if (slot < subsector_flat_baked.size())
            subsector_flat_baked[slot] = value;
    }
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
    span.is_wall       = capture_is_wall;
    span.mid_masked    = capture_mid_masked;
    span.covers_sector = capture_covers_sector;
    span.covers_line   = capture_covers_line;
    span.live          = true;
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

    for (int v = span.start; v < (int)batch.vertices.size(); v++)
    {
        RendererVertex &dest = batch.vertices[v];

        dest.rgba = epi::MakeRGBA(255, 255, 255, epi::GetRGBAAlpha(dest.rgba));

        dest.texture_coordinates[0].X -= capture_scroll_uv.X;
        dest.texture_coordinates[0].Y -= capture_scroll_uv.Y;

        if (!epi::AlmostEquals(capture_liquid_amplitude, 0.0f))
        {
            HMM_Vec2 turbulence;

            LiquidTurbulenceDelta(dest.position, &turbulence);

            dest.texture_coordinates[0].X -= turbulence.X;
            dest.texture_coordinates[0].Y -= turbulence.Y;
        }
    }

    span.count = (int)batch.vertices.size() - span.start;

    batch.gpu_dirty = true;

    SpanReference ref;

    ref.batch = capture_batch;
    ref.span  = (int)batch.spans.size();

    batch.spans.push_back(span);

    for (int i = 0; i < capture_dependency_count; i++)
        sector_spans[capture_dependencies[i]].push_back(ref);

    if (span.hash_key != 0)
    {
        region_surface_baked[span.hash_key] = 1;
    }
    else if (span.flag_slot >= 0)
    {
        size_t slot = (size_t)span.flag_slot * kHeightKeyTotal + (size_t)span.height_key;

        if (span.is_wall)
        {
            if (span.covers_line)
                MarkLineWallSlots(span, 1);
            else if (slot < seg_wall_baked.size())
                seg_wall_baked[slot] = 1;
        }
        else if (span.covers_sector)
            MarkSectorFlatSlots(span, 1);
        else if (slot < subsector_flat_baked.size())
            subsector_flat_baked[slot] = 1;
    }
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

        if (span.hash_key != 0)
        {
            region_surface_baked.erase(span.hash_key);
            continue;
        }

        if (span.flag_slot < 0)
            continue;

        size_t slot = (size_t)span.flag_slot * kHeightKeyTotal + (size_t)span.height_key;

        if (span.is_wall)
        {
            if (span.covers_line)
                MarkLineWallSlots(span, 0);
            else if (slot < seg_wall_baked.size())
                seg_wall_baked[slot] = 0;
        }
        else if (span.covers_sector)
            MarkSectorFlatSlots(span, 0);
        else if (slot < subsector_flat_baked.size())
            subsector_flat_baked[slot] = 0;
    }

    refs.clear();
}

void StaticCaptureEnd(void)
{
    capture_batch        = -1;
    capture_hash_key     = 0;
    capture_seg          = nullptr;
    capture_surf         = nullptr;
    capture_sector       = nullptr;
    capture_light_sector = nullptr;
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

            if (!span.live || span.light_sector != sec)
                continue;

            float light = ((float)((current + span.light_adjust) / 4) + 0.5f) / 64.0f;

            for (int v = span.start; v < span.start + span.count; v++)
                batch.vertices[v].texture_coordinates[1].Y = light;

            batch.gpu_dirty = true;
        }
    }
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

    subsector_flat_baked.assign((size_t)total_level_subsectors * 2 * kHeightKeyTotal, 0);
    seg_wall_baked.assign((size_t)total_level_segs * 3 * kHeightKeyTotal, 0);

    BuildLineSegIndex();
    region_surface_baked.clear();
    sector_spans.assign((size_t)total_level_sectors, std::vector<SpanReference>());

    sector_height_state.assign((size_t)total_level_sectors, 0);

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
    line_side_starts.clear();
    line_side_segs.clear();
    subsector_flat_baked.clear();
    seg_wall_baked.clear();
    region_surface_baked.clear();
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

void DrawStaticMesh(OitPass draw_pass, bool refresh)
{
    if (!static_mesh_built)
        return;

    EDGE_ZoneScoped;

    if (refresh && draw_pass == kOitPassNone && swirl_cache != (int)swirling_flats)
    {
        swirl_cache = (int)swirling_flats;

        DestroyStaticMesh();
        BuildStaticMesh();

        return;
    }

    if (refresh && draw_pass == kOitPassNone)
    {
        EDGE_ZoneScopedN("StaticMesh RefreshLighting");


        StaticPruneDynamicSectors();

        RefreshSectorHeightStates();


        RefreshStaticLighting();

    }

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &batch = static_batches[i];

        bool wanted = (batch.draw_pass == draw_pass) ||
                      (batch.draw_pass == kOitPassAccumulate && draw_pass == kOitPassRevealage);

        if (batch.spans.empty() || !wanted)
            continue;


        GLuint tex_id = ImageCache(batch.image, true, render_view_effect_colormap);

        static_batch_texture_offset = BatchScrollOffset(batch);

        static_batch_liquid = {{batch.liquid_amplitude, LiquidTurbulenceWave()}};

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

            if (live && batch.spans[k].height_key !=
                            StaticHeightKey(batch.spans[k].sector, batch.spans[k].back_sector))
            {

                live = false;
            }

            bool contiguous = live && run_start >= 0 && batch.spans[k].start == run_end &&
                              (size_t)(run_end + batch.spans[k].count - run_start) <= kMaximumStaticRun;

            if (live)

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


    }
}
