//----------------------------------------------------------------------------
//  EDGE OpenGL Rendering (Things)
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

#include "e_player.h"
#include "r_defs.h"
#include "r_gldefs.h"

extern std::vector<DrawThing *> draw_thing_list;

void BSPWalkThing(MapObject *mo);
void EnumerateViewThings(void);
void ThingRenderTic(MapObject *mo);
void ThingRenderSpawned(MapObject *mo);
void ThingRenderRemoved(MapObject *mo);
void ThingRenderDeleted(MapObject *mo);
void ResidentThingsReset(void);
void ResidentThingsInvalidate(void);

void RenderThings(std::vector<DrawThing *> &things, std::vector<DrawThing *> &transparent_things);
void RenderTransparentThings(const std::vector<DrawThing *> &transparent_things, bool models);

void RenderWeaponSprites(Player *p);
void RenderWeaponModel(Player *p);
void RenderCrosshair(Player *p);
