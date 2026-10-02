#pragma once

#include <vector>

#include "r_backend.h"
#include "r_units.h"

struct Sector;
struct LineSide;
struct MapSurface;
struct RegionProperties;
struct Extrafloor;
class Image;
class Colormap;

struct StaticSectorChange
{
    int  sector;
    bool appearance;
};

void SnapshotSurfaceBaseOffsets(void);
void BuildStaticMesh(void);
void DestroyStaticMesh(void);
void DrawStaticMesh(OitPass draw_pass, bool refresh = true);

const std::vector<StaticSectorChange> &StaticSectorChanges(void);

bool StaticMeshCoversFlat(const Sector *sec, int face_dir, const Extrafloor *plane_ef);
bool StaticMeshCoversWall(const LineSide *line_side, const MapSurface *surf, const Extrafloor *region_ef,
                          const Extrafloor *surface_ef);
bool StaticMeshBuilt(void);

void BakeStaticLevel(void);
void StaticBakeSetDeferred(bool deferred);
bool StaticBakeDeferred(void);

void StaticBakeBegin(void);
void StaticBakeEnd(void);
bool StaticBakeActive(void);

void StaticBakeSectorBegin(const Sector *sec);
void StaticBakeSectorEnd(const Sector *sec);
void StaticMarkSectorDeclined(const Sector *sec);
void StaticMarkSectorPending(const Sector *sec);
bool StaticSectorReady(const Sector *sec);
uint32_t StaticSectorEpoch(const Sector *sec);
void StaticTakeSettledPendingSectors(std::vector<Sector *> &out);

enum HeightRenderState
{
    kHeightStateNormal = 0,
    kHeightStateAbove,
    kHeightStateBelow,
    kHeightStateTotal
};

constexpr int kHeightKeyTotal = kHeightStateTotal * kHeightStateTotal;

float LiquidLevelSeconds(void);

int SectorHeightState(const Sector *sec);
int StaticHeightKey(const Sector *front, const Sector *back);

bool StaticWallBakeEligible(const LineSide *line_side, const MapSurface *surf, bool mid_masked,
                            const Extrafloor *region_ef, const Extrafloor *surface_ef);

bool StaticFlatBakeEligible(const Sector *sec, int face_dir);
bool StaticFlatBakeEligibleSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir);
int  StaticFlatBakeDeclineSurface(const Sector *sec, const MapSurface *surf, const Sector *surf_owner, int face_dir);
bool StaticPropertiesResolvable(const Sector *sec, const RegionProperties *props);

enum StaticBakeDecline
{
    kStaticBakeAccepted = 0,
    kStaticBakeMeshDisabled,
    kStaticBakeNoSurface,
    kStaticBakeNoSidedef,
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
int StaticWallBakeDecline(const LineSide *line_side, const MapSurface *surf, bool mid_masked,
                          const Extrafloor *region_ef, const Extrafloor *surface_ef);

int  StaticExtrafloorPlaneDecline(const Sector *sec, const Extrafloor *plane_ef, int face_dir);
bool StaticExtrafloorPlaneEligible(const Sector *sec, const Extrafloor *plane_ef, int face_dir);


void StaticMeshInvalidateSector(Sector *sec);
void StaticRefreshSectorTraits(void);

void    StaticPruneDynamicSectors(void);
int     StaticDynamicSectorCount(void);
Sector *StaticDynamicSector(int index);

void StaticMeshStats(int *batches, int *live_spans, int *dead_spans, int *vertices);


void StaticCaptureBeginFlat(Sector *sector, int face_dir, const Image *image, RegionProperties *props,
                            BlendingMode blending, const HMM_Vec3 &normal, OitPass draw_pass, const MapSurface *surf,
                            const HMM_Vec2 &uv_scale, const Extrafloor *plane_ef);
void StaticCaptureBegin(const LineSide *line_side, const MapSurface *surf, const Image *image, RegionProperties *props,
                        Sector *sector, BlendingMode blending, int light_adjust, const HMM_Vec3 &normal,
                        float div_x, float div_y, float div_delta_x, float div_delta_y, bool mid_masked,
                        OitPass draw_pass, const HMM_Vec2 &uv_scale, const Extrafloor *region_ef,
                        const Extrafloor *surface_ef);
void StaticCaptureVertices(GLuint shape, const RendererVertex *verts, int count);
void StaticCaptureEnd(void);
