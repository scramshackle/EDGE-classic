#pragma once

#include "r_units.h"

struct Sector;
struct Subsector;
struct Seg;
struct MapSurface;
struct RegionProperties;
class Image;

void BuildStaticMesh(void);
void DestroyStaticMesh(void);
void DrawStaticMesh(void);

bool StaticMeshCoversFlat(const Subsector *sub, int face_dir);
bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf);
bool StaticMeshEnabled(void);

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
    Sector *sector;

    GLuint       tex_id;
    BlendingMode blending;
};

void EmitStaticSpanLights(const StaticSpanLighting &info);

void StaticCaptureBeginFlat(const Subsector *sub, int face_dir, const Image *image, RegionProperties *props,
                            Sector *sector, BlendingMode blending, const HMM_Vec3 &normal);
void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y);
void StaticCaptureVertices(const RendererVertex *verts, int count);
void StaticCaptureEnd(void);
