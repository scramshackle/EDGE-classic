//----------------------------------------------------------------------------
//  EDGE OpenGL Rendering (BSP Traversal)
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

#include <math.h>

#include <algorithm>

#include <unordered_map>
#include <unordered_set>

#include "epi_math.h"
#include "dm_defs.h"
#include "dm_state.h"
#include "epi.h"
#include "edge_profiling.h"
#include "epi_doomdefs.h"
#include "epi_simd.h"
#include "g_game.h"
#include "i_defs_gl.h"
#include "i_system.h"
#include "m_bbox.h"
#include "p_local.h"
#include "p_tick.h"
#include "r_backend.h"
#include "r_colormap.h"
#include "r_defs.h"
#include "r_effects.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_polygon.h"
#include "r_render.h"
#include "r_shader.h"
#include "r_sky.h"
#include "r_state.h"
#include "r_static.h"
#include "r_things.h"
#include "r_units.h"

std::list<DrawSector *> draw_sector_list;
std::list<DrawThing *>  draw_thing_list;
std::list<DrawMirror *> draw_mirror_list;

MirrorSet active_mirror_set;

EDGE_DEFINE_CONSOLE_VARIABLE(debug_hall_of_mirrors, "0", kConsoleVariableFlagCheat)

extern ConsoleVariable draw_culling;

// -ES- 1999/03/20 Different right & left side clip angles, for asymmetric FOVs.
BAMAngle clip_left, clip_right;
BAMAngle clip_scope;

MapObject *view_camera_map_object;

ViewHeightZone view_height_zone;

static void EmitSkyWall(LineSide *line_side, float h1, float h2, Sector *sky_owner, int part, DrawMirror *mir,
                        bool resident)
{
    if (!mir && resident != SkyWallBakeable(line_side, sky_owner))
        return;

    RenderSkyWall(line_side, h1, h2, sky_owner, part, mir);
}

void SkyDecideLineSide(LineSide *line_side, DrawMirror *mir, bool resident)
{
    Sector *fsector = line_side->front_sector;
    Sector *bsector = line_side->back_sector;

    float             f_fh    = 0;
    float             f_ch    = 0;
    float             b_fh    = 0;
    float             b_ch    = 0;
    const MapSurface *f_floor = nullptr;
    const MapSurface *f_ceil  = nullptr;
    const MapSurface *b_floor = nullptr;
    const MapSurface *b_ceil  = nullptr;

    if (!fsector->height_sector)
    {
        f_fh    = fsector->interpolated_floor_height;
        f_floor = &fsector->floor;
        f_ch    = fsector->interpolated_ceiling_height;
        f_ceil  = &fsector->ceiling;
    }
    else
    {
        if (view_height_zone == kHeightZoneA && view_z > fsector->height_sector->interpolated_ceiling_height)
        {
            f_fh    = fsector->height_sector->interpolated_ceiling_height;
            f_ch    = fsector->interpolated_ceiling_height;
            f_floor = &fsector->height_sector->floor;
            f_ceil  = &fsector->height_sector->ceiling;
        }
        else if (view_height_zone == kHeightZoneC && view_z < fsector->height_sector->interpolated_floor_height)
        {
            f_fh    = fsector->interpolated_floor_height;
            f_ch    = fsector->height_sector->interpolated_floor_height;
            f_floor = &fsector->height_sector->floor;
            f_ceil  = &fsector->height_sector->ceiling;
        }
        else
        {
            f_fh    = fsector->height_sector->interpolated_floor_height;
            f_ch    = fsector->height_sector->interpolated_ceiling_height;
            f_floor = &fsector->floor;
            f_ceil  = &fsector->ceiling;
        }
    }

    if (bsector)
    {
        if (!bsector->height_sector)
        {
            b_fh    = bsector->interpolated_floor_height;
            b_floor = &bsector->floor;
            b_ch    = bsector->interpolated_ceiling_height;
            b_ceil  = &bsector->ceiling;
        }
        else
        {
            if (view_height_zone == kHeightZoneA && view_z > bsector->height_sector->interpolated_ceiling_height)
            {
                b_fh    = bsector->height_sector->interpolated_ceiling_height;
                b_ch    = bsector->interpolated_ceiling_height;
                b_floor = &bsector->height_sector->floor;
                b_ceil  = &bsector->height_sector->ceiling;
            }
            else if (view_height_zone == kHeightZoneC && view_z < bsector->height_sector->interpolated_floor_height)
            {
                b_fh    = bsector->interpolated_floor_height;
                b_ch    = bsector->height_sector->interpolated_floor_height;
                b_floor = &bsector->height_sector->floor;
                b_ceil  = &bsector->height_sector->ceiling;
            }
            else
            {
                b_fh    = bsector->height_sector->interpolated_floor_height;
                b_ch    = bsector->height_sector->interpolated_ceiling_height;
                b_floor = &bsector->floor;
                b_ceil  = &bsector->ceiling;
            }
        }
    }

    if (bsector && EDGE_IMAGE_IS_SKY(*f_floor) && EDGE_IMAGE_IS_SKY(*b_floor) &&
        line_side->sidedef->bottom.image == nullptr)
    {
        if (f_fh < b_fh)
        {
            EmitSkyWall(line_side, f_fh, b_fh, fsector, 0, mir, resident);
        }
    }

    if (EDGE_IMAGE_IS_SKY(*f_ceil))
    {
        if (f_ch < fsector->sky_height && (!bsector || !EDGE_IMAGE_IS_SKY(*b_ceil) || b_fh >= f_ch))
        {
            EmitSkyWall(line_side, f_ch, fsector->sky_height, fsector, 1, mir, resident);
        }
        else if (bsector && EDGE_IMAGE_IS_SKY(*b_ceil))
        {
            float max_f = HMM_MAX(f_fh, b_fh);

            if (b_ch <= max_f && max_f < fsector->sky_height)
            {
                EmitSkyWall(line_side, max_f, fsector->sky_height, fsector, 1, mir, resident);
            }
        }
    }
    // -AJA- 2004/08/29: Emulate Sky-Flooding TRICK
    else if (!debug_hall_of_mirrors.d_ && bsector && EDGE_IMAGE_IS_SKY(*b_ceil) &&
             line_side->sidedef->top.image == nullptr && b_ch < f_ch)
    {
        EmitSkyWall(line_side, b_ch, f_ch, bsector, 2, mir, resident);
    }
}

static bool PointPairViewAngles(float sx1, float sy1, float sx2, float sy2, bool precise,
                                BAMAngle *out_left, BAMAngle *out_right);

static bool LineSideViewAngles(const LineSide *line_side, BAMAngle *out_left, BAMAngle *out_right)
{
    float sx1 = line_side->vertex_1->X;
    float sy1 = line_side->vertex_1->Y;

    float sx2 = line_side->vertex_2->X;
    float sy2 = line_side->vertex_2->Y;

    // when there are active mirror planes, segs not only need to
    // be flipped across them but also clipped across them.

    int32_t active_mirrors = active_mirror_set.TotalActive();
    if (active_mirrors > 0)
    {
        for (int i = active_mirrors - 1; i >= 0; i--)
        {
            active_mirror_set.Transform(i, sx1, sy1);
            active_mirror_set.Transform(i, sx2, sy2);

            if (!active_mirror_set.IsPortal(i))
            {
                float tmp_x = sx1;
                sx1         = sx2;
                sx2         = tmp_x;
                float tmp_y = sy1;
                sy1         = sy2;
                sy2         = tmp_y;
            }

            LineSide *clipper = active_mirror_set.GetLineSide(i);

            DividingLine div;

            div.x       = clipper->vertex_1->X;
            div.y       = clipper->vertex_1->Y;
            div.delta_x = clipper->vertex_2->X - div.x;
            div.delta_y = clipper->vertex_2->Y - div.y;

            int s1 = PointOnDividingLineSide(sx1, sy1, &div);
            int s2 = PointOnDividingLineSide(sx2, sy2, &div);

            // seg lies completely in front of clipper?
            if (s1 == 0 && s2 == 0)
                return false;

            if (s1 != s2)
            {
                // seg crosses clipper, need to split it
                float ix, iy;

                ComputeIntersection(&div, sx1, sy1, sx2, sy2, &ix, &iy);

                if (s2 == 0)
                    sx2 = ix, sy2 = iy;
                else
                    sx1 = ix, sy1 = iy;
            }
        }
    }

    bool precise = active_mirrors > 0;
    if (!precise)
    {
        precise = (line_side->linedef->flags & kLineFlagMirror) || (line_side->linedef->portal_pair);
    }

    return PointPairViewAngles(sx1, sy1, sx2, sy2, precise, out_left, out_right);
}

static bool PointPairViewAngles(float sx1, float sy1, float sx2, float sy2, bool precise, BAMAngle *out_left,
                                BAMAngle *out_right)
{
    BAMAngle angle_L = PointToAngle(view_x, view_y, sx1, sy1, precise);
    BAMAngle angle_R = PointToAngle(view_x, view_y, sx2, sy2, precise);

    // Clip to view edges.

    BAMAngle span = angle_L - angle_R;

    // back side ?
    if (span >= kBAMAngle180)
    {
        return false;
    }

    angle_L -= view_angle;
    angle_R -= view_angle;

    if (clip_scope != kBAMAngle180)
    {
        BAMAngle tspan1 = angle_L - clip_right;
        BAMAngle tspan2 = clip_left - angle_R;

        if (tspan1 > clip_scope)
        {
            // Totally off the left edge?
            if (tspan2 >= kBAMAngle180)
            {
                return false;
            }

            angle_L = clip_left;
        }

        if (tspan2 > clip_scope)
        {
            // Totally off the left edge?
            if (tspan1 >= kBAMAngle180)
            {
                return false;
            }

            angle_R = clip_right;
        }

        span = angle_L - angle_R;
    }

    *out_left  = angle_L;
    *out_right = angle_R;

    return true;
}


static void VisitLineSide(DrawSector *dsector, LineSide *line_side)
{
    if (active_mirror_set.LineSideOnPortal(line_side))
        return;

    BAMAngle angle_L = 0;
    BAMAngle angle_R = 0;

    if (!LineSideViewAngles(line_side, &angle_L, &angle_R))
        return;

    if (angle_L - angle_R == 0)
        return;

    if (active_mirror_set.TotalActive() < kMaximumMirrors &&
        ((line_side->linedef->flags & kLineFlagMirror) || line_side->linedef->portal_pair))
        return;

    dsector->line_sides.push_back(line_side);

    SkyDecideLineSide(line_side, active_mirror_set.InnermostMirror(), false);
}

static inline void AddNewDrawFloor(DrawSector *dsector, Extrafloor *ef, float floor_height, float ceiling_height,
                                   float top_h, MapSurface *floor, MapSurface *ceil, RegionProperties *props,
                                   Extrafloor *floor_ef)
{
    DrawFloor *dfloor;


    dfloor = GetDrawFloor();

    dfloor->is_highest      = false;
    dfloor->is_lowest       = false;
    dfloor->render_next     = nullptr;
    dfloor->render_previous = nullptr;
    dfloor->floor           = nullptr;
    dfloor->ceiling         = nullptr;
    dfloor->extrafloor      = nullptr;
    dfloor->floor_extrafloor = nullptr;
    dfloor->properties      = nullptr;

    dfloor->floor_height   = floor_height;
    dfloor->ceiling_height = ceiling_height;
    dfloor->top_height     = top_h;
    dfloor->floor          = floor;
    dfloor->ceiling        = ceil;
    dfloor->extrafloor       = ef;
    dfloor->floor_extrafloor = floor_ef;
    dfloor->properties       = props;

    // link it in, height order

    dsector->floors.push_back(dfloor);

    // link it in, rendering order (very important)

    if (dsector->render_floors == nullptr || floor_height > view_z)
    {
        // add to head
        dfloor->render_next     = dsector->render_floors;
        dfloor->render_previous = nullptr;

        if (dsector->render_floors)
            dsector->render_floors->render_previous = dfloor;

        dsector->render_floors = dfloor;
    }
    else
    {
        // add to tail
        DrawFloor *tail;

        for (tail = dsector->render_floors; tail->render_next; tail = tail->render_next)
        { /* nothing here */
        }

        dfloor->render_next     = nullptr;
        dfloor->render_previous = tail;

        tail->render_next = dfloor;
    }
}

static void EmitSkyPlane(Sector *sector, float h, Sector *sky_owner, int face, DrawMirror *mir, bool resident)
{
    if (!mir && !resident)
        return;

    RenderSkyPlane(sector, h, sky_owner, face, mir);
}

void SkyDecideSector(Sector *sector, DrawMirror *mir, bool resident)
{
    if (!sector->height_sector)
    {
        if (EDGE_IMAGE_IS_SKY(sector->floor) && view_z > sector->interpolated_floor_height)
        {
            EmitSkyPlane(sector, sector->interpolated_floor_height, sector, 1, mir, resident);
        }

        if (EDGE_IMAGE_IS_SKY(sector->ceiling) && view_z < sector->sky_height)
        {
            EmitSkyPlane(sector, sector->sky_height, sector, 0, mir, resident);
        }

        return;
    }

    float floor_h = sector->interpolated_floor_height;

    MapSurface *floor_s = &sector->floor;
    MapSurface *ceil_s  = &sector->ceiling;

    if (view_height_zone == kHeightZoneA && view_z > sector->height_sector->interpolated_ceiling_height)
    {
        floor_h = sector->height_sector->interpolated_ceiling_height;
        floor_s = &sector->height_sector->floor;
        ceil_s  = &sector->height_sector->ceiling;
    }
    else if (view_height_zone == kHeightZoneC && view_z < sector->height_sector->interpolated_floor_height)
    {
        floor_h = sector->interpolated_floor_height;
        floor_s = &sector->height_sector->floor;
        ceil_s  = &sector->height_sector->ceiling;
    }
    else
    {
        floor_h = sector->height_sector->interpolated_floor_height;
    }

    if (EDGE_IMAGE_IS_SKY(*floor_s) && view_z > floor_h)
    {
        EmitSkyPlane(sector, floor_h, sector->height_sector, 1, mir, resident);
    }

    if (EDGE_IMAGE_IS_SKY(*ceil_s) && view_z < sector->sky_height)
    {
        EmitSkyPlane(sector, sector->sky_height, sector->height_sector, 0, mir, resident);
    }
}

static bool LineSideIsMirrorCandidate(const LineSide *line_side)
{
    if (!line_side->sidedef)
        return false;

    return (line_side->linedef->flags & kLineFlagMirror) || line_side->linedef->portal_pair;
}

static std::vector<int32_t> mirror_candidates;
static const LineSide      *mirror_candidate_base  = nullptr;
static int                  mirror_candidate_count = 0;

static void RefreshMirrorCandidates(void)
{
    if (mirror_candidate_base == level_line_sides && mirror_candidate_count == total_level_lines)
        return;

    mirror_candidate_base  = level_line_sides;
    mirror_candidate_count = total_level_lines;

    mirror_candidates.clear();

    for (int i = 0; i < total_level_lines * 2; i++)
    {
        if (LineSideIsMirrorCandidate(&level_line_sides[i]))
            mirror_candidates.push_back(i);
    }
}

void EnumerateViewMirrors(void)
{
    EDGE_ZoneScoped;

    if (active_mirror_set.TotalActive() >= kMaximumMirrors)
        return;

    RefreshMirrorCandidates();

    for (size_t c = 0; c < mirror_candidates.size(); c++)
    {
        LineSide *line_side = &level_line_sides[mirror_candidates[c]];

        if (active_mirror_set.LineSideOnPortal(line_side))
            continue;

        BAMAngle left  = 0;
        BAMAngle right = 0;

        if (!LineSideViewAngles(line_side, &left, &right))
            continue;

        if (left - right == 0)
            continue;

        bool is_portal = (line_side->linedef->flags & kLineFlagMirror) ? false : true;

        DrawMirror *mir = GetDrawMirror();

        mir->line_side = line_side;
        mir->draw_sectors.clear();
        mir->draw_things.clear();
        mir->draw_mirrors.clear();

        mir->left      = view_angle + left;
        mir->right     = view_angle + right;
        mir->is_portal = is_portal;

        int32_t enclosing = active_mirror_set.TotalActive();

        if (enclosing > 0)
            active_mirror_set.PushMirror(enclosing - 1, mir);
        else
            draw_mirror_list.push_back(mir);

        active_mirror_set.Push(mir);

        BAMAngle save_clip_L = clip_left;
        BAMAngle save_clip_R = clip_right;
        BAMAngle save_scope  = clip_scope;

        clip_left  = left;
        clip_right = right;
        clip_scope = left - right;

        EnumerateViewSectors();

        EnumerateViewThings();

        EnumerateViewMirrors();

        clip_left  = save_clip_L;
        clip_right = save_clip_R;
        clip_scope = save_scope;

        active_mirror_set.Pop();
    }
}

void EnumerateViewSky(void)
{
    EDGE_ZoneScoped;

    for (int i = 0; i < total_level_sectors; i++)
    {
        SkyDecideSector(&level_sectors[i], nullptr, true);
    }

    for (int i = 0; i < total_level_lines * 2; i++)
    {
        LineSide *line_side = &level_line_sides[i];

        if (!line_side->sidedef)
            continue;

        if ((line_side->linedef->flags & kLineFlagMirror) || line_side->linedef->portal_pair)
            continue;

        SkyDecideLineSide(line_side, nullptr, true);
    }
}

static bool SectorBeyondFarClip(const Sector *sector)
{
    const SectorPolygon *poly = SectorPolygonForSector((int)(sector - level_sectors));

    if (!poly || poly->bounds[0] > poly->bounds[2] || poly->bounds[1] > poly->bounds[3])
        return false;

    float dx = 0.0f;
    float dy = 0.0f;

    if (view_x < poly->bounds[0])
        dx = poly->bounds[0] - view_x;
    else if (view_x > poly->bounds[2])
        dx = view_x - poly->bounds[2];

    if (view_y < poly->bounds[1])
        dy = poly->bounds[1] - view_y;
    else if (view_y > poly->bounds[3])
        dy = view_y - poly->bounds[3];

    float limit = renderer_far_clip.f_ + 500.0f;

    return (dx * dx + dy * dy) > limit * limit;
}

static void VisitSector(Sector *sector)
{
    if (draw_culling.d_ && SectorBeyondFarClip(sector))
        return;

    DrawSector *K    = GetDrawSector();
    K->sector        = sector;
    K->render_floors = nullptr;

    K->floors.clear();
    K->line_sides.clear();

    SkyDecideSector(sector, active_mirror_set.InnermostMirror(), false);

    float floor_h = sector->interpolated_floor_height;
    float ceil_h  = sector->interpolated_ceiling_height;

    MapSurface *floor_s = &sector->floor;
    MapSurface *ceil_s  = &sector->ceiling;

    RegionProperties *props = sector->active_properties;

    // Boom compatibility -- deep water FX
    if (sector->height_sector != nullptr)
    {
        if (view_height_zone == kHeightZoneA && view_z > sector->height_sector->interpolated_ceiling_height)
        {
            floor_h = sector->height_sector->interpolated_ceiling_height;
            ceil_h  = sector->interpolated_ceiling_height;
            floor_s = &sector->height_sector->floor;
            ceil_s  = &sector->height_sector->ceiling;
            props   = sector->height_sector->active_properties;
        }
        else if (view_height_zone == kHeightZoneC && view_z < sector->height_sector->interpolated_floor_height)
        {
            floor_h = sector->interpolated_floor_height;
            ceil_h  = sector->height_sector->interpolated_floor_height;
            floor_s = &sector->height_sector->floor;
            ceil_s  = &sector->height_sector->ceiling;
            props   = sector->height_sector->active_properties;
        }
        else
        {
            floor_h = sector->height_sector->interpolated_floor_height;
            ceil_h  = sector->height_sector->interpolated_ceiling_height;
        }
    }
    // -AJA- 2004/04/22: emulate the Deep-Water TRICK
    else if (sector->deep_water_reference != nullptr)
    {
        floor_h = sector->deep_water_reference->interpolated_floor_height;
        floor_s = &sector->deep_water_reference->floor;

        ceil_h = sector->deep_water_reference->interpolated_ceiling_height;
        ceil_s = &sector->deep_water_reference->ceiling;
    }

    // the OLD method of Boom deep water (the BOOMTEX flag)
    Extrafloor *boom_ef = sector->bottom_liquid ? sector->bottom_liquid : sector->bottom_extrafloor;
    if (boom_ef && (boom_ef->extrafloor_definition->type_ & kExtraFloorTypeBoomTex))
        floor_s = &boom_ef->extrafloor_line->front_sector->floor;

    // add in each extrafloor, traversing strictly upwards

    Extrafloor *floor_ef = nullptr;

    Extrafloor *S = sector->bottom_extrafloor;
    Extrafloor *L = sector->bottom_liquid;

    while (S || L)
    {
        Extrafloor *C = nullptr;

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
        if (C->bottom_height < floor_h || C->bottom_height > sector->interpolated_ceiling_height)
            continue;

        AddNewDrawFloor(K, C, floor_h, C->bottom_height, C->top_height, floor_s, C->bottom, C->properties, floor_ef);

        floor_s  = C->top;
        floor_h  = C->top_height;
        floor_ef = C;
    }

    AddNewDrawFloor(K, nullptr, floor_h, ceil_h, ceil_h, floor_s, ceil_s, props, floor_ef);

    K->floors[0]->is_lowest                     = true;
    K->floors[K->floors.size() - 1]->is_highest = true;

    for (int i = 0; i < sector->line_count; i++)
    {
        Line *line = sector->lines[i];

        for (int side = 0; side < 2; side++)
        {
            LineSide *line_side = &level_line_sides[(line - level_lines) * 2 + side];

            if (line_side->sidedef && line_side->front_sector == sector)
                VisitLineSide(K, line_side);
        }
    }

    int32_t active_mirrors = active_mirror_set.TotalActive();

    if (active_mirrors > 0)
        active_mirror_set.PushSector(active_mirrors - 1, K);
    else
        draw_sector_list.push_back(K);
}

void EnumerateViewSectors(void)
{
    for (int i = 0; i < total_level_sectors; i++)
    {
        VisitSector(&level_sectors[i]);
    }
}
