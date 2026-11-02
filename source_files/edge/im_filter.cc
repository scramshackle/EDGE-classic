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

#include <string.h>

#include <vector>

#include "epi.h"
#include "epi_simd.h"

static constexpr int     kXBRBorder         = 2;
static constexpr int32_t kXBRLumaRed        = 2126;
static constexpr int32_t kXBRLumaGreen      = 7152;
static constexpr int32_t kXBRLumaBlue       = 722;
static constexpr int32_t kXBRLumaUnit       = 10000;
static constexpr int32_t kXBREdgeBias       = 255 * kXBRLumaUnit / 10;

static inline int XBRSourceIndex(int index, int size, bool wrap)
{
    if (wrap)
        return ((index % size) + size) % size;

    if (index < 0)
        return 0;

    if (index >= size)
        return size - 1;

    return index;
}

static inline epi::SimdI32x4 XBRDistance(epi::SimdI32x4 a, epi::SimdI32x4 b)
{
    return epi::AbsI32x4(epi::SubI32x4(a, b));
}

static inline epi::SimdI32x4 XBRCorner(epi::SimdI32x4 e, epi::SimdI32x4 b, epi::SimdI32x4 c, epi::SimdI32x4 d,
                                       epi::SimdI32x4 f, epi::SimdI32x4 g, epi::SimdI32x4 h, epi::SimdI32x4 i,
                                       epi::SimdI32x4 i4, epi::SimdI32x4 i5, epi::SimdI32x4 h5, epi::SimdI32x4 f4,
                                       epi::SimdI32x4 color_e, epi::SimdI32x4 color_f, epi::SimdI32x4 color_h)
{
    epi::SimdI32x4 same_as_neighbor = epi::OrI32x4(epi::CmpEqI32x4(e, f), epi::CmpEqI32x4(e, h));

    epi::SimdI32x4 distance_ef = XBRDistance(e, f);
    epi::SimdI32x4 distance_eh = XBRDistance(e, h);

    epi::SimdI32x4 weight_across =
        epi::AddI32x4(epi::AddI32x4(XBRDistance(e, c), XBRDistance(e, g)),
                      epi::AddI32x4(epi::AddI32x4(XBRDistance(i, h5), XBRDistance(i, f4)),
                                    epi::ShiftLeftI32x4<2>(XBRDistance(h, f))));
    epi::SimdI32x4 weight_along =
        epi::AddI32x4(epi::AddI32x4(XBRDistance(h, d), XBRDistance(h, i5)),
                      epi::AddI32x4(epi::AddI32x4(XBRDistance(f, i4), XBRDistance(f, b)),
                                    epi::ShiftLeftI32x4<2>(XBRDistance(e, i))));

    epi::SimdI32x4 no_edge =
        epi::OrI32x4(same_as_neighbor,
                     epi::CmpGtI32x4(epi::AddI32x4(weight_across, epi::SplatI32x4(kXBREdgeBias)), weight_along));

    epi::SimdI32x4 nearest = epi::SelectI32x4(epi::CmpGtI32x4(distance_ef, distance_eh), color_h, color_f);

    return epi::SelectI32x4(no_edge, color_e, nearest);
}

ImageData *ImageXBR(const ImageData *image, bool wrap)
{
    int width  = image->width_;
    int height = image->height_;
    int depth  = image->depth_;

    int groups        = (width + 3) / 4;
    int padded_width  = groups * 4 + kXBRBorder * 2;
    int padded_height = height + kXBRBorder * 2;

    std::vector<int32_t> keys((size_t)padded_width * padded_height);
    std::vector<int32_t> colors((size_t)padded_width * padded_height);

    std::vector<int32_t> row_keys(width);
    std::vector<int32_t> row_colors(width);

    for (int py = 0; py < padded_height; py++)
    {
        int            sy  = XBRSourceIndex(py - kXBRBorder, height, wrap);
        const uint8_t *src = image->pixels_ + (size_t)sy * width * depth;

        for (int x = 0; x < width; x++, src += depth)
        {
            uint8_t rgba[4] = {src[0], src[1], src[2], (uint8_t)((depth == 4) ? src[3] : 255)};

            int32_t luma = kXBRLumaRed * rgba[0] + kXBRLumaGreen * rgba[1] + kXBRLumaBlue * rgba[2];

            row_keys[x] = luma * rgba[3] / 255 - (255 - rgba[3]) * kXBRLumaUnit;
            memcpy(&row_colors[x], rgba, 4);
        }

        int32_t *key_row   = keys.data() + (size_t)py * padded_width;
        int32_t *color_row = colors.data() + (size_t)py * padded_width;

        for (int px = 0; px < padded_width; px++)
        {
            int sx        = XBRSourceIndex(px - kXBRBorder, width, wrap);
            key_row[px]   = row_keys[sx];
            color_row[px] = row_colors[sx];
        }
    }

    ImageData *result = new ImageData(width * 2, height * 2, 4);

    int32_t tail[16];

    for (int y = 0; y < height; y++)
    {
        const int32_t *k0 = keys.data() + (size_t)(y + kXBRBorder) * padded_width + kXBRBorder;
        const int32_t *km = k0 - padded_width;
        const int32_t *kn = k0 - padded_width * 2;
        const int32_t *kp = k0 + padded_width;
        const int32_t *kq = k0 + padded_width * 2;
        const int32_t *c0 = colors.data() + (size_t)(y + kXBRBorder) * padded_width + kXBRBorder;
        const int32_t *cm = c0 - padded_width;
        const int32_t *cp = c0 + padded_width;

        int32_t *top    = (int32_t *)result->pixels_ + (size_t)(y * 2) * (width * 2);
        int32_t *bottom = top + width * 2;

        for (int x = 0; x < width; x += 4)
        {
            epi::SimdI32x4 A1 = epi::LoadI32x4(kn + x - 1);
            epi::SimdI32x4 B1 = epi::LoadI32x4(kn + x);
            epi::SimdI32x4 C1 = epi::LoadI32x4(kn + x + 1);

            epi::SimdI32x4 A0 = epi::LoadI32x4(km + x - 2);
            epi::SimdI32x4 A  = epi::LoadI32x4(km + x - 1);
            epi::SimdI32x4 B  = epi::LoadI32x4(km + x);
            epi::SimdI32x4 C  = epi::LoadI32x4(km + x + 1);
            epi::SimdI32x4 C4 = epi::LoadI32x4(km + x + 2);

            epi::SimdI32x4 D0 = epi::LoadI32x4(k0 + x - 2);
            epi::SimdI32x4 D  = epi::LoadI32x4(k0 + x - 1);
            epi::SimdI32x4 E  = epi::LoadI32x4(k0 + x);
            epi::SimdI32x4 F  = epi::LoadI32x4(k0 + x + 1);
            epi::SimdI32x4 F4 = epi::LoadI32x4(k0 + x + 2);

            epi::SimdI32x4 G0 = epi::LoadI32x4(kp + x - 2);
            epi::SimdI32x4 G  = epi::LoadI32x4(kp + x - 1);
            epi::SimdI32x4 H  = epi::LoadI32x4(kp + x);
            epi::SimdI32x4 I  = epi::LoadI32x4(kp + x + 1);
            epi::SimdI32x4 I4 = epi::LoadI32x4(kp + x + 2);

            epi::SimdI32x4 G5 = epi::LoadI32x4(kq + x - 1);
            epi::SimdI32x4 H5 = epi::LoadI32x4(kq + x);
            epi::SimdI32x4 I5 = epi::LoadI32x4(kq + x + 1);

            epi::SimdI32x4 color_b = epi::LoadI32x4(cm + x);
            epi::SimdI32x4 color_d = epi::LoadI32x4(c0 + x - 1);
            epi::SimdI32x4 color_e = epi::LoadI32x4(c0 + x);
            epi::SimdI32x4 color_f = epi::LoadI32x4(c0 + x + 1);
            epi::SimdI32x4 color_h = epi::LoadI32x4(cp + x);

            epi::SimdI32x4 bottom_right = XBRCorner(E, B, C, D, F, G, H, I, I4, I5, H5, F4, color_e, color_f, color_h);
            epi::SimdI32x4 top_right    = XBRCorner(E, D, A, H, B, I, F, C, C1, C4, F4, B1, color_e, color_b, color_f);
            epi::SimdI32x4 top_left     = XBRCorner(E, H, G, F, D, C, B, A, A0, A1, B1, D0, color_e, color_d, color_b);
            epi::SimdI32x4 bottom_left  = XBRCorner(E, F, I, B, H, A, D, G, G5, G0, D0, H5, color_e, color_h, color_d);

            epi::SimdI32x4 top_lo    = epi::InterleaveLoI32x4(top_left, top_right);
            epi::SimdI32x4 top_hi    = epi::InterleaveHiI32x4(top_left, top_right);
            epi::SimdI32x4 bottom_lo = epi::InterleaveLoI32x4(bottom_left, bottom_right);
            epi::SimdI32x4 bottom_hi = epi::InterleaveHiI32x4(bottom_left, bottom_right);

            if (x + 4 <= width)
            {
                epi::StoreI32x4(top + x * 2, top_lo);
                epi::StoreI32x4(top + x * 2 + 4, top_hi);
                epi::StoreI32x4(bottom + x * 2, bottom_lo);
                epi::StoreI32x4(bottom + x * 2 + 4, bottom_hi);
            }
            else
            {
                size_t bytes = (size_t)(width - x) * 2 * sizeof(int32_t);

                epi::StoreI32x4(tail, top_lo);
                epi::StoreI32x4(tail + 4, top_hi);
                memcpy(top + x * 2, tail, bytes);

                epi::StoreI32x4(tail + 8, bottom_lo);
                epi::StoreI32x4(tail + 12, bottom_hi);
                memcpy(bottom + x * 2, tail + 8, bytes);
            }
        }
    }

    return result;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
