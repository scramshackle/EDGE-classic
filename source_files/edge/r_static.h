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
bool StaticMeshCoversSubsector(const Subsector *sub);
bool StaticMeshCoversWall(const Seg *seg, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef);
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

bool StaticWallCoversLine(const Seg *seg, const MapSurface *surf, bool mid_masked, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef);

bool StaticFlatBakeEligible(const Sector *sec, int face_dir);
bool StaticFlatBakeEligibleSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir);
int  StaticFlatBakeDeclineSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir);
bool StaticPropertiesResolvable(const Sector *sec, const RegionProperties *props);

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


void StaticMeshInvalidateSector(Sector *sec);

void    StaticPruneDynamicSectors(void);
int     StaticDynamicSectorCount(void);
Sector *StaticDynamicSector(int index);

void StaticMeshStats(int *batches, int *live_spans, int *dead_spans, int *vertices);


void StaticCaptureBeginFlat(const Subsector *sub, int face_dir, const Image *image, RegionProperties *props,
                            Sector *sector, BlendingMode blending, const HMM_Vec3 &normal, OitPass draw_pass,
                            const MapSurface *surf, const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef,
                            bool covers_sector);
void StaticCaptureBegin(const Seg *seg, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale, const Extrafloor *region_ef,
                        const Extrafloor *surface_ef, bool covers_line);
void StaticCaptureVertices(GLuint shape, const RendererVertex *verts, int count);
void StaticCaptureEnd(void);
