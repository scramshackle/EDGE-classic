//----------------------------------------------------------------------------
//  EDGE Sight Code
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
//  -AJA- 2001/07/24: New sight code.
//
//  Works like this: First we do what the original DOOM source did,
//  traverse the BSP to find lines that intersecting the LOS ray.  We
//  keep the top/bottom slope optimisation too.
//
//  The difference is that we remember where abouts the intercepts
//  occur, and if the basic LOS check succeeds (e.g. no one-sided
//  lines blocking view) then we use the intercept list to check for
//  extrafloors that block the view.
//

#include <math.h>

#include <algorithm>
#include <vector>

#include "epi_math.h"
#include "dm_defs.h"
#include "epi.h"
#include "epi_doomdefs.h"
#include "m_bbox.h"
#include "p_local.h"
#include "r_misc.h"
#include "r_state.h"

#define EDGE_DEBUG_SIGHT 0

struct LineOfSight
{
    // source position (dx/dy is vector to dest)
    DividingLine source;
    float        source_z;
    Sector      *source_sector;

    // dest position
    HMM_Vec2 destination;
    float    destination_z;
    Sector  *destination_sector;

    BAMAngle angle;

    // slopes from source to top/bottom of destination.  They will be
    // updated when one or two-sided lines are encountered.  If they
    // close up completely, then no other heights need to be checked.
    //
    // NOTE: the values are not real slopes, the distance from src to
    //       dest is the implied denominator.
    //
    float top_slope;
    float bottom_slope;

    // bounding box on LOS line (idea pinched from PrBOOM).
    float bounding_box[4];

    // true if one of the sectors contained extrafloors
    bool saw_extrafloors;

    // true if one of the sectors contained vertex slopes
    bool saw_vertex_slopes;
};

static LineOfSight sight_check;

// intercepts found during first pass

struct WallIntercept
{
    // fractional distance, 0.0 -> 1.0
    float along;

    // sector that faces the source from this intercept point
    Sector *sector;
};

// intercept array
static std::vector<WallIntercept> wall_intercepts;

static inline void AddSightIntercept(float frac, Sector *sec)
{
    WallIntercept WI;

    WI.along  = frac;
    WI.sector = sec;

    wall_intercepts.push_back(WI);
}

struct SightCrossing
{
    float along;
    Line *line;
};

static std::vector<SightCrossing> sight_crossings;

static bool SightCrossingLess(const SightCrossing &a, const SightCrossing &b)
{
    return a.along < b.along;
}

static bool SightCollectLine(Line *ld, void *data)
{
    EPI_UNUSED(data);

    if (ld->bounding_box[kBoundingBoxLeft] > sight_check.bounding_box[kBoundingBoxRight] ||
        ld->bounding_box[kBoundingBoxRight] < sight_check.bounding_box[kBoundingBoxLeft] ||
        ld->bounding_box[kBoundingBoxBottom] > sight_check.bounding_box[kBoundingBoxTop] ||
        ld->bounding_box[kBoundingBoxTop] < sight_check.bounding_box[kBoundingBoxBottom])
        return true;

    int s1 = PointOnDividingLineSide(ld->vertex_1->X, ld->vertex_1->Y, &sight_check.source);
    int s2 = PointOnDividingLineSide(ld->vertex_2->X, ld->vertex_2->Y, &sight_check.source);

    if (s1 == s2)
        return true;

    DividingLine divl;

    divl.x       = ld->vertex_1->X;
    divl.y       = ld->vertex_1->Y;
    divl.delta_x = ld->delta_x;
    divl.delta_y = ld->delta_y;

    s1 = PointOnDividingLineSide(sight_check.source.x, sight_check.source.y, &divl);
    s2 = PointOnDividingLineSide(sight_check.destination.X, sight_check.destination.Y, &divl);

    if (s1 == s2)
        return true;

    float den = divl.delta_y * sight_check.source.delta_x - divl.delta_x * sight_check.source.delta_y;

    if (epi::AlmostEquals(den, 0.0f))
        return true;

    float num = (divl.x - sight_check.source.x) * divl.delta_y + (sight_check.source.y - divl.y) * divl.delta_x;

    float along = num / den;

    if (epi::AlmostEquals(along, 0.0f))
        return true;

    sight_crossings.push_back(SightCrossing{along, ld});

    return true;
}

static void SightNoteSector(const Sector *sec)
{
    if (sec->extrafloor_used > 0)
        sight_check.saw_extrafloors = true;

    if (sec->floor_vertex_slope || sec->ceiling_vertex_slope)
        sight_check.saw_vertex_slopes = true;
}

static bool CheckSightLines(void)
{
    sight_crossings.clear();

    BlockmapSegmentLineIterator(sight_check.source.x, sight_check.source.y, sight_check.destination.X,
                                sight_check.destination.Y, SightCollectLine);

    std::sort(sight_crossings.begin(), sight_crossings.end(), SightCrossingLess);

    SightNoteSector(sight_check.source_sector);

    for (size_t i = 0; i < sight_crossings.size(); i++)
    {
        Line *ld    = sight_crossings[i].line;
        float along = sight_crossings[i].along;

        // stop because it is not two sided anyway
        if (!(ld->flags & kLineFlagTwoSided) || ld->blocked)
            return false;

        // line explicitly blocks sight ?  (XDoom compatibility)
        if (ld->flags & kLineFlagSightBlock)
            return false;

        // -AJA- 2001/11/11: closed Sliding door ?
        if (ld->slide_door && !ld->slide_door->s_.see_through_ && !ld->slider_move)
            return false;

        DividingLine divl;

        divl.x       = ld->vertex_1->X;
        divl.y       = ld->vertex_1->Y;
        divl.delta_x = ld->delta_x;
        divl.delta_y = ld->delta_y;

        bool source_behind = PointOnDividingLineSide(sight_check.source.x, sight_check.source.y, &divl) != 0;

        Sector *front = source_behind ? ld->back_sector : ld->front_sector;
        Sector *back  = source_behind ? ld->front_sector : ld->back_sector;

        EPI_ASSERT(front && back);

        SightNoteSector(front);
        SightNoteSector(back);

        if (!epi::AlmostEquals(front->floor_height, back->floor_height))
        {
            float openbottom = HMM_MAX(ld->front_sector->floor_height, ld->back_sector->floor_height);
            float slope      = (openbottom - sight_check.source_z) / along;
            if (slope > sight_check.bottom_slope)
                sight_check.bottom_slope = slope;
        }

        if (!epi::AlmostEquals(front->ceiling_height, back->ceiling_height))
        {
            float opentop = HMM_MIN(ld->front_sector->ceiling_height, ld->back_sector->ceiling_height);
            float slope   = (opentop - sight_check.source_z) / along;
            if (slope < sight_check.top_slope)
                sight_check.top_slope = slope;
        }

        // did our slope range close up ?
        if (sight_check.top_slope <= sight_check.bottom_slope)
            return false;

        AddSightIntercept(along, front);
    }

    SightNoteSector(sight_check.destination_sector);

    AddSightIntercept(1.0f, sight_check.destination_sector);

    return true;
}

//
// CheckSightIntercepts
//
// Returns false if LOS is blocked by extrafloors, otherwise true.
//
static bool CheckSightIntercepts(float slope)
{
    int     i, j;
    Sector *sec;

    float last_h = sight_check.source_z;
    float cur_h;

#if (EDGE_DEBUG_SIGHT >= 1)
    LogDebug("INTERCEPTS  slope %1.0f\n", slope);
#endif

    for (i = 0; i < (int)wall_intercepts.size(); i++, last_h = cur_h)
    {
        bool blocked = true;

        cur_h = sight_check.source_z + slope * wall_intercepts[i].along;

#if (EDGE_DEBUG_SIGHT >= 1)
        LogDebug("  %d/%d  FRAC %1.4f  SEC %d  H=%1.4f/%1.4f\n", i + 1, wall_intercepts.size(),
                 wall_intercepts[i].along, wall_intercepts[i].sector - sectors, last_h, cur_h);
#endif

        // check all the sight gaps.
        sec = wall_intercepts[i].sector;

        for (j = 0; j < sec->sight_gap_number; j++)
        {
            float z1 = sec->sight_gaps[j].floor;
            float z2 = sec->sight_gaps[j].ceiling;

#if (EDGE_DEBUG_SIGHT >= 3)
            LogDebug("    SIGHT GAP [%d] = %1.1f .. %1.1f\n", j, z1, z2);
#endif

            if (z1 <= last_h && last_h <= z2 && z1 <= cur_h && cur_h <= z2)
            {
                blocked = false;
                break;
            }
        }

        if (blocked)
            return false;
    }

    return true;
}

//
// CheckSightSameSubsector
//
// When the subsector is the same, we only need to check whether a
// non-SeeThrough extrafloor gets in the way.
//
static bool CheckSightSameSector(MapObject *src, MapObject *dest)
{
    int     j;
    Sector *sec;

    float lower_z;
    float upper_z;

    if (sight_check.source_z < dest->z)
    {
        lower_z = sight_check.source_z;
        upper_z = dest->z;
    }
    else if (sight_check.source_z > dest->z + dest->height_)
    {
        lower_z = dest->z + dest->height_;
        upper_z = sight_check.source_z;
    }
    else
    {
        return true;
    }

    // check all the sight gaps.
    sec = src->sector_;

    for (j = 0; j < sec->sight_gap_number; j++)
    {
        float z1 = sec->sight_gaps[j].floor;
        float z2 = sec->sight_gaps[j].ceiling;

        if (z1 <= lower_z && upper_z <= z2)
            return true;
    }

    return false;
}

bool CheckSight(MapObject *src, MapObject *dest)
{
    if (!dest)
        return false;

    // -ACB- 1998/07/20 t2 is Invisible, t1 cannot possibly see it.
    if (epi::AlmostEquals(dest->visibility_, 0.0f))
        return false;

    int n, num_div;

    float dest_heights[5];
    float dist_a;

    // First check for trivial rejection.

    EPI_ASSERT(src->sector_);
    EPI_ASSERT(dest->sector_);

    // An unobstructed LOS is possible.
    // Now look from eyes of t1 to any part of t2.

    // The "eyes" of a thing is 75% of its height.
    EPI_ASSERT(src->info_);
    sight_check.source_z = src->z + src->height_ * src->info_->viewheight_;

    sight_check.source.x         = src->x;
    sight_check.source.y         = src->y;
    sight_check.source.delta_x   = dest->x - src->x;
    sight_check.source.delta_y = dest->y - src->y;
    sight_check.source_sector  = src->sector_;

    sight_check.destination.X      = dest->x;
    sight_check.destination.Y      = dest->y;
    sight_check.destination_sector = dest->sector_;

    sight_check.bottom_slope = dest->z - sight_check.source_z;
    sight_check.top_slope    = sight_check.bottom_slope + dest->height_;

    // destination out of object's DDF slope range ?
    dist_a = ApproximateDistance(sight_check.source.delta_x, sight_check.source.delta_y);

    if (src->info_->sight_distance_ > -1) // if we have sight_distance set
    {
        if (src->info_->sight_distance_ < dist_a)
        {
            // src->SetTarget(nullptr); //forget we even saw the guy?
            return false; // too far away for this thing to see
        }
    }


    if (sight_check.top_slope < dist_a * -src->info_->sight_slope_)
        return false;

    if (sight_check.bottom_slope > dist_a * src->info_->sight_slope_)
        return false;

    sight_check.angle =
        PointToAngle(sight_check.source.x, sight_check.source.y, sight_check.destination.X, sight_check.destination.Y);

    sight_check.bounding_box[kBoundingBoxLeft]   = HMM_MIN(sight_check.source.x, sight_check.destination.X);
    sight_check.bounding_box[kBoundingBoxRight]  = HMM_MAX(sight_check.source.x, sight_check.destination.X);
    sight_check.bounding_box[kBoundingBoxBottom] = HMM_MIN(sight_check.source.y, sight_check.destination.Y);
    sight_check.bounding_box[kBoundingBoxTop]    = HMM_MAX(sight_check.source.y, sight_check.destination.Y);

    wall_intercepts.clear(); // FIXME

    sight_check.saw_extrafloors   = false;
    sight_check.saw_vertex_slopes = false;

    // initial pass -- check for basic blockage & create intercepts
    if (!CheckSightLines())
        return false;

    // -AJA- handle the case where no linedefs are crossed
    if (sight_crossings.empty())
        return CheckSightSameSector(src, dest);

    // no extrafloors or vertslopes encountered ?  Then the checks made by
    // CheckSightBSP are sufficient.  (-AJA- double check this)
    //
    if (!sight_check.saw_extrafloors && !sight_check.saw_vertex_slopes)
        return true;

    // Leveraging the existing hitscan attack code is easier than trying to
    // wrangle this stuff
    if (sight_check.saw_vertex_slopes)
    {
        float objslope;
        AimLineAttack(src, sight_check.angle, 64000, &objslope);
        LineAttack(src, sight_check.angle, 64000, objslope, 0, nullptr, nullptr, nullptr);
        bool slope_sight_good = dest->slope_sight_hit_;
        if (slope_sight_good)
        {
            dest->slope_sight_hit_ = false; // reset for future sight checks
            return true;
        }
        else
            return false;
    }

    // Enter the HackMan...  The new sight code only tests LOS to one
    // destination height.  (The old code kept track of angles -- but
    // this approach is not well suited for extrafloors).  The number of
    // points we test depends on the destination: 5 for players, 3 for
    // monsters, 1 for everything else.

    if (dest->player_)
    {
        num_div         = 5;
        dest_heights[0] = dest->z;
        dest_heights[1] = dest->z + dest->height_ * 0.25f;
        dest_heights[2] = dest->z + dest->height_ * 0.50f;
        dest_heights[3] = dest->z + dest->height_ * 0.75f;
        dest_heights[4] = dest->z + dest->height_;
    }
    else if (dest->extended_flags_ & kExtendedFlagMonster)
    {
        num_div         = 3;
        dest_heights[0] = dest->z;
        dest_heights[1] = dest->z + dest->height_ * 0.5f;
        dest_heights[2] = dest->z + dest->height_;
    }
    else
    {
        num_div         = 1;
        dest_heights[0] = dest->z + dest->height_ * 0.5f;
    }

    // use intercepts to check extrafloor heights
    //
    for (n = 0; n < num_div; n++)
    {
        float slope = dest_heights[n] - sight_check.source_z;

        if (slope > sight_check.top_slope || slope < sight_check.bottom_slope)
            continue;

        if (CheckSightIntercepts(slope))
            return true;
    }

    return false;
}

bool CheckSightToPoint(MapObject *src, float x, float y, float z)
{
    sight_check.source.x       = src->x;
    sight_check.source.y       = src->y;
    sight_check.source_z       = src->z + src->height_ * src->info_->viewheight_;
    sight_check.source.delta_x = x - src->x;
    sight_check.source.delta_y = y - src->y;
    sight_check.source_sector  = src->sector_;

    sight_check.destination.X      = x;
    sight_check.destination.Y      = y;
    sight_check.destination_z      = z;
    sight_check.destination_sector = PointInSector(x, y);

    sight_check.bottom_slope = z - 1.0f - sight_check.source_z;
    sight_check.top_slope    = z + 1.0f - sight_check.source_z;

    sight_check.angle =
        PointToAngle(sight_check.source.x, sight_check.source.y, sight_check.destination.X, sight_check.destination.Y);

    sight_check.bounding_box[kBoundingBoxLeft]   = HMM_MIN(sight_check.source.x, sight_check.destination.X);
    sight_check.bounding_box[kBoundingBoxRight]  = HMM_MAX(sight_check.source.x, sight_check.destination.X);
    sight_check.bounding_box[kBoundingBoxBottom] = HMM_MIN(sight_check.source.y, sight_check.destination.Y);
    sight_check.bounding_box[kBoundingBoxTop]    = HMM_MAX(sight_check.source.y, sight_check.destination.Y);

    wall_intercepts.clear();

    sight_check.saw_extrafloors   = false;
    sight_check.saw_vertex_slopes = false;

    if (!CheckSightLines())
        return false;

    if (sight_crossings.empty())
        return true;

#if 1
    if (!sight_check.saw_extrafloors)
        return true;
#endif

    float slope = z - sight_check.source_z;

    if (slope > sight_check.top_slope || slope < sight_check.bottom_slope)
        return false;

    return CheckSightIntercepts(slope);
}

//
// P_CheckSightApproxVert
//
// Quickly check that object t1 can vertically see object t2.  Only
// takes extrafloors into account.  Mainly used so that archviles
// don't resurrect monsters that are completely out of view in another
// vertical region.  Returns true if sight possible, false otherwise.
//
bool QuickVerticalSightCheck(MapObject *src, MapObject *dest)
{
    EPI_ASSERT(src->info_);

    sight_check.source_z = src->z + src->height_ * src->info_->viewheight_;

    return CheckSightSameSector(src, dest);
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
