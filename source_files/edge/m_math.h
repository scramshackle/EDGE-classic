//----------------------------------------------------------------------------
//  EDGE Floating Point Math Stuff
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

#pragma once

#include "epi_bam.h"
#include "epi_vector.h"

float     PointToSegDistance(epi::Vec2 seg_a, epi::Vec2 seg_b, epi::Vec2 point);
epi::Vec3 TripleCrossProduct(epi::Vec3 v1, epi::Vec3 v2, epi::Vec3 v3);
epi::Vec3 LinePlaneIntersection(epi::Vec3 line_a, epi::Vec3 line_b, epi::Vec3 plane_c, epi::Vec3 plane_normal);
epi::Vec3 LinePlaneIntersection(epi::Vec3 line_a, epi::Vec3 line_b, epi::Vec3 plane_a, epi::Vec3 plane_b,
                                epi::Vec3 plane_c);
void      BAMAngleToMatrix(BAMAngle ang, epi::Vec2 *x, epi::Vec2 *y);
int       PointInTriangle(epi::Vec2 v1, epi::Vec2 v2, epi::Vec2 v3, epi::Vec2 test);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
