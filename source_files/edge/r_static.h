#pragma once

#include "r_backend.h"
#include "r_units.h"

struct Sector;
struct Subsector;
struct Seg;
struct MapSurface;
struct RegionProperties;
struct Extrafloor;
class Image;

void SnapshotSurfaceBaseOffsets(void);
void BuildStaticMesh(void);
void DestroyStaticMesh(void);
void DrawStaticMesh(OitPass draw_pass, bool refresh = true);

bool StaticMeshCoversFlat(const Subsector *sub, int face_dir, const Extrafloor *plane_ef);
bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef);
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

bool StaticWallBakeEligible(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                            const Extrafloor *surface_ef);

bool StaticFlatBakeEligible(const Sector *sec, int face_dir);

enum StaticBakeDecline
{
    kStaticBakeAccepted = 0,
    kStaticBakeMeshDisabled,
    kStaticBakeNoSurface,
    kStaticBakeMiniseg,
    kStaticBakeNotSidedefPart,
    kStaticBakeGlass,
    kStaticBakeSideDynamic,
    kStaticBakeSectorDynamic,
    kStaticBakeSectorScrolls,
    kStaticBakeSectorSuppressed,
    kStaticBakeExtrafloor,
    kStaticBakeHeightSector,
    kStaticBakeBackDynamic,
    kStaticBakeBackScrolls,
    kStaticBakeBackSuppressed,
    kStaticBakeBackExtrafloor,
    kStaticBakeBackHeightSector,
    kStaticBakeMirrorLine,
    kStaticBakePortalLine,
    kStaticBakeSlideDoor,
    kStaticBakeVertexSector,
    kStaticBakeOverrideProperties,
    kStaticBakeSky,
    kStaticBakeNoImage,
    kStaticBakeAnimationSize,
    kStaticBakeComplexOpacity,
    kStaticBakeRotatedScroll,
    kStaticBakeSurfaceBob,
    kStaticBakeNotOwnPlane,
    kStaticBakeKeyOverflow,
    kStaticBakeDeclineTotal
};

const char *StaticBakeDeclineName(int reason);

int StaticFlatBakeDecline(const Sector *sec, int face_dir);
int StaticWallBakeDecline(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef);

int  StaticExtrafloorPlaneDecline(const Subsector *sub, const Extrafloor *plane_ef, int face_dir);
bool StaticExtrafloorPlaneEligible(const Subsector *sub, const Extrafloor *plane_ef, int face_dir);

void StaticResidencyNoteFlat(const Subsector *sub, int face_dir, bool resident, bool own_plane,
                             const Extrafloor *plane_ef);
void StaticResidencyNoteWall(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                             const Extrafloor *surface_ef, bool resident);
void StaticResidencyNoteRegionProperties(Sector *sec, RegionProperties *props);
void StaticResidencyReport(void);
void StaticResidencyTick(void);
void StaticResidencySurvey(void);

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
                            const MapSurface *surf, const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef);
void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale, const Extrafloor *region_ef,
                        const Extrafloor *surface_ef);
void StaticCaptureVertices(const RendererVertex *verts, int count);
void StaticCaptureEnd(void);
