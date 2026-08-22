#include "r_lightgrid.h"

#include <math.h>

#include <unordered_map>

#include "con_var.h"
#include "edge_profiling.h"
#include "epi.h"
#include "i_system.h"
#include "p_blockmap.h"
#include "p_mobj.h"
#include "r_backend.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_misc.h"
#include "r_shader.h"
#include "r_state.h"


static LightGrid current_light_grid;

static constexpr float kLightGridMinimumW = 0.0001f;

struct LightGridCollected
{
    MapObject *object;
    HMM_Vec3   world_position;
    float      radius;
    HMM_Vec3   color;
    float      additive;
};

static std::vector<LightGridCollected> collected_lights;

static std::vector<LightGridGlowSet>   glow_sets;
static std::unordered_map<const Sector *, int> glow_set_lookup;

static HMM_Mat4 glow_model_view;

static int glow_dropped = 0;

static void LightGridCollect(MapObject *mo, void *data)
{
    EPI_UNUSED(data);

    if (collected_lights.size() >= (size_t)kLightGridMaximumLights)
        return;

    if (!mo->dynamic_light_.shader)
        return;

    DynamicLightParameters parameters;

    if (!mo->dynamic_light_.shader->GetLightParameters(&parameters))
        return;

    LightGridCollected entry;

    entry.object         = mo;
    entry.world_position = parameters.position;
    entry.radius         = parameters.radius;
    entry.color          = parameters.color;
    entry.additive       = parameters.additive ? 1.0f : 0.0f;

    if (entry.radius <= 0.0f)
        return;

    if (entry.color.X <= 0.0f && entry.color.Y <= 0.0f && entry.color.Z <= 0.0f)
        return;

    collected_lights.push_back(entry);
}

int LightGridSampleTotal(void)
{
    return (int)collected_lights.size();
}

MapObject *LightGridSampleLight(int index)
{
    if (index < 0 || index >= (int)collected_lights.size())
        return nullptr;

    return collected_lights[(size_t)index].object;
}

int LightGridGlowDropped(void)
{
    return glow_dropped;
}

const LightGridGlowSet *LightGridGlowSetAt(int index)
{
    if (index < 0 || index >= (int)glow_sets.size())
        return nullptr;

    return &glow_sets[(size_t)index];
}

static bool BuildGlowForObject(MapObject *mo, const Sector *sec, LightGridGlow *out)
{
    if (mo->info_->dlight_.type_ == kDynamicLightTypeNone)
        return false;

    float radius = mo->dynamic_light_.r;

    if (radius <= 0.0f)
        return false;

    if (!mo->info_->force_fullbright_ && mo->state_->bright <= 0)
        return false;

    HMM_Vec4 plane;

    if (mo->info_->glow_type_ == kSectorGlowTypeWall)
    {
        Line *ld = mo->dynamic_light_.glow_wall;

        if (!ld || ld->length <= 0.0f)
            return false;

        float norm_x = (ld->vertex_1->Y - ld->vertex_2->Y) / ld->length;
        float norm_y = (ld->vertex_2->X - ld->vertex_1->X) / ld->length;

        plane = {{-norm_x, -norm_y, 0.0f, ld->vertex_1->X * norm_x + ld->vertex_1->Y * norm_y}};
    }
    else if (mo->info_->glow_type_ == kSectorGlowTypeFloor)
        plane = {{0.0f, 0.0f, 1.0f, -sec->floor_height}};
    else
        plane = {{0.0f, 0.0f, 1.0f, -sec->ceiling_height}};

    HMM_Vec4 eye_plane = EyeSpacePlane(glow_model_view, plane);

    for (int i = 0; i < 4; i++)
        out->plane[i] = eye_plane.Elements[i];

    RGBAColor color = mo->dynamic_light_.color;

    float level = (mo->info_->force_fullbright_ ? 255.0f : mo->state_->bright) / 255.0f;

    out->color[0] = level * epi::GetRGBARed(color);
    out->color[1] = level * epi::GetRGBAGreen(color);
    out->color[2] = level * epi::GetRGBABlue(color);

    out->radius   = radius;
    out->additive = (mo->info_->dlight_.type_ == kDynamicLightTypeAdd) ? 1.0f : 0.0f;

    return true;
}

int LightGridGlowSetForSector(Sector *sec)
{
    if (!use_dynamic_lights || !sec || !sec->glow_things)
        return -1;

    if (render_view_extra_light >= 250)
        return -1;

    std::unordered_map<const Sector *, int>::iterator existing = glow_set_lookup.find(sec);

    if (existing != glow_set_lookup.end())
        return existing->second;

    LightGridGlowSet set;

    for (MapObject *mo = sec->glow_things; mo; mo = mo->dynamic_light_next_)
    {
        LightGridGlow glow;

        if (!BuildGlowForObject(mo, sec, &glow))
            continue;

        if (set.count >= kLightGridMaximumGlows)
        {
            glow_dropped++;
            continue;
        }

        set.glows[set.count++] = glow;
    }

    int index = -1;

    if (set.count > 0)
    {
        index = (int)glow_sets.size();

        glow_sets.push_back(set);
    }

    glow_set_lookup[sec] = index;

    return index;
}

int LightGridSliceFromDepth(float eye_depth, float near_plane, float far_plane)
{
    if (eye_depth <= near_plane)
        return 0;

    if (eye_depth >= far_plane)
        return kLightGridDepthSlices - 1;

    float ratio = logf(eye_depth / near_plane) / logf(far_plane / near_plane);

    int slice = (int)(ratio * (float)kLightGridDepthSlices);

    return HMM_Clamp(0, slice, kLightGridDepthSlices - 1);
}

static bool LightGridScreenBounds(const HMM_Mat4 &view_projection, const LightGridCollected &light, float *out_min_x,
                                  float *out_min_y, float *out_max_x, float *out_max_y)
{
    float minimum_x = 0.0f;
    float minimum_y = 0.0f;
    float maximum_x = 0.0f;
    float maximum_y = 0.0f;

    int in_front = 0;

    for (int corner = 0; corner < 8; corner++)
    {
        HMM_Vec4 world;

        world.X = light.world_position.X + ((corner & 1) ? light.radius : -light.radius);
        world.Y = light.world_position.Y + ((corner & 2) ? light.radius : -light.radius);
        world.Z = light.world_position.Z + ((corner & 4) ? light.radius : -light.radius);
        world.W = 1.0f;

        HMM_Vec4 clip = HMM_MulM4V4(view_projection, world);

        if (clip.W <= kLightGridMinimumW)
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
        return false;

    if (in_front < 8)
    {
        *out_min_x = (float)view_window_x;
        *out_min_y = (float)view_window_y;
        *out_max_x = (float)(view_window_x + view_window_width);
        *out_max_y = (float)(view_window_y + view_window_height);

        return true;
    }

    *out_min_x = minimum_x;
    *out_min_y = minimum_y;
    *out_max_x = maximum_x;
    *out_max_y = maximum_y;

    return true;
}

void ClearLightGrid(void)
{
    current_light_grid.lights.clear();
    current_light_grid.tile_counts.clear();
    current_light_grid.tile_offsets.clear();
    current_light_grid.tile_list.clear();
    current_light_grid.cluster_counts.clear();
    current_light_grid.cluster_offsets.clear();
    current_light_grid.cluster_list.clear();

    current_light_grid.tiles_x = 0;
    current_light_grid.tiles_y = 0;

    current_light_grid.max_tile_count    = 0;
    current_light_grid.max_cluster_count = 0;
    current_light_grid.dropped_tile      = 0;
    current_light_grid.dropped_cluster   = 0;
}

const LightGrid *CurrentLightGrid(void)
{
    return &current_light_grid;
}

void BuildLightGrid(void)
{
    EDGE_ZoneScoped;

    ClearLightGrid();

    glow_sets.clear();
    glow_set_lookup.clear();
    glow_dropped = 0;

    glow_model_view = render_backend->WorldModelView();

    current_light_grid.serial++;

    if (!use_dynamic_lights || view_window_width <= 0 || view_window_height <= 0)
        return;

    if (render_view_extra_light >= 250)
        return;

    collected_lights.clear();

    float reach = renderer_far_clip.f_;

    uint64_t collect_mark = GetMicroseconds();

    DynamicLightIterator(view_x - reach, view_y - reach, view_z - reach, view_x + reach, view_y + reach, view_z + reach,
                         LightGridCollect, nullptr);

    ec_frame_stats.light_grid_collect_us += GetMicroseconds() - collect_mark;

    if (collected_lights.empty())
        return;

    uint64_t bin_mark = GetMicroseconds();

    HMM_Mat4 model_view      = render_backend->WorldModelView();
    HMM_Mat4 view_projection = render_backend->WorldViewProjection();

    glow_model_view = model_view;

    current_light_grid.view_x      = view_window_x;
    current_light_grid.view_y      = view_window_y;
    current_light_grid.view_width  = view_window_width;
    current_light_grid.view_height = view_window_height;

    current_light_grid.tiles_x = (view_window_width + kLightGridTileSize - 1) / kLightGridTileSize;
    current_light_grid.tiles_y = (view_window_height + kLightGridTileSize - 1) / kLightGridTileSize;

    current_light_grid.cluster_near = HMM_MAX(1.0f, renderer_near_clip.f_);
    current_light_grid.cluster_far  = HMM_MAX(current_light_grid.cluster_near * 2.0f, renderer_far_clip.f_);

    int binning = render_backend->LightGridBinningMode();

    bool use_clusters = (binning == kLightGridBinClusters);

    int tile_total    = (binning == kLightGridBinTiles) ? current_light_grid.TileTotal() : 0;
    int cluster_total = use_clusters ? current_light_grid.ClusterTotal() : 0;

    current_light_grid.tile_counts.assign((size_t)tile_total, 0);
    current_light_grid.tile_offsets.assign((size_t)tile_total, 0);

    current_light_grid.cluster_counts.assign((size_t)cluster_total, 0);
    current_light_grid.cluster_offsets.assign((size_t)cluster_total, 0);

    struct LightGridCoverage
    {
        int tile_x1, tile_y1, tile_x2, tile_y2;
        int slice1, slice2;
    };

    static std::vector<LightGridCoverage> coverage;

    coverage.clear();

    for (size_t i = 0; i < collected_lights.size(); i++)
    {
        const LightGridCollected &light = collected_lights[i];

        float minimum_x = 0.0f;
        float minimum_y = 0.0f;
        float maximum_x = 0.0f;
        float maximum_y = 0.0f;

        if (!LightGridScreenBounds(view_projection, light, &minimum_x, &minimum_y, &maximum_x, &maximum_y))
            continue;

        HMM_Vec4 world = {{light.world_position.X, light.world_position.Y, light.world_position.Z, 1.0f}};

        HMM_Vec4 eye = HMM_MulM4V4(model_view, world);

        LightGridLight entry;

        entry.eye_position = {{eye.X, eye.Y, eye.Z}};
        entry.radius       = light.radius;
        entry.color        = light.color;
        entry.additive     = light.additive;

        LightGridCoverage cover;

        cover.tile_x1 = HMM_MAX(0, (int)floorf((minimum_x - (float)view_window_x) / (float)kLightGridTileSize));
        cover.tile_y1 = HMM_MAX(0, (int)floorf((minimum_y - (float)view_window_y) / (float)kLightGridTileSize));
        cover.tile_x2 = HMM_MIN(current_light_grid.tiles_x - 1,
                                (int)floorf((maximum_x - (float)view_window_x) / (float)kLightGridTileSize));
        cover.tile_y2 = HMM_MIN(current_light_grid.tiles_y - 1,
                                (int)floorf((maximum_y - (float)view_window_y) / (float)kLightGridTileSize));

        if (cover.tile_x1 > cover.tile_x2 || cover.tile_y1 > cover.tile_y2)
            continue;

        float depth = -eye.Z;

        cover.slice1 = LightGridSliceFromDepth(depth - light.radius, current_light_grid.cluster_near,
                                               current_light_grid.cluster_far);
        cover.slice2 = LightGridSliceFromDepth(depth + light.radius, current_light_grid.cluster_near,
                                               current_light_grid.cluster_far);

        current_light_grid.lights.push_back(entry);

        coverage.push_back(cover);
    }

    for (size_t i = 0; binning != kLightGridBinNone && i < coverage.size(); i++)
    {
        const LightGridCoverage &cover = coverage[i];

        for (int tile_y = cover.tile_y1; tile_y <= cover.tile_y2; tile_y++)
        {
            for (int tile_x = cover.tile_x1; tile_x <= cover.tile_x2; tile_x++)
            {
                if (!use_clusters)
                {
                    int tile = tile_y * current_light_grid.tiles_x + tile_x;

                    if (current_light_grid.tile_counts[tile] < kLightGridMaximumPerTile)
                        current_light_grid.tile_counts[tile]++;
                    else
                        current_light_grid.dropped_tile++;

                    continue;
                }

                for (int slice = cover.slice1; slice <= cover.slice2; slice++)
                {
                    int cluster = (slice * current_light_grid.tiles_y + tile_y) * current_light_grid.tiles_x + tile_x;

                    if (current_light_grid.cluster_counts[cluster] < kLightGridMaximumPerTile)
                        current_light_grid.cluster_counts[cluster]++;
                    else
                        current_light_grid.dropped_cluster++;
                }
            }
        }
    }

    uint32_t tile_running = 0;

    for (int tile = 0; tile < tile_total; tile++)
    {
        current_light_grid.tile_offsets[tile] = tile_running;

        tile_running += current_light_grid.tile_counts[tile];

        if (current_light_grid.tile_counts[tile] > current_light_grid.max_tile_count)
            current_light_grid.max_tile_count = current_light_grid.tile_counts[tile];
    }

    uint32_t cluster_running = 0;

    for (int cluster = 0; cluster < cluster_total; cluster++)
    {
        current_light_grid.cluster_offsets[cluster] = cluster_running;

        cluster_running += current_light_grid.cluster_counts[cluster];

        if (current_light_grid.cluster_counts[cluster] > current_light_grid.max_cluster_count)
            current_light_grid.max_cluster_count = current_light_grid.cluster_counts[cluster];
    }

    current_light_grid.tile_list.assign((size_t)tile_running, 0);
    current_light_grid.cluster_list.assign((size_t)cluster_running, 0);

    static std::vector<uint8_t> tile_filled;
    static std::vector<uint8_t> cluster_filled;

    tile_filled.assign((size_t)tile_total, 0);
    cluster_filled.assign((size_t)cluster_total, 0);

    for (size_t i = 0; binning != kLightGridBinNone && i < coverage.size(); i++)
    {
        const LightGridCoverage &cover = coverage[i];

        for (int tile_y = cover.tile_y1; tile_y <= cover.tile_y2; tile_y++)
        {
            for (int tile_x = cover.tile_x1; tile_x <= cover.tile_x2; tile_x++)
            {
                if (!use_clusters)
                {
                    int tile = tile_y * current_light_grid.tiles_x + tile_x;

                    if (tile_filled[tile] < current_light_grid.tile_counts[tile])
                        current_light_grid.tile_list[current_light_grid.tile_offsets[tile] + tile_filled[tile]++] =
                            (uint8_t)i;

                    continue;
                }

                for (int slice = cover.slice1; slice <= cover.slice2; slice++)
                {
                    int cluster = (slice * current_light_grid.tiles_y + tile_y) * current_light_grid.tiles_x + tile_x;

                    if (cluster_filled[cluster] < current_light_grid.cluster_counts[cluster])
                        current_light_grid
                            .cluster_list[current_light_grid.cluster_offsets[cluster] + cluster_filled[cluster]++] =
                            (uint8_t)i;
                }
            }
        }
    }

    ec_frame_stats.light_grid_bin_us += GetMicroseconds() - bin_mark;
}
