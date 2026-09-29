//----------------------------------------------------------------------------
//  EDGE Main Rendering Organisation Code
//----------------------------------------------------------------------------
//
//  Copyright (c) 1999-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------
//
//  Based on the DOOM source code, released by Id Software under the
//  following copyright:
//
//    Copyright (C) 1993-1996 by id Software, Inc.
//
//----------------------------------------------------------------------------
//
// -KM- 1998/09/27 Dynamic Colourmaps
//

#include "r_misc.h"

#include <math.h>

#include "epi_math.h"
#include "am_map.h"
#include "dm_defs.h"
#include "dm_state.h"
#include "e_main.h"
#include "epi.h"
#include "epi_doomdefs.h"
#include "i_defs_gl.h"
#include "m_misc.h"
#include "n_network.h"
#include "p_local.h"
#include "p_mobj.h"
#include "r_colormap.h"
#include "r_defs.h"
#include "r_draw.h"
#include "r_gldefs.h"
#include "r_modes.h"
#include "r_polygon.h"
#include "r_shader.h"
#include "r_units.h"

EDGE_DEFINE_CONSOLE_VARIABLE(field_of_view, "90", kConsoleVariableFlagArchive)


int view_window_x;
int view_window_y;
int view_window_width;
int view_window_height;

BAMAngle view_angle          = 0;
BAMAngle view_vertical_angle = 0;

HMM_Vec3 view_forward;
HMM_Vec3 view_up;
HMM_Vec3 view_right;

BAMAngle normal_field_of_view, zoomed_field_of_view;
bool     view_is_zoomed = false;

// increment every time a check is made
int valid_count = 1;

// just for profiling purposes
int render_frame_count;
int line_count;

Sector           *view_sector;
RegionProperties *view_properties;

float view_x;
float view_y;
float view_z;

float view_cosine;
float view_sine;
float view_rotation = 0.0f;

Player *view_player;

MapObject *background_camera_map_object = nullptr;

//
// precalculated math tables
//

int reduce_flash = 0;

int invulnerability_effect;

float sine_table[kSineTableSize];

void FreeBSP(void);

static float ApproximateAtan2(float y, float x)
{
    static constexpr float kHalfPi = HMM_PI32 / 2.0f;

    // Avoid division by zero
    if (x == 0.0f)
    {
        if (y > 0.0f)
            return kHalfPi;
        if (y < 0.0f)
            return -kHalfPi;
        return 0.0f; // undefined, but return 0
    }

    float atan;
    float z = y / x;
    if (fabsf(z) < 1.0f)
    {
        atan = z / (1.0f + 0.28f * z * z);
        if (x < 0.0f)
        {
            if (y < 0.0f)
                return atan - HMM_PI32;
            else
                return atan + HMM_PI32;
        }
    }
    else
    {
        atan = kHalfPi - z / (z * z + 0.28f);
        if (y < 0.0f)
            return atan - HMM_PI32;
    }

    return atan;
}

BAMAngle PointToAngle(float x1, float y1, float x, float y, bool precise)
{
    x -= x1;
    y -= y1;

    if (precise)
    {
        return (epi::AlmostEquals(x, 0.0f) && epi::AlmostEquals(y, 0.0f)) ? 0 : epi::BAMFromDegrees(atan2(y, x) * (180 / HMM_PI));
    }

    return epi::BAMFromDegrees(ApproximateAtan2(y, x) * (180 / HMM_PI));
}

float PointToDistance(float x1, float y1, float x2, float y2)
{
    BAMAngle angle;
    float    dx;
    float    dy;
    float    temp;
    float    dist;

    dx = (float)fabs(x2 - x1);
    dy = (float)fabs(y2 - y1);

    if (epi::AlmostEquals(dx, 0.0f))
        return dy;
    else if (epi::AlmostEquals(dy, 0.0f))
        return dx;

    if (dy > dx)
    {
        temp = dx;
        dx   = dy;
        dy   = temp;
    }

    angle = epi::BAMFromATan(dy / dx) + kBAMAngle90;

    // use as cosine
    dist = dx / epi::BAMSin(angle);

    return dist;
}

//
// Called once at startup, to initialise some rendering stuff.
//
void RendererStartup(void)
{
    if (language["RefreshDaemon"])
        LogPrint("%s", language["RefreshDaemon"]);
    else
        LogPrint("Unknown Refresh Daemon");

    for (int i = 0; i < kSineTableSize; i++)
    {
        sine_table[i] = HMM_SINF(HMM_AngleDeg(i * 360.0f / ((float)(kSineTableMask))));
    }

    render_frame_count = 0;
}

//
// Called at shutdown
//
void RendererShutdown(void)
{
    FreeBSP();
    DeleteAllImages(true);
}

Sector *PointInSector(float x, float y)
{
    int index = SectorPolygonAtPoint(x, y, -1);

    if (index >= 0)
        return level_sectors + index;

    Line *ld = BlockmapNearestLine(x, y);

    if (!ld)
        return level_sectors;

    DividingLine div;

    div.x       = ld->vertex_1->X;
    div.y       = ld->vertex_1->Y;
    div.delta_x = ld->delta_x;
    div.delta_y = ld->delta_y;

    if (PointOnDividingLineSide(x, y, &div) == 1 && ld->back_sector)
        return ld->back_sector;

    return ld->front_sector;
}

RegionProperties *GetPointProperties(Sector *sector, float z)
{
    if (sector->height_sector)
    {
        if (view_height_zone == kHeightZoneA && view_z > sector->height_sector->interpolated_ceiling_height)
        {
            return sector->height_sector->active_properties;
        }
        else if (view_height_zone == kHeightZoneC && view_z < sector->height_sector->interpolated_floor_height)
        {
            return sector->height_sector->active_properties;
        }
        else
        {
            return sector->active_properties;
        }
    }
    else
    {
        Extrafloor *S, *L, *C;
        float       floor_h;
        // traverse extrafloors upwards

        floor_h = sector->floor_height;

        S = sector->bottom_extrafloor;
        L = sector->bottom_liquid;

        while (S || L)
        {
            if (!L || (S && S->bottom_height < L->bottom_height))
            {
                C = S;
                S = S->higher;
            }
            else
            {
                C = L;
                L = L->higher;
            }

            EPI_ASSERT(C);

            // ignore liquids in the middle of THICK solids, or below real
            // floor or above real ceiling
            //
            if (C->bottom_height < floor_h || C->bottom_height > sector->ceiling_height)
                continue;

            if (z < C->top_height)
                return C->properties;

            floor_h = C->top_height;
        }

        // extrafloors were exhausted, must be top area
        return sector->active_properties;
    }
}

//----------------------------------------------------------------------------

// large buffers for cache coherency vs allocating each on heap
static constexpr uint32_t kDefaultDrawThings  = 65536;
static constexpr uint32_t kDefaultDrawFloors  = 65536;
static constexpr uint32_t kDefaultDrawSectors = 65536;
static constexpr uint32_t kDefaultDrawMirrors = 512;

static std::vector<DrawThing *>  draw_things;
static std::vector<DrawFloor *>  draw_floors;
static std::vector<DrawSector *> draw_sectors;
static std::vector<DrawMirror *> draw_mirrors;

static size_t draw_thing_position;
static size_t draw_floor_position;
static size_t draw_sector_position;
static size_t draw_mirror_position;

static void *draw_memory_buffer = nullptr;

//
// AllocateDrawStructs
//
// One-time initialisation routine.
//
void AllocateDrawStructs(void)
{
    EPI_ASSERT(!draw_memory_buffer);

    size_t size = sizeof(DrawThing) * kDefaultDrawThings;
    size += sizeof(DrawFloor) * kDefaultDrawFloors;
    size += sizeof(DrawSector) * kDefaultDrawSectors;
    size += sizeof(DrawMirror) * kDefaultDrawMirrors;
    size += sizeof(AutomapLine) * kDefaultAutomapLines;

    draw_things.reserve(kDefaultDrawThings);
    draw_floors.reserve(kDefaultDrawFloors);
    draw_sectors.reserve(kDefaultDrawSectors);
    draw_mirrors.reserve(kDefaultDrawMirrors);
    automap_lines.reserve(kDefaultAutomapLines);

    draw_memory_buffer = malloc(size);

    uint8_t *dst = (uint8_t *)draw_memory_buffer;

    for (uint32_t i = 0; i < kDefaultDrawThings; i++, dst += sizeof(DrawThing))
    {
        draw_things.emplace_back(new (dst) DrawThing());
    }

    for (uint32_t i = 0; i < kDefaultDrawFloors; i++, dst += sizeof(DrawFloor))
    {
        draw_floors.emplace_back(new (dst) DrawFloor());
    }

    for (uint32_t i = 0; i < kDefaultDrawSectors; i++, dst += sizeof(DrawSector))
    {
        draw_sectors.emplace_back(new (dst) DrawSector());
    }

    for (uint32_t i = 0; i < kDefaultDrawMirrors; i++, dst += sizeof(DrawMirror))
    {
        draw_mirrors.emplace_back(new (dst) DrawMirror());
    }

    for (uint32_t i = 0; i < kDefaultAutomapLines; i++, dst += sizeof(AutomapLine))
    {
        automap_lines.emplace_back(new (dst) AutomapLine());
    }
}

// bsp clear function

void ClearBSP(void)
{
    draw_thing_position  = 0;
    draw_floor_position  = 0;
    draw_sector_position = 0;
    draw_mirror_position = 0;
}

void FreeBSP(void)
{
    for (size_t i = 0; i < kDefaultDrawThings; i++)
    {
        draw_things[i]->~DrawThing();
    }
    for (size_t i = kDefaultDrawThings; i < draw_things.size(); i++)
    {
        delete draw_things[i];
    }
    for (size_t i = 0; i < kDefaultDrawFloors; i++)
    {
        draw_floors[i]->~DrawFloor();
    }
    for (size_t i = kDefaultDrawFloors; i < draw_floors.size(); i++)
    {
        delete draw_floors[i];
    }
    for (size_t i = 0; i < kDefaultDrawSectors; i++)
    {
        draw_sectors[i]->~DrawSector();
    }
    for (size_t i = kDefaultDrawSectors; i < draw_sectors.size(); i++)
    {
        delete draw_sectors[i];
    }
    for (size_t i = 0; i < kDefaultDrawMirrors; i++)
    {
        draw_mirrors[i]->~DrawMirror();
    }
    for (size_t i = kDefaultDrawMirrors; i < draw_mirrors.size(); i++)
    {
        delete draw_mirrors[i];
    }
    for (size_t i = 0; i < kDefaultAutomapLines; i++)
    {
        automap_lines[i]->~AutomapLine();
    }
    for (size_t i = kDefaultAutomapLines; i < automap_lines.size(); i++)
    {
        delete automap_lines[i];
    }

    draw_things.clear();
    draw_floors.clear();
    draw_sectors.clear();
    draw_mirrors.clear();
    automap_lines.clear();

    ClearBSP();

    if (draw_memory_buffer)
    {
        free(draw_memory_buffer);
        draw_memory_buffer = nullptr;
    }
}

DrawThing *GetDrawThing()
{
    if (draw_thing_position == draw_things.size())
        draw_things.push_back(new DrawThing());

    return draw_things[draw_thing_position++];
}

DrawFloor *GetDrawFloor()
{
    if (draw_floor_position == draw_floors.size())
        draw_floors.push_back(new DrawFloor());

    return draw_floors[draw_floor_position++];
}

DrawSector *GetDrawSector()
{
    if (draw_sector_position == draw_sectors.size())
        draw_sectors.push_back(new DrawSector());

    return draw_sectors[draw_sector_position++];
}

DrawMirror *GetDrawMirror()
{
    if (draw_mirror_position == draw_mirrors.size())
        draw_mirrors.push_back(new DrawMirror());

    return draw_mirrors[draw_mirror_position++];
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
