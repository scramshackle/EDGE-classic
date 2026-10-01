//------------------------------------------------------------------------
//  EDGE Image Filtering/Scaling
//------------------------------------------------------------------------
//
//  Copyright (c) 2007-2024 The EDGE Team.
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
//  Blur is based on "C++ implementation of a fast Gaussian blur algorithm by
//    Ivan Kutskir - Integer Version"
//
//  Copyright (C) 2017 Basile Fraboni
//  Copyright (C) 2014 Ivan Kutskir
//  All Rights Reserved
//  You may use, distribute and modify this code under the
//  terms of the MIT license. For further details please refer
//  to : https://mit-license.org/
//
//----------------------------------------------------------------------------
//
//  HQ2x is based heavily on the code (C) 2003 Maxim Stepin, which is
//  under the GNU LGPL (Lesser General Public License).
//
//  For more information, see: http://hiend3d.com/hq2x.html
//
//----------------------------------------------------------------------------

#include "im_filter.h"

#include "HandmadeMath.h"
#include "epi.h"

struct EPXSource
{
    const uint8_t *pixels;
    int            width;
    int            height;
    int            depth;
    bool           wrap;

    inline uint32_t At(int x, int y) const
    {
        if (wrap)
        {
            x = ((x % width) + width) % width;
            y = ((y % height) + height) % height;
        }
        else
        {
            x = HMM_Clamp(0, x, width - 1);
            y = HMM_Clamp(0, y, height - 1);
        }

        const uint8_t *p     = pixels + ((size_t)y * width + x) * depth;
        uint8_t        alpha = (depth == 4) ? p[3] : 255;

        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)alpha << 24);
    }
};

ImageData *ImageEPX(const ImageData *image, bool wrap)
{
    int w = image->width_;
    int h = image->height_;

    ImageData *result = new ImageData(w * 2, h * 2, 4);

    EPXSource source = {image->pixels_, w, h, image->depth_, wrap};

    uint32_t *dest = (uint32_t *)result->pixels_;

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            uint32_t B = source.At(x, y - 1);
            uint32_t D = source.At(x - 1, y);
            uint32_t E = source.At(x, y);
            uint32_t F = source.At(x + 1, y);
            uint32_t H = source.At(x, y + 1);

            uint32_t J = E, K = E, L = E, M = E;

            if (D == B && D != H && D != F)
                J = D;
            if (B == F && B != D && B != H)
                K = B;
            if (H == D && H != F && H != B)
                L = H;
            if (F == H && F != B && F != D)
                M = F;

            uint32_t *out = dest + (size_t)(y * 2) * (w * 2) + x * 2;

            out[0]         = J;
            out[1]         = K;
            out[w * 2]     = L;
            out[w * 2 + 1] = M;
        }
    }

    return result;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
