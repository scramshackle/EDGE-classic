//----------------------------------------------------------------------------
//  EDGE Movie Rendering
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

#pragma once

#include <stdint.h>

struct MoviePlaneSizes
{
    int luma_width;
    int luma_height;
    int chroma_width;
    int chroma_height;
};

void MovieSetupPlanes(const MoviePlaneSizes &sizes);

void MovieUploadPlanes(const uint8_t *luma, const uint8_t *chroma_blue, const uint8_t *chroma_red);

void MovieDrawFrame(float x1, float y1, float x2, float y2, float luma_scale_x, float luma_scale_y,
                    float chroma_scale_x, float chroma_scale_y);

void MovieReleasePlanes(void);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
