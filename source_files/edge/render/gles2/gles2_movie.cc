//----------------------------------------------------------------------------
//  EDGE Movie Rendering (GLES2)
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

#include "epi.h"
#include "gles2_immediate.h"
#include "gles2_program.h"
#include "i_defs_gl.h"
#include "r_movie.h"
#include "r_state.h"
#include "r_units.h"

static GLuint          movie_planes[3] = {0, 0, 0};
static MoviePlaneSizes movie_sizes     = {0, 0, 0, 0};

static void CreatePlaneTexture(GLuint *texture, int width, int height)
{
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
}

void MovieSetupPlanes(const MoviePlaneSizes &sizes)
{
    MovieReleasePlanes();

    movie_sizes = sizes;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    CreatePlaneTexture(&movie_planes[0], sizes.luma_width, sizes.luma_height);
    CreatePlaneTexture(&movie_planes[1], sizes.chroma_width, sizes.chroma_height);
    CreatePlaneTexture(&movie_planes[2], sizes.chroma_width, sizes.chroma_height);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void MovieUploadPlanes(const uint8_t *luma, const uint8_t *chroma_blue, const uint8_t *chroma_red)
{
    if (!movie_planes[0])
        return;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, movie_planes[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, movie_sizes.luma_width, movie_sizes.luma_height, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, luma);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, movie_planes[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, movie_sizes.chroma_width, movie_sizes.chroma_height, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, chroma_blue);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, movie_planes[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, movie_sizes.chroma_width, movie_sizes.chroma_height, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, chroma_red);

    glActiveTexture(GL_TEXTURE0);
}

void MovieDrawFrame(float x1, float y1, float x2, float y2, float luma_scale_x, float luma_scale_y,
                    float chroma_scale_x, float chroma_scale_y)
{
    if (!movie_planes[0])
        return;

    RendererVertex quad[4];

    EPI_CLEAR_MEMORY(quad, RendererVertex, 4);

    quad[0].position               = {{x1, y1, 0.0f}};
    quad[0].texture_coordinates[0] = {{0.0f, 0.0f}};
    quad[1].position               = {{x2, y1, 0.0f}};
    quad[1].texture_coordinates[0] = {{1.0f, 0.0f}};
    quad[2].position               = {{x2, y2, 0.0f}};
    quad[2].texture_coordinates[0] = {{1.0f, 1.0f}};
    quad[3].position               = {{x1, y2, 0.0f}};
    quad[3].texture_coordinates[0] = {{0.0f, 1.0f}};

    render_state->Disable(GL_BLEND);
    render_state->Disable(GL_DEPTH_TEST);
    render_state->Disable(GL_ALPHA_TEST);

    Gles2ApplyRenderState();

    gles2_movie_program.Use();
    gles2_movie_program.SetMatrices(gles2_immediate.ModelViewMatrix(), gles2_immediate.ProjectionMatrix());
    gles2_movie_program.SetPlaneScales(luma_scale_x, luma_scale_y, chroma_scale_x, chroma_scale_y);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, movie_planes[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, movie_planes[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, movie_planes[2]);

    gles2_immediate.DrawMovieQuad(quad);

    glActiveTexture(GL_TEXTURE0);

    gles2_program.Use();
}

void MovieReleasePlanes(void)
{
    for (int i = 0; i < 3; i++)
    {
        if (movie_planes[i])
        {
            glDeleteTextures(1, &movie_planes[i]);
            movie_planes[i] = 0;
        }
    }

    movie_sizes = {0, 0, 0, 0};
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
