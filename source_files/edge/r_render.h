#pragma once

#include "r_gldefs.h"
#include "r_image.h"
#include "r_units.h"

extern std::unordered_set<Line *> newly_seen_lines;

void RenderSubList(std::list<DrawSubsector *> &dsubs, std::list<DrawThing *> &dthings,
                   std::list<DrawMirror *> &dmirrors, bool for_mirror = false);

void BSPWalkNode(unsigned int);

void EnumerateViewSky(void);

void EnumerateViewMirrors(void);
void EnumerateViewSubsectors(void);
bool SubsectorEnumerateEnabled(void);
bool MirrorEnumerateEnabled(void);

void SkyDecideSeg(Seg *seg, DrawMirror *mir, bool queue);
void SkyDecideSubsector(Subsector *sub, DrawMirror *mir, bool queue);

void UpdateSectorInterpolation(Sector *sector);

#ifdef EDGE_THREADED_BSP

constexpr int32_t kRenderItemBatchSize = 16;

enum kRenderType
{
    kRenderSubsector = 0,
    kRenderSkyWall,
    kRenderSkyPlane
};

struct RenderItem
{
    kRenderType type_;

    DrawSubsector *subsector_;

    Seg        *wallSeg_;
    Subsector  *wallPlane_;
    Sector     *skyOwner_;
    DrawMirror *mirror_;

    float height1_;
    float height2_;

    int part_;
};

struct RenderBatch
{
    RenderItem items_[kRenderItemBatchSize];
    int32_t    num_items_;
};

void BSPStartThread();
void BSPStopThread();

void         BSPTraverse();
bool         BSPTraversing();
RenderBatch *BSPReadRenderBatch();

#endif
