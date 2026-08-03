//------------------------------------------------------------------------
//
//  AJ-BSP  Copyright (C) 2000-2023  Andrew Apted, et al
//          Copyright (C) 1994-1998  Colin Reed
//          Copyright (C) 1997-1998  Lee Killough
//
//  Originally based on the program 'BSP', version 2.3.
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
//------------------------------------------------------------------------

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace ajbsp
{

struct InputVertex
{
    float x;
    float y;
};

struct InputLinedef
{
    int32_t vertex_1;
    int32_t vertex_2;
    int32_t right_side;
    int32_t left_side;
    int32_t tag;
};

enum NodeCacheResult
{
    kNodeCacheOK = 0,
    kNodeCacheLevelMissing,
    kNodeCacheCorrupt
};

struct InputLevel
{
    std::vector<InputVertex>  vertexes;
    std::vector<int32_t>      sidedef_sectors;
    std::vector<InputLinedef> linedefs;
    int32_t                   sector_count;
};

void BuildNodes(const InputLevel &input);

void BeginNodeCache(void);

bool AddNodeCacheLevel(const char *name, uint32_t geometry_crc);

bool WriteNodeCache(const std::string &path);

void ClearNodeCache(void);

NodeCacheResult LoadNodeCacheLevel(const std::string &path, const char *name, uint32_t geometry_crc);

bool IsNodeCacheCurrent(const std::string &path);

} // namespace ajbsp

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
