#pragma once

#include <stdint.h>

#include <vector>

#include "HandmadeMath.h"

constexpr int kLightGridTileSize      = 16;
constexpr int kLightGridMaximumLights = 254;
constexpr int kLightGridMaximumPerTile = 64;
constexpr int kLightGridDepthSlices   = 8;
constexpr int kLightGridMaximumGlows  = 2;

struct LightGridLight
{
    HMM_Vec3 eye_position;
    float    radius;

    HMM_Vec3 color;

    float additive;
};

struct LightGridGlow
{
    float plane[4];
    float color[3];
    float radius;
    float additive;
};

struct LightGridGlowSet
{
    int           count = 0;
    LightGridGlow glows[kLightGridMaximumGlows];
};

struct LightGrid
{
    std::vector<LightGridLight> lights;

    int tiles_x = 0;
    int tiles_y = 0;

    int view_x = 0;
    int view_y = 0;
    int view_width  = 0;
    int view_height = 0;

    std::vector<uint32_t> tile_offsets;
    std::vector<uint8_t>  tile_counts;
    std::vector<uint8_t>  tile_list;

    std::vector<uint32_t> cluster_offsets;
    std::vector<uint8_t>  cluster_counts;
    std::vector<uint8_t>  cluster_list;

    float cluster_near = 1.0f;
    float cluster_far  = 1.0f;

    uint32_t serial = 0;

    int max_tile_count    = 0;
    int max_cluster_count = 0;
    int dropped_tile      = 0;
    int dropped_cluster   = 0;

    bool Empty() const
    {
        return lights.empty();
    }

    int TileTotal() const
    {
        return tiles_x * tiles_y;
    }

    int ClusterTotal() const
    {
        return tiles_x * tiles_y * kLightGridDepthSlices;
    }
};

enum LightGridBinning
{
    kLightGridBinTiles,
    kLightGridBinClusters,
    kLightGridBinNone
};

struct Sector;
class MapObject;

int LightGridGlowSetForSector(Sector *sec);

int LightGridSampleTotal(void);

MapObject *LightGridSampleLight(int index);

const LightGridGlowSet *LightGridGlowSetAt(int index);

int LightGridGlowDropped(void);

void BuildLightGrid(void);

void ClearLightGrid(void);

const LightGrid *CurrentLightGrid(void);

int LightGridSliceFromDepth(float eye_depth, float near_plane, float far_plane);
