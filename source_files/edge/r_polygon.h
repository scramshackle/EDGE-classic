#pragma once

#include <stdint.h>

#include <vector>

#include "r_defs.h"

constexpr size_t kMaximumSectorPolygonVertices = 16384;

enum SectorPolygonStatus
{
    kSectorPolygonOk = 0,
    kSectorPolygonNoEdges,
    kSectorPolygonOpenLoop,
    kSectorPolygonDegenerate,
    kSectorPolygonIncomplete,
    kSectorPolygonOpenSelfReference,
    kSectorPolygonStatusTotal
};

struct SectorPolygon
{
    std::vector<Vertex *> points;
    std::vector<uint32_t> indices;

    std::vector<uint32_t> loop_points;
    std::vector<uint32_t> loop_starts;

    float bounds[4];

    int loop_count;
    int hole_count;
    int status;
};

void BuildSectorPolygons(void);
void DestroySectorPolygons(void);

bool                 SectorPolygonsBuilt(void);
bool                 SectorPolygonsEnabled(void);
const SectorPolygon *SectorPolygonForSector(int sector_index);

const char *SectorPolygonStatusName(int status);

void SectorPolygonReport(void);

int SectorPolygonAtPoint(float x, float y, int exclude_sector);
