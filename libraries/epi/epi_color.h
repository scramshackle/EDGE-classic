//------------------------------------------------------------------------
//  EPI Colour types (RGBA and HSV)
//----------------------------------------------------------------------------
//
//  Copyright (c) 2004-2024 The EDGE Team.
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

#include <bit>

#include "epi_math.h"

namespace epi
{

// RGBA 8:8:8:8
typedef uint32_t RGBAColor;

constexpr int kRGBARedShift   = (std::endian::native == std::endian::little) ? 0 : 24;
constexpr int kRGBAGreenShift = (std::endian::native == std::endian::little) ? 8 : 16;
constexpr int kRGBABlueShift  = (std::endian::native == std::endian::little) ? 16 : 8;
constexpr int kRGBAAlphaShift = (std::endian::native == std::endian::little) ? 24 : 0;

consteval RGBAColor MakeRGBAConstant(uint32_t rgba)
{
    return (RGBAColor)((((rgba >> 24) & 0xFF) << kRGBARedShift) | (((rgba >> 16) & 0xFF) << kRGBAGreenShift) |
                       (((rgba >> 8) & 0xFF) << kRGBABlueShift) | ((rgba & 0xFF) << kRGBAAlphaShift));
}

inline RGBAColor MakeRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return (RGBAColor)(((uint32_t)r << kRGBARedShift) | ((uint32_t)g << kRGBAGreenShift) |
                       ((uint32_t)b << kRGBABlueShift) | ((uint32_t)a << kRGBAAlphaShift));
}

inline RGBAColor MakeRGBAFloat(float r, float g, float b, float a = 1.0f)
{
    return (RGBAColor)(((uint32_t)(r * 255.0f) << kRGBARedShift) | ((uint32_t)(g * 255.0f) << kRGBAGreenShift) |
                       ((uint32_t)(b * 255.0f) << kRGBABlueShift) | ((uint32_t)(a * 255.0f) << kRGBAAlphaShift));
}

inline RGBAColor MakeRGBAClamped(int r, int g, int b, int a = 255)
{
    uint32_t nr = epi::Clamp(r, 0, 255);
    uint32_t ng = epi::Clamp(g, 0, 255);
    uint32_t nb = epi::Clamp(b, 0, 255);
    uint32_t na = epi::Clamp(a, 0, 255);

    return (RGBAColor)((nr << kRGBARedShift) | (ng << kRGBAGreenShift) | (nb << kRGBABlueShift) |
                       (na << kRGBAAlphaShift));
}

inline uint8_t GetRGBARed(RGBAColor rgba)
{
    return (uint8_t)(rgba >> kRGBARedShift);
}

inline uint8_t GetRGBAGreen(RGBAColor rgba)
{
    return (uint8_t)(rgba >> kRGBAGreenShift);
}

inline uint8_t GetRGBABlue(RGBAColor rgba)
{
    return (uint8_t)(rgba >> kRGBABlueShift);
}

inline uint8_t GetRGBAAlpha(RGBAColor rgba)
{
    return (uint8_t)(rgba >> kRGBAAlphaShift);
}

inline void SetRGBAAlpha(RGBAColor &rgba, uint8_t alpha)
{
    rgba &= ~((uint32_t)0xFF << kRGBAAlphaShift);
    rgba |= (uint32_t)alpha << kRGBAAlphaShift;
}

inline void SetRGBAAlpha(RGBAColor &rgba, float alpha)
{
    uint32_t ualpha = (uint32_t)(alpha * 255.0f);
    rgba &= ~((uint32_t)0xFF << kRGBAAlphaShift);
    rgba |= ualpha << kRGBAAlphaShift;
}

inline RGBAColor MixRGBA(const RGBAColor &mix1, const RGBAColor &mix2, int qty = 128)
{
    int nr = int(GetRGBARed(mix1)) * (255 - qty) + int(GetRGBARed(mix2)) * qty;
    int ng = int(GetRGBAGreen(mix1)) * (255 - qty) + int(GetRGBAGreen(mix2)) * qty;
    int nb = int(GetRGBABlue(mix1)) * (255 - qty) + int(GetRGBABlue(mix2)) * qty;
    int na = int(GetRGBAAlpha(mix1)) * (255 - qty) + int(GetRGBAAlpha(mix2)) * qty;

    return MakeRGBA(uint8_t(nr / 255), uint8_t(ng / 255), uint8_t(nb / 255), uint8_t(na / 255));
}

class HSVColor
{
  public:
    // sealed, value semantics.
    //
    // h is hue (angle from 0 to 359: 0 = RED, 120 = GREEN, 240 = BLUE).
    // s is saturation (0 to 255: 0 = White, 255 = Pure color).
    // v is value (0 to 255: 0 = Darkest, 255 = Brightest).

    short   h_;
    uint8_t s_, v_;

    HSVColor(const RGBAColor &col); // conversion from RGBA

    RGBAColor ToRGBA() const;       // conversion to RGBA

    inline HSVColor &Rotate(int delta)
    {
        int bam = int(h_ + delta) * 372827;

        h_ = short((bam & 0x7FFFFFF) / 372827);

        return *this;
    } // usable range: -1800 to +1800

    inline HSVColor &SetSaturation(int sat)
    {
        s_ = sat;

        return *this;
    }

    inline HSVColor &SetValue(int val)
    {
        v_ = val;

        return *this;
    }
};

} // namespace epi

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
