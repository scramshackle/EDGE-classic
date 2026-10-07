#pragma once

#include "i_defs_gl.h"

class Image;

struct AtlasRegion
{
    GLuint texture;
    float  rectangle[4];
};

bool AtlasSpriteRegion(int sprite, const Image *image, AtlasRegion *region);

bool AtlasImageRegion(const Image *image, AtlasRegion *region);

void AtlasPrecacheSprite(int sprite);

void AtlasClear(void);
