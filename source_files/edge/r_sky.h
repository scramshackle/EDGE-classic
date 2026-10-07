//----------------------------------------------------------------------------
//  EDGE Sky Handling Code
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

#include "HandmadeMath.h"
#include "im_data.h"
#include "r_image.h"

extern const Image *sky_image;
extern MapSurface  *sky_ref;

// true when a custom sky box is present
extern bool custom_skybox;

extern bool need_to_draw_sky;

enum SkyboxFace
{
    kSkyboxNorth = 0,
    kSkyboxEast,
    kSkyboxSouth,
    kSkyboxWest,
    kSkyboxTop,
    kSkyboxBottom
};

void ComputeSkyHeights(void);

void BeginSky(void);
struct DrawMirror;

void FinishSky(bool use_depth_mask);

void FinishSkyForMirror(const DrawMirror *mir);

constexpr int kSkyWallPartEntry = 3;

void RenderSkyPlane(Sector *sector, float h, Sector *sky_owner, int face, DrawMirror *mir);
void RenderSkyWall(LineSide *line_side, float h1, float h2, Sector *sky_owner, int part, DrawMirror *mir);

void UpdateSkyboxTextures(void);

void SetupSkyMatrices(void);
void RendererRevertSkyMatrices(void);
void GetSkyInverseMatrices(HMM_Mat4 &inverse_projection, HMM_Mat4 &inverse_view);

GLuint CreateSkyCubemap(ImageData *faces[6], int face_size);
void   DeleteSkyCubemap(GLuint cubemap);

void ShutdownSky(void);

void SkyResidentInvalidateSector(Sector *sec);

uint32_t SkyResidentGeneration(void);

bool SkyResidentEnabled(void);

void SkyNoteResidentVisible(void);

bool SkyWallBakeable(const LineSide *line_side, const Sector *sky_owner);

void SkyEntryNoteSectorChanged(int index);

void SkyResidentOrganize(void);

bool SkyEntryClipNeeded(const Sector *entered, const Sector *from);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
