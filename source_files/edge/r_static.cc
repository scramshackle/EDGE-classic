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

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_mesh, "1", kConsoleVariableFlagArchive)

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_mesh_resident, "1", kConsoleVariableFlagArchive)

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_residency, "0", kConsoleVariableFlagNone)

EDGE_DEFINE_CONSOLE_VARIABLE(r_static_survey, "0", kConsoleVariableFlagNone)

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

    return sec;
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
static bool capture_is_wall    = false;
static bool capture_mid_masked = false;
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

bool StaticMeshEnabled(void)
{
    return r_static_mesh.d_ != 0;
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

    if (surf.override_properties || surf.boom_colormap)
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

int StaticFlatBakeDecline(const Sector *sec, int face_dir)
{
    if (!r_static_mesh.d_ || !static_mesh_built)
    {
        return kStaticBakeMeshDisabled;
    }

    if (!sec)
    {
        return kStaticBakeNoSurface;
    }

    int sector_reason = SectorDeclineReason(sec, false);

    if (sector_reason != kStaticBakeAccepted)
    {
        return sector_reason;
    }

    return FlatSurfaceDecline((face_dir > 0) ? sec->floor : sec->ceiling, sec, face_dir);
}

static Sector *ExtrafloorControlSector(const Extrafloor *ef)
{
    if (!ef || !ef->extrafloor_line)
        return nullptr;

    return ef->extrafloor_line->front_sector;
}

int StaticExtrafloorPlaneDecline(const Subsector *sub, const Extrafloor *plane_ef, int face_dir)
{
    if (!r_static_mesh.d_ || !static_mesh_built)
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
    if (!r_static_mesh.d_ || !static_mesh_built || !sub)
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
    if (!r_static_mesh.d_ || !static_mesh_built)
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

    if (surf->override_properties || surf->boom_colormap)
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

static std::vector<uint8_t> residency_flat_state;
static std::vector<uint8_t> residency_wall_state;
static std::vector<uint8_t> residency_extra_flat;
static std::vector<uint8_t> residency_extra_wall;
static std::vector<uint8_t> residency_frame_flat;
static std::vector<uint8_t> residency_frame_wall;
static int                  residency_report_countdown = 0;
static int                  residency_frame_number      = 0;
static int                  frame_span_height_skipped  = 0;
static uint64_t             frame_time_height_state    = 0;
static uint64_t             frame_time_light_refresh   = 0;
static uint64_t             frame_time_span_runs       = 0;
static uint64_t             frame_time_span_lights     = 0;
static int                  frame_span_runs            = 0;
static int                  frame_span_live            = 0;
static int                  frame_batches_drawn        = 0;
static int                  frame_flat_resident        = 0;
static int                  frame_flat_dynamic         = 0;
static int                  frame_flat_extra           = 0;
static int                  frame_flat_repeat          = 0;
static int                  frame_wall_resident        = 0;
static int                  frame_wall_dynamic         = 0;
static int                  frame_wall_extra           = 0;
static int                  frame_wall_repeat          = 0;
static int                  frame_wall_region          = 0;
static int                  frame_flat_extra_resident  = 0;
static int                  frame_wall_extra_resident  = 0;
static int                  frame_extra_reason[kStaticBakeDeclineTotal];
static int                  frame_light_mismatch       = 0;
static int                  frame_light_checked        = 0;
static int                  frame_light_unresolved     = 0;

static void ResidencyEnsureStorage(void)
{
    if ((int)residency_flat_state.size() != total_level_subsectors * 2)
    {
        residency_flat_state.assign((size_t)total_level_subsectors * 2, 0xFF);
        residency_extra_flat.assign((size_t)total_level_subsectors * 2, 0);
        residency_frame_flat.assign((size_t)total_level_subsectors * 2, 0);
    }

    if ((int)residency_wall_state.size() != total_level_segs * 3)
    {
        residency_wall_state.assign((size_t)total_level_segs * 3, 0xFF);
        residency_extra_wall.assign((size_t)total_level_segs, 0);
        residency_frame_wall.assign((size_t)total_level_segs * 3, 0);
    }
}

void StaticResidencyNoteFlat(const Subsector *sub, int face_dir, bool resident, bool own_plane,
                             const Extrafloor *plane_ef)
{
    if (!r_static_residency.d_ || !sub || !static_mesh_built)
        return;

    ResidencyEnsureStorage();

    size_t slot = (size_t)(sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1);

    if (slot >= residency_flat_state.size())
        return;

    if (!own_plane)
    {
        residency_extra_flat[slot] = 1;
        frame_flat_extra++;

        if (resident)
            frame_flat_extra_resident++;
        else
        {
            int reason = plane_ef ? StaticExtrafloorPlaneDecline(sub, plane_ef, face_dir) : kStaticBakeNotOwnPlane;

            if (reason >= 0 && reason < kStaticBakeDeclineTotal)
                frame_extra_reason[reason]++;
        }

        return;
    }

    if (resident)
        frame_flat_resident++;
    else
    {
        frame_flat_dynamic++;

        if (residency_frame_flat[slot])
            frame_flat_repeat++;

        residency_frame_flat[slot] = 1;
    }

    int reason = resident ? kStaticBakeAccepted : StaticFlatBakeDecline(sub->sector, face_dir);

    residency_flat_state[slot] = (uint8_t)(resident ? kStaticBakeAccepted : (reason + kStaticBakeDeclineTotal));
}

void StaticResidencyNoteWall(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                             const Extrafloor *surface_ef, bool resident)
{
    if (!r_static_residency.d_ || !seg || !static_mesh_built)
        return;

    ResidencyEnsureStorage();

    int part = WallPartIndex(seg, surf);

    if (surface_ef || part < 0)
    {
        size_t extra = (size_t)(seg - level_segs);

        if (extra < residency_extra_wall.size())
            residency_extra_wall[extra] = 1;

        frame_wall_extra++;

        if (resident)
            frame_wall_extra_resident++;
        else
        {
            int reason = StaticWallBakeDecline(seg, surf, mid_masked, region_ef, surface_ef);

            if (reason >= 0 && reason < kStaticBakeDeclineTotal)
                frame_extra_reason[reason]++;
        }

        return;
    }

    size_t slot = (size_t)(seg - level_segs) * 3 + (size_t)part;

    if (slot >= residency_wall_state.size())
        return;

    if (resident)
        frame_wall_resident++;
    else
    {
        frame_wall_dynamic++;

        if (residency_frame_wall[slot])
            frame_wall_repeat++;

        residency_frame_wall[slot] = 1;

        if (region_ef)
            frame_wall_region++;
    }

    int reason = resident ? kStaticBakeAccepted : StaticWallBakeDecline(seg, surf, mid_masked, region_ef, nullptr);

    residency_wall_state[slot] = (uint8_t)(resident ? kStaticBakeAccepted : (reason + kStaticBakeDeclineTotal));
}

void StaticResidencyNoteRegionProperties(Sector *sec, RegionProperties *props)
{
    if (!r_static_residency.d_ || !static_mesh_built || !sec || !props)
        return;

    frame_light_checked++;

    if (props == &sec->properties)
        return;

    frame_light_mismatch++;

    Sector *owner = ResolvePropertiesOwner(sec, props);

    if (owner == sec || &owner->properties != props)
        frame_light_unresolved++;
}

static void ResidencyPrintTable(const char *label, const std::vector<uint8_t> &state)
{
    int counts[kStaticBakeDeclineTotal];

    for (int i = 0; i < kStaticBakeDeclineTotal; i++)
        counts[i] = 0;

    int resident = 0;
    int seen     = 0;

    for (size_t i = 0; i < state.size(); i++)
    {
        if (state[i] == 0xFF)
            continue;

        seen++;

        if (state[i] == (uint8_t)kStaticBakeAccepted)
        {
            resident++;
            continue;
        }

        int reason = (int)state[i] - kStaticBakeDeclineTotal;

        if (reason >= 0 && reason < kStaticBakeDeclineTotal)
            counts[reason]++;
    }

    if (seen == 0)
    {
        LogPrint("  %s: nothing drawn yet\n", label);
        return;
    }

    LogPrint("  %s: %d of %d drawn surfaces resident (%.1f%%)\n", label, resident, seen,
             100.0f * (float)resident / (float)seen);

    for (int i = 0; i < kStaticBakeDeclineTotal; i++)
    {
        if (counts[i] == 0)
            continue;

        const char *name = (i == kStaticBakeAccepted) ? "eligible, not yet captured" : StaticBakeDeclineName(i);

        LogPrint("      %-32s %6d (%.1f%%)\n", name, counts[i], 100.0f * (float)counts[i] / (float)seen);
    }
}

static int ResidencyCountMarks(const std::vector<uint8_t> &marks)
{
    int total = 0;

    for (size_t i = 0; i < marks.size(); i++)
        total += marks[i] ? 1 : 0;

    return total;
}

void StaticResidencyTick(void)
{
    if (!r_static_residency.d_ || !static_mesh_built)
        return;

    residency_frame_number++;

    residency_report_countdown--;

    if (r_static_residency.d_ >= 3)
        StaticResidencyReport();
    else if (residency_report_countdown <= 0)
    {
        residency_report_countdown = 5 * kTicRate;

        if (r_static_residency.d_ >= 2)
            StaticResidencyReport();
    }

    memset(residency_frame_flat.data(), 0, residency_frame_flat.size());
    memset(residency_frame_wall.data(), 0, residency_frame_wall.size());

    frame_flat_resident = frame_flat_dynamic = frame_flat_extra = frame_flat_repeat = 0;
    frame_wall_resident = frame_wall_dynamic = frame_wall_extra = frame_wall_repeat = frame_wall_region = 0;
    frame_flat_extra_resident = frame_wall_extra_resident = 0;
    frame_span_height_skipped = frame_span_runs = frame_span_live = frame_batches_drawn = 0;
    frame_time_height_state = frame_time_light_refresh = frame_time_span_runs = frame_time_span_lights = 0;

    for (int i = 0; i < kStaticBakeDeclineTotal; i++)
        frame_extra_reason[i] = 0;
    frame_light_mismatch = frame_light_checked = frame_light_unresolved = 0;
}

void StaticResidencyReport(void)
{
    if (!static_mesh_built)
    {
        LogPrint("Static residency: no mesh built.\n");
        return;
    }

    if (!r_static_residency.d_)
    {
        LogPrint("Static residency: disabled (set r_static_residency 1 and render a frame).\n");
        return;
    }

    LogPrint("Static residency frame %d (surfaces drawn since level load, last state of each):\n",
             residency_frame_number);

    ResidencyPrintTable("flats", residency_flat_state);
    ResidencyPrintTable("walls", residency_wall_state);

    LogPrint("  last frame emitted:\n");
    LogPrint("      %-32s %6d resident-skipped, %d dynamic (%d repeat), %d extra planes (%d resident)\n", "flats",
             frame_flat_resident, frame_flat_dynamic, frame_flat_repeat, frame_flat_extra, frame_flat_extra_resident);
    LogPrint("      %-32s %6d resident-skipped, %d dynamic (%d repeat), %d extrafloor tiles (%d resident)\n", "walls",
             frame_wall_resident, frame_wall_dynamic, frame_wall_repeat, frame_wall_extra, frame_wall_extra_resident);
    LogPrint("      %-32s %6d dynamic, %d hash slots live\n", "walls in extrafloor regions", frame_wall_region,
             (int)region_surface_baked.size());

    for (int i = 0; i < kStaticBakeDeclineTotal; i++)
    {
        if (frame_extra_reason[i] == 0)
            continue;

        LogPrint("      extrafloor surface declined: %-20s %6d\n", StaticBakeDeclineName(i), frame_extra_reason[i]);
    }

    LogPrint("      %-32s %6d of %d emitted (%d unresolved)\n", "props not the sector's own",
             frame_light_mismatch, frame_light_checked, frame_light_unresolved);

    LogPrint("      %-32s %6d live in %d runs over %d batches, %d skipped by height key\n", "baked spans",
             frame_span_live, frame_span_runs, frame_batches_drawn, frame_span_height_skipped);

    LogPrint("      %-32s %6d of %d bakeable surfaces (%.1f%%)\n", "mesh coverage so far", frame_span_live,
             total_level_subsectors * 2 + total_level_segs * 3,
             100.0f * (float)frame_span_live / (float)HMM_MAX(1, total_level_subsectors * 2 + total_level_segs * 3));

    LogPrint("      %-32s heights %llu us, light refresh %llu us, span runs %llu us, span lights %llu us\n",
             "per-frame mesh loops", (unsigned long long)frame_time_height_state,
             (unsigned long long)frame_time_light_refresh, (unsigned long long)frame_time_span_runs,
             (unsigned long long)frame_time_span_lights);

    LogPrint("  never bakeable and not counted above:\n");
    LogPrint("      %-32s %6d\n", "subsector faces with extra planes", ResidencyCountMarks(residency_extra_flat));
    LogPrint("      %-32s %6d\n", "segs with extrafloor wall tiles", ResidencyCountMarks(residency_extra_wall));
}

static bool SurveyWallPartDrawn(const Seg *seg, int part)
{
    const Side *side = seg->sidedef;

    if (!side)
        return false;

    const MapSurface *surf = (part == 0) ? &side->bottom : (part == 1) ? &side->middle : &side->top;

    if (!surf->image)
        return false;

    const Sector *front = seg->front_sector;
    const Sector *back  = seg->back_sector;

    if (!back)
        return part == 1;

    if (part == 0)
        return back->floor_height > front->floor_height;

    if (part == 2)
        return back->ceiling_height < front->ceiling_height;

    return true;
}

void StaticResidencySurvey(void)
{
    if (!static_mesh_built)
        return;

    int flat_counts[kStaticBakeDeclineTotal];
    int wall_counts[kStaticBakeDeclineTotal];

    for (int i = 0; i < kStaticBakeDeclineTotal; i++)
        flat_counts[i] = wall_counts[i] = 0;

    int flat_total = 0;
    int wall_total = 0;
    int flat_sky   = 0;
    int wall_sky   = 0;

    for (int i = 0; i < total_level_subsectors; i++)
    {
        const Subsector *sub = level_subsectors + i;

        for (int face = 0; face < 2; face++)
        {
            int face_dir = (face == 0) ? 1 : -1;

            const MapSurface &surf = (face_dir > 0) ? sub->sector->floor : sub->sector->ceiling;

            if (!surf.image)
                continue;

            int reason = StaticFlatBakeDecline(sub->sector, face_dir);

            if (reason == kStaticBakeSky)
            {
                flat_sky++;
                continue;
            }

            flat_counts[reason]++;
            flat_total++;
        }
    }

    for (int i = 0; i < total_level_segs; i++)
    {
        const Seg *seg = level_segs + i;

        if (seg->miniseg || !seg->sidedef)
            continue;

        for (int part = 0; part < 3; part++)
        {
            if (!SurveyWallPartDrawn(seg, part))
                continue;

            const Side       *side = seg->sidedef;
            const MapSurface *surf = (part == 0) ? &side->bottom : (part == 1) ? &side->middle : &side->top;

            bool mid_masked = (part == 1) && seg->back_sector != nullptr;

            int reason = StaticWallBakeDecline(seg, surf, mid_masked, nullptr, nullptr);

            if (reason == kStaticBakeSky)
            {
                wall_sky++;
                continue;
            }

            wall_counts[reason]++;
            wall_total++;
        }
    }

    static int survey_count = 0;

    survey_count++;

    LogPrint("Static bake survey #%d for %s (whole map, current sector heights):\n", survey_count,
             current_map ? current_map->name_.c_str() : "?");

    LogPrint("  drawn by the sky path and excluded below: %d flats, %d wall parts\n", flat_sky, wall_sky);

    if (flat_total > 0)
    {
        LogPrint("  flats: %d of %d eligible (%.1f%%) over %d subsectors\n", flat_counts[kStaticBakeAccepted],
                 flat_total, 100.0f * (float)flat_counts[kStaticBakeAccepted] / (float)flat_total,
                 total_level_subsectors);

        for (int i = 1; i < kStaticBakeDeclineTotal; i++)
        {
            if (flat_counts[i] == 0)
                continue;

            LogPrint("      %-32s %6d (%.1f%%)\n", StaticBakeDeclineName(i), flat_counts[i],
                     100.0f * (float)flat_counts[i] / (float)flat_total);
        }
    }

    if (wall_total > 0)
    {
        LogPrint("  walls: %d of %d eligible (%.1f%%) over %d segs\n", wall_counts[kStaticBakeAccepted], wall_total,
                 100.0f * (float)wall_counts[kStaticBakeAccepted] / (float)wall_total, total_level_segs);

        for (int i = 1; i < kStaticBakeDeclineTotal; i++)
        {
            if (wall_counts[i] == 0)
                continue;

            LogPrint("      %-32s %6d (%.1f%%)\n", StaticBakeDeclineName(i), wall_counts[i],
                     100.0f * (float)wall_counts[i] / (float)wall_total);
        }
    }
}

bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef)
{
    if (!r_static_mesh.d_ || !static_mesh_built || !seg)
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

void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
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
                            const MapSurface *surf, const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef)
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

    capture_is_wall    = false;
    capture_mid_masked = false;
    capture_flag_slot = (int)((sub - level_subsectors) * 2 + (face_dir > 0 ? 0 : 1));
    capture_hash_key  = 0;

    if (plane_ef)
    {
        capture_flag_slot = -1;

        if (!BuildRegionFlatKey(sub, face_dir, plane_ef, capture_height_key, &capture_hash_key))
            capture_hash_key = 0;
    }

    capture_dependency_count = 0;

    AddCaptureDependency(sector);
    AddCaptureDependency(sector->height_sector);
    AddCaptureDependency(capture_light_sector);
    AddCaptureDependency(ExtrafloorControlSector(plane_ef));
}

void StaticCaptureVertices(const RendererVertex *verts, int count)
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
            if (slot < seg_wall_baked.size())
                seg_wall_baked[slot] = 1;
        }
        else if (slot < subsector_flat_baked.size())
            subsector_flat_baked[slot] = 1;
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
            if (slot < seg_wall_baked.size())
                seg_wall_baked[slot] = 0;
        }
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

    subsector_flat_baked.assign((size_t)total_level_subsectors * 2 * kHeightKeyTotal, 0);
    seg_wall_baked.assign((size_t)total_level_segs * 3 * kHeightKeyTotal, 0);
    region_surface_baked.clear();
    sector_spans.assign((size_t)total_level_sectors, std::vector<SpanReference>());

    sector_height_state.assign((size_t)total_level_sectors, 0);

    sector_light_cache.resize((size_t)total_level_sectors);

    for (int i = 0; i < total_level_sectors; i++)
        sector_light_cache[i] = level_sectors[i].properties.light_level;

    residency_flat_state.assign((size_t)total_level_subsectors * 2, 0xFF);
    residency_wall_state.assign((size_t)total_level_segs * 3, 0xFF);
    residency_extra_flat.assign((size_t)total_level_subsectors * 2, 0);
    residency_extra_wall.assign((size_t)total_level_segs, 0);
    residency_frame_flat.assign((size_t)total_level_subsectors * 2, 0);
    residency_frame_wall.assign((size_t)total_level_segs * 3, 0);

    static_mesh_built = true;

    if (r_static_survey.d_)
        StaticResidencySurvey();
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
    region_surface_baked.clear();
    sector_spans.clear();
    sector_light_cache.clear();
    residency_flat_state.clear();
    residency_wall_state.clear();
    residency_extra_flat.clear();
    residency_extra_wall.clear();
    residency_frame_flat.clear();
    residency_frame_wall.clear();

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
    if (!r_static_mesh.d_ || !static_mesh_built)
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

        uint64_t mark = GetMicroseconds();

        RefreshSectorHeightStates();

        uint64_t after_heights = GetMicroseconds();

        RefreshStaticLighting();

        frame_time_height_state += after_heights - mark;
        frame_time_light_refresh += GetMicroseconds() - after_heights;
    }

    for (size_t i = 0; i < static_batches.size(); i++)
    {
        StaticBatch &batch = static_batches[i];

        bool wanted = (batch.draw_pass == draw_pass) ||
                      (batch.draw_pass == kOitPassAccumulate && draw_pass == kOitPassRevealage);

        if (batch.spans.empty() || !wanted)
            continue;

        frame_batches_drawn++;

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

        uint64_t run_mark = GetMicroseconds();

        for (size_t k = 0; k <= batch.spans.size(); k++)
        {
            bool live = (k < batch.spans.size()) && batch.spans[k].live;

            if (live && batch.spans[k].height_key !=
                            StaticHeightKey(batch.spans[k].sector, batch.spans[k].back_sector))
            {
                frame_span_height_skipped++;

                live = false;
            }

            bool contiguous = live && run_start >= 0 && batch.spans[k].start == run_end &&
                              (size_t)(run_end + batch.spans[k].count - run_start) <= kMaximumStaticRun;

            if (live)
                frame_span_live++;

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
                frame_span_runs++;

                run_start = batch.spans[k].start;
                run_end   = batch.spans[k].start + batch.spans[k].count;
            }
        }

        frame_time_span_runs += GetMicroseconds() - run_mark;

    }
}
