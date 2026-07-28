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

#include "HandmadeMath.h"
#include "epi_endian.h"

// RGBA 8:8:8:8
typedef uint32_t RGBAColor;

constexpr int kRGBARedShift   = (kByteOrder == kLittleEndian) ? 0 : 24;
constexpr int kRGBAGreenShift = (kByteOrder == kLittleEndian) ? 8 : 16;
constexpr int kRGBABlueShift  = (kByteOrder == kLittleEndian) ? 16 : 8;
constexpr int kRGBAAlphaShift = (kByteOrder == kLittleEndian) ? 24 : 0;

constexpr RGBAColor MakeRGBAConstant(uint32_t rgba)
{
    return (RGBAColor)((((rgba >> 24) & 0xFF) << kRGBARedShift) | (((rgba >> 16) & 0xFF) << kRGBAGreenShift) |
                       (((rgba >> 8) & 0xFF) << kRGBABlueShift) | ((rgba & 0xFF) << kRGBAAlphaShift));
}

constexpr RGBAColor kRGBANoValue = MakeRGBAConstant(0x01FEFEFF); /* EDGE special value, bright cyan */

// X11 Color Presets
// Todo: Perhaps automatically gen DDFCOLM entries for these before loading
// other colormaps to ensure they are "always available" for modders? - Dasho
constexpr RGBAColor kRGBAAliceBlue         = MakeRGBAConstant(0xF0F8FFFF);
constexpr RGBAColor kRGBAAntiqueWhite      = MakeRGBAConstant(0xFAEBD7FF);
constexpr RGBAColor kRGBAAqua              = MakeRGBAConstant(0x00FFFFFF);
constexpr RGBAColor kRGBAAquamarine        = MakeRGBAConstant(0x7FFFD4FF);
constexpr RGBAColor kRGBAAzure             = MakeRGBAConstant(0xF0FFFFFF);
constexpr RGBAColor kRGBABeige             = MakeRGBAConstant(0xF5F5DCFF);
constexpr RGBAColor kRGBABisque            = MakeRGBAConstant(0xFFE4C4FF);
constexpr RGBAColor kRGBABlack             = MakeRGBAConstant(0x000000FF);
constexpr RGBAColor kRGBABlanchedAlmond    = MakeRGBAConstant(0xFFEBCDFF);
constexpr RGBAColor kRGBABlue              = MakeRGBAConstant(0x0000FFFF);
constexpr RGBAColor kRGBABlueViolet        = MakeRGBAConstant(0x8A2BE2FF);
constexpr RGBAColor kRGBABrown             = MakeRGBAConstant(0xA52A2AFF);
constexpr RGBAColor kRGBABurlywood         = MakeRGBAConstant(0xDEB887FF);
constexpr RGBAColor kRGBACadetBlue         = MakeRGBAConstant(0x5F9EA0FF);
constexpr RGBAColor kRGBAChartreuse        = MakeRGBAConstant(0x7FFF00FF);
constexpr RGBAColor kRGBAChocolate         = MakeRGBAConstant(0xD2691EFF);
constexpr RGBAColor kRGBACoral             = MakeRGBAConstant(0xFF7F50FF);
constexpr RGBAColor kRGBACornflowerBlue    = MakeRGBAConstant(0x6495EDFF);
constexpr RGBAColor kRGBACornsilk          = MakeRGBAConstant(0xFFF8DCFF);
constexpr RGBAColor kRGBACrimson           = MakeRGBAConstant(0xDC143CFF);
constexpr RGBAColor kRGBACyan              = MakeRGBAConstant(0x00FFFFFF);
constexpr RGBAColor kRGBADarkBlue          = MakeRGBAConstant(0x00008BFF);
constexpr RGBAColor kRGBADarkCyan          = MakeRGBAConstant(0x008B8BFF);
constexpr RGBAColor kRGBADarkGoldenrod     = MakeRGBAConstant(0xB8860BFF);
constexpr RGBAColor kRGBADarkGray          = MakeRGBAConstant(0xA9A9A9FF);
constexpr RGBAColor kRGBADarkGreen         = MakeRGBAConstant(0x006400FF);
constexpr RGBAColor kRGBADarkKhaki         = MakeRGBAConstant(0xBDB76BFF);
constexpr RGBAColor kRGBADarkMagenta       = MakeRGBAConstant(0x8B008BFF);
constexpr RGBAColor kRGBADarkOliveGreen    = MakeRGBAConstant(0x556B2FFF);
constexpr RGBAColor kRGBADarkOrange        = MakeRGBAConstant(0xFF8C00FF);
constexpr RGBAColor kRGBADarkOrchid        = MakeRGBAConstant(0x9932CCFF);
constexpr RGBAColor kRGBADarkRed           = MakeRGBAConstant(0x8B0000FF);
constexpr RGBAColor kRGBADarkSlamon        = MakeRGBAConstant(0xE9967AFF);
constexpr RGBAColor kRGBADarkSeaGreen      = MakeRGBAConstant(0x8FBC8FFF);
constexpr RGBAColor kRGBADarkSlateBlue     = MakeRGBAConstant(0x483D8BFF);
constexpr RGBAColor kRGBADarkSlateGray     = MakeRGBAConstant(0x2F4F4FFF);
constexpr RGBAColor kRGBADarkTurquoise     = MakeRGBAConstant(0x00CED1FF);
constexpr RGBAColor kRGBADarkViolet        = MakeRGBAConstant(0x9400D3FF);
constexpr RGBAColor kRGBADeepPink          = MakeRGBAConstant(0xFF1493FF);
constexpr RGBAColor kRGBADeepSkyBlue       = MakeRGBAConstant(0x00BFFFFF);
constexpr RGBAColor kRGBADimGray           = MakeRGBAConstant(0x696969FF);
constexpr RGBAColor kRGBADodgerBlue        = MakeRGBAConstant(0x1E90FFFF);
constexpr RGBAColor kRGBAFireBrick         = MakeRGBAConstant(0xB22222FF);
constexpr RGBAColor kRGBAFloralWhite       = MakeRGBAConstant(0xFFFAF0FF);
constexpr RGBAColor kRGBAForestGreen       = MakeRGBAConstant(0x228B22FF);
constexpr RGBAColor kRGBAFuchsia           = MakeRGBAConstant(0xFF00FFFF);
constexpr RGBAColor kRGBAGainsboro         = MakeRGBAConstant(0xDCDCDCFF);
constexpr RGBAColor kRGBAGhostWhite        = MakeRGBAConstant(0xF8F8FFFF);
constexpr RGBAColor kRGBAGold              = MakeRGBAConstant(0xFFD700FF);
constexpr RGBAColor kRGBAGoldenrod         = MakeRGBAConstant(0xDAA520FF);
constexpr RGBAColor kRGBAGray              = MakeRGBAConstant(0xBEBEBEFF);
constexpr RGBAColor kRGBAWebGray           = MakeRGBAConstant(0x808080FF);
constexpr RGBAColor kRGBAGreen             = MakeRGBAConstant(0x00FF00FF);
constexpr RGBAColor kRGBAWebGreen          = MakeRGBAConstant(0x008000FF);
constexpr RGBAColor kRGBAGreenYellow       = MakeRGBAConstant(0xADFF2FFF);
constexpr RGBAColor kRGBAHoneydew          = MakeRGBAConstant(0xF0FFF0FF);
constexpr RGBAColor kRGBAHotPink           = MakeRGBAConstant(0xFF69B4FF);
constexpr RGBAColor kRGBAIndianRed         = MakeRGBAConstant(0xCD5C5CFF);
constexpr RGBAColor kRGBAIndigo            = MakeRGBAConstant(0x4B0082FF);
constexpr RGBAColor kRGBAIvory             = MakeRGBAConstant(0xFFFFF0FF);
constexpr RGBAColor kRGBAKhaki             = MakeRGBAConstant(0xF0E68CFF);
constexpr RGBAColor kRGBALavender          = MakeRGBAConstant(0xE6E6FAFF);
constexpr RGBAColor kRGBALavenderBlush     = MakeRGBAConstant(0xFFF0F5FF);
constexpr RGBAColor kRGBALawnGreen         = MakeRGBAConstant(0x7CFC00FF);
constexpr RGBAColor kRGBALemonChiffon      = MakeRGBAConstant(0xFFFACDFF);
constexpr RGBAColor kRGBALightBlue         = MakeRGBAConstant(0xADD8E6FF);
constexpr RGBAColor kRGBALightCoral        = MakeRGBAConstant(0xF08080FF);
constexpr RGBAColor kRGBALightCyan         = MakeRGBAConstant(0xE0FFFFFF);
constexpr RGBAColor kRGBALightGoldenrod    = MakeRGBAConstant(0xFAFAD2FF);
constexpr RGBAColor kRGBALightGray         = MakeRGBAConstant(0xD3D3D3FF);
constexpr RGBAColor kRGBALightGreen        = MakeRGBAConstant(0x90EE90FF);
constexpr RGBAColor kRGBALightPink         = MakeRGBAConstant(0xFFB6C1FF);
constexpr RGBAColor kRGBALightSalmon       = MakeRGBAConstant(0xFFA07AFF);
constexpr RGBAColor kRGBALightSeaGreen     = MakeRGBAConstant(0x20B2AAFF);
constexpr RGBAColor kRGBALightSkyBlue      = MakeRGBAConstant(0x87CEFAFF);
constexpr RGBAColor kRGBALightSlateGray    = MakeRGBAConstant(0x778899FF);
constexpr RGBAColor kRGBALightSteelBlue    = MakeRGBAConstant(0xB0C4DEFF);
constexpr RGBAColor kRGBALightYellow       = MakeRGBAConstant(0xFFFFE0FF);
constexpr RGBAColor kRGBALime              = MakeRGBAConstant(0x00FF00FF);
constexpr RGBAColor kRGBALimeGreen         = MakeRGBAConstant(0x32CD32FF);
constexpr RGBAColor kRGBALinen             = MakeRGBAConstant(0xFAF0E6FF);
constexpr RGBAColor kRGBAMagenta           = MakeRGBAConstant(0xFF00FFFF);
constexpr RGBAColor kRGBAMaroon            = MakeRGBAConstant(0xB03060FF);
constexpr RGBAColor kRGBAWebMaroon         = MakeRGBAConstant(0x800000FF);
constexpr RGBAColor kRGBAMediumAquamarine  = MakeRGBAConstant(0x66CDAAFF);
constexpr RGBAColor kRGBAMediumBlue        = MakeRGBAConstant(0x0000CDFF);
constexpr RGBAColor kRGBAMediumOrchid      = MakeRGBAConstant(0xBA55D3FF);
constexpr RGBAColor kRGBAMediumPurple      = MakeRGBAConstant(0x9370DBFF);
constexpr RGBAColor kRGBAMediumSeaGreen    = MakeRGBAConstant(0x3CB371FF);
constexpr RGBAColor kRGBAMediumSlateBlue   = MakeRGBAConstant(0x7B68EEFF);
constexpr RGBAColor kRGBAMediumSpringGreen = MakeRGBAConstant(0x00FA9AFF);
constexpr RGBAColor kRGBAMediumTurquoise   = MakeRGBAConstant(0x48D1CCFF);
constexpr RGBAColor kRGBAMediumVioletRed   = MakeRGBAConstant(0xC71585FF);
constexpr RGBAColor kRGBAMidnightBlue      = MakeRGBAConstant(0x191970FF);
constexpr RGBAColor kRGBAMintCream         = MakeRGBAConstant(0xF5FFFAFF);
constexpr RGBAColor kRGBAMistyRose         = MakeRGBAConstant(0xFFE4E1FF);
constexpr RGBAColor kRGBAMoccasin          = MakeRGBAConstant(0xFFE4B5FF);
constexpr RGBAColor kRGBANavajoWhite       = MakeRGBAConstant(0xFFDEADFF);
constexpr RGBAColor kRGBANavyBlue          = MakeRGBAConstant(0x000080FF);
constexpr RGBAColor kRGBAOldLace           = MakeRGBAConstant(0xFDF5E6FF);
constexpr RGBAColor kRGBAOlive             = MakeRGBAConstant(0x808000FF);
constexpr RGBAColor kRGBAOliveDrab         = MakeRGBAConstant(0x6B8E23FF);
constexpr RGBAColor kRGBAOrange            = MakeRGBAConstant(0xFFA500FF);
constexpr RGBAColor kRGBAOrangeRed         = MakeRGBAConstant(0xFF4500FF);
constexpr RGBAColor kRGBAOrchid            = MakeRGBAConstant(0xDA70D6FF);
constexpr RGBAColor kRGBAPaleGoldenrod     = MakeRGBAConstant(0xEEE8AAFF);
constexpr RGBAColor kRGBAPaleGreen         = MakeRGBAConstant(0x98FB98FF);
constexpr RGBAColor kRGBAPaleTurquoise     = MakeRGBAConstant(0xAFEEEEFF);
constexpr RGBAColor kRGBAPaleVioletRed     = MakeRGBAConstant(0xDB7093FF);
constexpr RGBAColor kRGBAPapayaWhip        = MakeRGBAConstant(0xFFEFD5FF);
constexpr RGBAColor kRGBAPeachPuff         = MakeRGBAConstant(0xFFDAB9FF);
constexpr RGBAColor kRGBAPeru              = MakeRGBAConstant(0xCD853FFF);
constexpr RGBAColor kRGBAPink              = MakeRGBAConstant(0xFFC0CBFF);
constexpr RGBAColor kRGBAPlum              = MakeRGBAConstant(0xDDA0DDFF);
constexpr RGBAColor kRGBAPowderBlue        = MakeRGBAConstant(0xB0E0E6FF);
constexpr RGBAColor kRGBAPurple            = MakeRGBAConstant(0xA020F0FF);
constexpr RGBAColor kRGBAWebPurple         = MakeRGBAConstant(0x800080FF);
constexpr RGBAColor kRGBARebeccaPurple     = MakeRGBAConstant(0x663399FF);
constexpr RGBAColor kRGBARed               = MakeRGBAConstant(0xFF0000FF);
constexpr RGBAColor kRGBARosyBrown         = MakeRGBAConstant(0xBC8F8FFF);
constexpr RGBAColor kRGBARoyalBlue         = MakeRGBAConstant(0x4169E1FF);
constexpr RGBAColor kRGBASaddleBrown       = MakeRGBAConstant(0x8B4513FF);
constexpr RGBAColor kRGBASalmon            = MakeRGBAConstant(0xFA8072FF);
constexpr RGBAColor kRGBASandyBrown        = MakeRGBAConstant(0xF4A460FF);
constexpr RGBAColor kRGBASeaGreen          = MakeRGBAConstant(0x2E8B57FF);
constexpr RGBAColor kRGBASeaShell          = MakeRGBAConstant(0xFFF5EEFF);
constexpr RGBAColor kRGBASienna            = MakeRGBAConstant(0xA0522DFF);
constexpr RGBAColor kRGBASilver            = MakeRGBAConstant(0xC0C0C0FF);
constexpr RGBAColor kRGBASkyBlue           = MakeRGBAConstant(0x87CEEBFF);
constexpr RGBAColor kRGBASlateBlue         = MakeRGBAConstant(0x6A5ACDFF);
constexpr RGBAColor kRGBASlateGray         = MakeRGBAConstant(0x708090FF);
constexpr RGBAColor kRGBASnow              = MakeRGBAConstant(0xFFFAFAFF);
constexpr RGBAColor kRGBASpringGreen       = MakeRGBAConstant(0x00FF7FFF);
constexpr RGBAColor kRGBASteelBlue         = MakeRGBAConstant(0x4682B4FF);
constexpr RGBAColor kRGBATan               = MakeRGBAConstant(0xD2B48CFF);
constexpr RGBAColor kRGBATeal              = MakeRGBAConstant(0x008080FF);
constexpr RGBAColor kRGBAThistle           = MakeRGBAConstant(0xD8BFD8FF);
constexpr RGBAColor kRGBATomato            = MakeRGBAConstant(0xFF6347FF);
constexpr RGBAColor kRGBATransparent       = MakeRGBAConstant(0x00000000);
constexpr RGBAColor kRGBATurquoise         = MakeRGBAConstant(0x40E0D0FF);
constexpr RGBAColor kRGBAViolet            = MakeRGBAConstant(0xEE82EEFF);
constexpr RGBAColor kRGBAWheat             = MakeRGBAConstant(0xF5DEB3FF);
constexpr RGBAColor kRGBAWhite             = MakeRGBAConstant(0xFFFFFFFF);
constexpr RGBAColor kRGBAWhiteSmoke        = MakeRGBAConstant(0xF5F5F5FF);
constexpr RGBAColor kRGBAYellow            = MakeRGBAConstant(0xFFFF00FF);
constexpr RGBAColor kRGBAYellowGreen       = MakeRGBAConstant(0x9ACD32FF);

namespace epi
{

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
    uint32_t nr = HMM_Clamp(0, r, 255);
    uint32_t ng = HMM_Clamp(0, g, 255);
    uint32_t nb = HMM_Clamp(0, b, 255);
    uint32_t na = HMM_Clamp(0, a, 255);

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
