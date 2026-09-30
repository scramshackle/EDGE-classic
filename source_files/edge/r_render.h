#pragma once

#include "r_gldefs.h"
#include "r_image.h"
#include "r_units.h"

extern std::unordered_set<Line *> newly_seen_lines;

void RenderSectorList(std::list<DrawSector *> &dsectors, std::list<DrawThing *> &dthings,
                      std::list<DrawMirror *> &dmirrors, bool for_mirror = false);

void EnumerateViewSky(void);

void EnumerateViewMirrors(void);
void EnumerateViewSectors(void);
bool SectorReachedThisView(const Sector *sector);

DrawSector *BakeDrawSector(Sector *sector);

void SkyDecideLineSide(LineSide *line_side, DrawMirror *mir, bool resident);
void SkyDecideSector(Sector *sector, DrawMirror *mir, bool resident);

void UpdateSectorInterpolation(Sector *sector);
