#pragma once

#include <stdint.h>

#include <vector>

#include "epi_vector.h"

constexpr int kLightGridClusterSize       = 16;
constexpr int kLightGridMaximumLights     = 1023;
constexpr int kLightGridMaximumPerCluster = 64;
constexpr int kLightGridDepthSlices       = 8;
constexpr int kLightGridMaximumGlows      = 2;

struct LightGridLight
{
    epi::Vec3 eye_position;
    float    radius;

    epi::Vec3 world_position;

    epi::Vec3 color;

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

    int clusters_x = 0;
    int clusters_y = 0;

    int view_x = 0;
    int view_y = 0;
    int view_width  = 0;
    int view_height = 0;

    std::vector<uint32_t> cluster_offsets;
    std::vector<uint8_t>  cluster_counts;
    std::vector<uint16_t> cluster_list;

    float cluster_near = 1.0f;
    float cluster_far  = 1.0f;

    uint32_t serial = 0;

    int max_cluster_count = 0;
    int dropped_cluster   = 0;

    bool Empty() const
    {
        return lights.empty();
    }

    int ClusterTotal() const
    {
        return clusters_x * clusters_y * kLightGridDepthSlices;
    }
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

void PushLightGrid(void);

void PopLightGrid(void);

const LightGrid *CurrentLightGrid(void);

bool LightGridReachesBox(const float low[3], const float high[3]);

int LightGridSliceFromDepth(float eye_depth, float near_plane, float far_plane);
