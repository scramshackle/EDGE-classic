#pragma once

#include "r_backend.h"
#include "r_units.h"

struct Sector;
struct Subsector;
struct Seg;
struct MapSurface;
struct RegionProperties;
class Image;

void SnapshotSurfaceBaseOffsets(void);
void BuildStaticMesh(void);
void DestroyStaticMesh(void);
void DrawStaticMesh(OitPass draw_pass, bool refresh = true);

bool StaticMeshCoversFlat(const Subsector *sub, int face_dir);
bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf);
bool StaticMeshEnabled(void);
bool StaticMeshBuilt(void);

enum HeightRenderState
{
    kHeightStateNormal = 0,
    kHeightStateAbove,
    kHeightStateBelow,
    kHeightStateTotal
};

constexpr int kHeightKeyTotal = kHeightStateTotal * kHeightStateTotal;

float LiquidTurbulenceAmplitude(void);
float LiquidTurbulenceWave(void);
void  LiquidTurbulenceDelta(const HMM_Vec3 &pos, HMM_Vec2 *delta);

int SectorHeightState(const Sector *sec);
int StaticHeightKey(const Sector *front, const Sector *back);

bool StaticWallBakeEligible(const Seg *seg, const MapSurface *surf, bool mid_masked);

bool StaticFlatBakeEligible(const Sector *sec, int face_dir);

void StaticMeshInvalidateSector(Sector *sec);

void StaticMeshStats(int *batches, int *live_spans, int *dead_spans, int *vertices);

struct StaticSpanLighting
{
    const RendererVertex *vertices;
    int                   count;

    HMM_Vec3 normal;

    float low[3];
    float high[3];

    float div_x, div_y, div_delta_x, div_delta_y;

    bool    is_wall;
    bool    mid_masked;
    Sector *sector;

    GLuint       tex_id;
    BlendingMode blending;
};

void EmitStaticSpanLights(const StaticSpanLighting &info);

void StaticCaptureBeginFlat(const Subsector *sub, int face_dir, const Image *image, RegionProperties *props,
                            Sector *sector, BlendingMode blending, const HMM_Vec3 &normal, OitPass draw_pass,
                            const MapSurface *surf, const HMM_Vec2 &uv_scale);
void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale);
void StaticCaptureVertices(const RendererVertex *verts, int count);
void StaticCaptureEnd(void);
