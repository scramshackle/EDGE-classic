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

static constexpr int      kHQ2xBorder          = 1;
static constexpr int      kHQ2xGroup           = 16;
static constexpr uint8_t  kHQ2xLumaThreshold   = 48;
static constexpr uint8_t  kHQ2xChromaThreshold = 7;
static constexpr uint64_t kHQ2xChannelMask     = 0x00FF00FF00FF00FFull;

struct HQ2xRule
{
    uint8_t neighbor[3];
    uint8_t weight[3];
};

struct HQ2xPatternRule
{
    uint8_t rule[2];
    uint8_t condition;
};

static constexpr int kHQ2xRuleCount = 41;

static constexpr HQ2xRule kHQ2xRules[kHQ2xRuleCount] = {
    {{0, 1, 4}, {4, 4, 8}},  {{0, 3, 4}, {4, 4, 8}},  {{0, 4, 4}, {4, 12, 0}}, {{1, 3, 4}, {1, 1, 14}},
    {{1, 4, 5}, {1, 14, 1}}, {{1, 3, 4}, {2, 2, 12}}, {{1, 3, 4}, {2, 4, 10}}, {{1, 4, 5}, {2, 10, 4}},
    {{1, 4, 5}, {2, 12, 2}}, {{1, 2, 4}, {4, 4, 8}},  {{1, 3, 4}, {4, 2, 10}}, {{1, 3, 4}, {4, 4, 8}},
    {{1, 4, 5}, {4, 8, 4}},  {{1, 4, 5}, {4, 10, 2}}, {{1, 4, 4}, {4, 12, 0}}, {{1, 3, 4}, {6, 6, 4}},
    {{1, 4, 5}, {6, 4, 6}},  {{2, 4, 5}, {4, 8, 4}},  {{2, 4, 4}, {4, 12, 0}}, {{3, 4, 7}, {1, 14, 1}},
    {{3, 4, 7}, {2, 10, 4}}, {{3, 4, 7}, {2, 12, 2}}, {{3, 4, 6}, {4, 8, 4}},  {{3, 4, 7}, {4, 8, 4}},
    {{3, 4, 7}, {4, 10, 2}}, {{3, 4, 4}, {4, 12, 0}}, {{3, 4, 7}, {6, 4, 6}},  {{4, 5, 7}, {4, 6, 6}},
    {{4, 5, 7}, {8, 4, 4}},  {{4, 5, 8}, {8, 4, 4}},  {{4, 6, 7}, {8, 4, 4}},  {{4, 7, 8}, {8, 4, 4}},
    {{4, 5, 7}, {10, 2, 4}}, {{4, 5, 7}, {10, 4, 2}}, {{4, 5, 7}, {12, 2, 2}}, {{4, 5, 4}, {12, 4, 0}},
    {{4, 6, 4}, {12, 4, 0}}, {{4, 7, 4}, {12, 4, 0}}, {{4, 8, 4}, {12, 4, 0}}, {{4, 5, 7}, {14, 1, 1}},
    {{4, 4, 4}, {16, 0, 0}},
};

static constexpr HQ2xPatternRule kHQ2xPatterns[256][4] = {
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 2}, 0}, {{17, 17}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{15, 2}, 0}, {{13, 35}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{15, 40}, 0}, {{13, 35}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{1, 1}, 0}, {{12, 18}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{10, 25}, 1}, {{16, 18}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{10, 25}, 1}, {{16, 40}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{11, 40}, 0}, {{12, 40}, 1}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{11, 40}, 0}, {{18, 18}, 0}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{2, 2}, 0}, {{12, 40}, 1}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{11, 40}, 0}, {{12, 40}, 1}, {{30, 30}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{15, 2}, 0}, {{17, 17}, 0}, {{24, 37}, 0}, {{28, 28}, 0}},
    {{{15, 40}, 0}, {{17, 17}, 0}, {{24, 37}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{5, 2}, 0}, {{35, 35}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{3, 40}, 0}, {{35, 35}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{1, 1}, 0}, {{12, 18}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{10, 25}, 1}, {{16, 18}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{10, 25}, 1}, {{16, 40}, 1}, {{23, 23}, 0}, {{31, 31}, 0}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{11, 40}, 0}, {{8, 18}, 1}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{2, 2}, 0}, {{12, 40}, 1}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{3, 40}, 0}, {{12, 40}, 1}, {{37, 37}, 0}, {{31, 31}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{22, 22}, 0}, {{29, 29}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{23, 36}, 2}, {{29, 29}, 0}},
    {{{6, 14}, 2}, {{12, 12}, 0}, {{26, 36}, 2}, {{29, 29}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{36, 36}, 0}, {{29, 29}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{23, 36}, 2}, {{29, 29}, 0}},
    {{{6, 14}, 2}, {{12, 12}, 0}, {{26, 36}, 2}, {{29, 29}, 0}},
    {{{5, 2}, 0}, {{35, 35}, 0}, {{21, 36}, 2}, {{29, 29}, 0}},
    {{{11, 40}, 0}, {{35, 35}, 0}, {{21, 36}, 2}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{22, 22}, 0}, {{28, 38}, 3}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{22, 22}, 0}, {{28, 38}, 3}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{25, 25}, 0}, {{8, 18}, 1}, {{22, 22}, 0}, {{34, 38}, 3}},
    {{{11, 11}, 0}, {{7, 14}, 3}, {{22, 22}, 0}, {{27, 38}, 3}},
    {{{11, 11}, 0}, {{7, 14}, 3}, {{22, 22}, 0}, {{27, 38}, 3}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{22, 22}, 0}, {{38, 38}, 0}},
    {{{25, 25}, 0}, {{12, 40}, 1}, {{22, 22}, 0}, {{34, 38}, 3}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{23, 40}, 2}, {{28, 40}, 3}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{11, 40}, 0}, {{8, 18}, 1}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{5, 2}, 0}, {{12, 40}, 1}, {{21, 36}, 2}, {{34, 38}, 3}},
    {{{11, 40}, 0}, {{12, 40}, 1}, {{36, 36}, 0}, {{38, 38}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{25, 25}, 0}, {{29, 29}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{6, 14}, 2}, {{12, 12}, 0}, {{26, 40}, 2}, {{29, 29}, 0}},
    {{{2, 2}, 0}, {{17, 17}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{6, 14}, 2}, {{12, 12}, 0}, {{26, 40}, 2}, {{29, 29}, 0}},
    {{{2, 2}, 0}, {{35, 35}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{3, 40}, 0}, {{35, 35}, 0}, {{23, 40}, 2}, {{29, 29}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{20, 25}, 3}, {{27, 38}, 3}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{20, 25}, 3}, {{27, 38}, 3}},
    {{{1, 1}, 0}, {{8, 18}, 1}, {{25, 25}, 0}, {{34, 38}, 3}},
    {{{25, 25}, 0}, {{8, 18}, 1}, {{25, 25}, 0}, {{34, 38}, 3}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{25, 25}, 0}, {{34, 38}, 3}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{25, 25}, 0}, {{34, 38}, 3}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{25, 25}, 0}, {{38, 38}, 0}},
    {{{10, 25}, 1}, {{16, 40}, 1}, {{25, 25}, 0}, {{38, 38}, 0}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{23, 40}, 2}, {{38, 38}, 0}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{23, 40}, 2}, {{34, 38}, 3}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{23, 40}, 2}, {{34, 38}, 3}},
    {{{11, 40}, 0}, {{18, 18}, 0}, {{23, 40}, 2}, {{38, 38}, 0}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{23, 40}, 2}, {{38, 38}, 0}},
    {{{6, 14}, 2}, {{14, 14}, 0}, {{26, 40}, 2}, {{38, 38}, 0}},
    {{{2, 2}, 0}, {{12, 40}, 1}, {{23, 40}, 2}, {{38, 38}, 0}},
    {{{3, 40}, 0}, {{12, 40}, 1}, {{23, 40}, 2}, {{38, 38}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 2}, 0}, {{17, 17}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{15, 2}, 0}, {{13, 35}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{15, 40}, 0}, {{13, 35}, 0}, {{30, 30}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{1, 1}, 0}, {{16, 18}, 1}, {{23, 23}, 0}, {{33, 37}, 1}},
    {{{25, 25}, 0}, {{8, 18}, 1}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{1, 1}, 0}, {{16, 40}, 1}, {{23, 23}, 0}, {{33, 37}, 1}},
    {{{25, 25}, 0}, {{4, 40}, 1}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{11, 40}, 0}, {{18, 18}, 0}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{5, 2}, 0}, {{12, 40}, 1}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{11, 40}, 0}, {{4, 40}, 1}, {{30, 30}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{23, 23}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{15, 2}, 0}, {{17, 17}, 0}, {{24, 37}, 0}, {{28, 28}, 0}},
    {{{15, 40}, 0}, {{17, 17}, 0}, {{24, 37}, 0}, {{28, 28}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{5, 2}, 0}, {{35, 35}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{3, 40}, 0}, {{35, 35}, 0}, {{37, 37}, 0}, {{28, 28}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{1, 1}, 0}, {{16, 18}, 1}, {{23, 23}, 0}, {{33, 37}, 1}},
    {{{25, 25}, 0}, {{8, 18}, 1}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{1, 1}, 0}, {{16, 40}, 1}, {{23, 23}, 0}, {{33, 37}, 1}},
    {{{25, 25}, 0}, {{4, 40}, 1}, {{23, 23}, 0}, {{37, 37}, 0}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{15, 40}, 0}, {{18, 18}, 0}, {{24, 37}, 0}, {{37, 37}, 0}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{2, 2}, 0}, {{16, 40}, 1}, {{37, 37}, 0}, {{33, 37}, 1}},
    {{{3, 40}, 0}, {{4, 40}, 1}, {{37, 37}, 0}, {{37, 37}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{22, 22}, 0}, {{35, 35}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{26, 36}, 2}, {{32, 35}, 2}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{21, 36}, 2}, {{35, 35}, 0}},
    {{{5, 2}, 0}, {{17, 17}, 0}, {{21, 36}, 2}, {{35, 35}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{36, 36}, 0}, {{35, 35}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{26, 36}, 2}, {{32, 35}, 2}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{21, 36}, 2}, {{35, 35}, 0}},
    {{{5, 2}, 0}, {{35, 35}, 0}, {{21, 36}, 2}, {{35, 35}, 0}},
    {{{15, 40}, 0}, {{13, 35}, 0}, {{36, 36}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{1, 1}, 0}, {{18, 18}, 0}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{25, 25}, 0}, {{18, 18}, 0}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{11, 11}, 0}, {{7, 14}, 3}, {{22, 22}, 0}, {{27, 40}, 3}},
    {{{11, 11}, 0}, {{7, 14}, 3}, {{22, 22}, 0}, {{27, 40}, 3}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{25, 25}, 0}, {{4, 40}, 1}, {{22, 22}, 0}, {{28, 40}, 3}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{36, 36}, 0}, {{28, 40}, 3}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{36, 36}, 0}, {{28, 40}, 3}},
    {{{5, 2}, 0}, {{8, 18}, 1}, {{21, 36}, 2}, {{28, 40}, 3}},
    {{{11, 40}, 0}, {{18, 18}, 0}, {{36, 36}, 0}, {{28, 40}, 3}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{21, 36}, 2}, {{28, 40}, 3}},
    {{{14, 14}, 0}, {{7, 14}, 3}, {{36, 36}, 0}, {{27, 40}, 3}},
    {{{2, 2}, 0}, {{12, 40}, 1}, {{36, 36}, 0}, {{28, 40}, 3}},
    {{{11, 40}, 0}, {{4, 40}, 1}, {{36, 36}, 0}, {{28, 40}, 3}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{1, 1}, 0}, {{17, 17}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{25, 25}, 0}, {{17, 17}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{12, 12}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{1, 1}, 0}, {{35, 35}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{25, 25}, 0}, {{35, 35}, 0}, {{25, 25}, 0}, {{35, 35}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{26, 40}, 2}, {{32, 35}, 2}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{19, 40}, 2}, {{35, 35}, 0}},
    {{{5, 2}, 0}, {{17, 17}, 0}, {{23, 40}, 2}, {{35, 35}, 0}},
    {{{11, 40}, 0}, {{17, 17}, 0}, {{19, 40}, 2}, {{35, 35}, 0}},
    {{{0, 0}, 0}, {{12, 12}, 0}, {{26, 40}, 2}, {{32, 35}, 2}},
    {{{14, 14}, 0}, {{12, 12}, 0}, {{19, 40}, 2}, {{35, 35}, 0}},
    {{{2, 2}, 0}, {{35, 35}, 0}, {{26, 40}, 2}, {{32, 35}, 2}},
    {{{3, 40}, 0}, {{35, 35}, 0}, {{19, 40}, 2}, {{35, 35}, 0}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{20, 25}, 3}, {{27, 40}, 3}},
    {{{11, 11}, 0}, {{9, 9}, 0}, {{20, 25}, 3}, {{27, 40}, 3}},
    {{{1, 1}, 0}, {{8, 18}, 1}, {{25, 25}, 0}, {{28, 40}, 3}},
    {{{25, 25}, 0}, {{18, 18}, 0}, {{20, 25}, 3}, {{27, 40}, 3}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{25, 25}, 0}, {{39, 40}, 3}},
    {{{11, 11}, 0}, {{14, 14}, 0}, {{25, 25}, 0}, {{39, 40}, 3}},
    {{{1, 1}, 0}, {{12, 40}, 1}, {{25, 25}, 0}, {{39, 40}, 3}},
    {{{25, 25}, 0}, {{4, 40}, 1}, {{25, 25}, 0}, {{39, 40}, 3}},
    {{{0, 0}, 0}, {{9, 9}, 0}, {{23, 40}, 2}, {{28, 40}, 3}},
    {{{14, 14}, 0}, {{9, 9}, 0}, {{19, 40}, 2}, {{28, 40}, 3}},
    {{{2, 2}, 0}, {{18, 18}, 0}, {{23, 40}, 2}, {{28, 40}, 3}},
    {{{11, 40}, 0}, {{18, 18}, 0}, {{19, 40}, 2}, {{28, 40}, 3}},
    {{{0, 0}, 0}, {{14, 14}, 0}, {{23, 40}, 2}, {{39, 40}, 3}},
    {{{14, 14}, 0}, {{14, 14}, 0}, {{19, 40}, 2}, {{39, 40}, 3}},
    {{{2, 2}, 0}, {{12, 40}, 1}, {{23, 40}, 2}, {{39, 40}, 3}},
    {{{3, 40}, 0}, {{4, 40}, 1}, {{19, 40}, 2}, {{39, 40}, 3}},
};

struct HQ2xPlanes
{
    int                   stride;
    std::vector<uint8_t>  luma;
    std::vector<uint8_t>  blue_difference;
    std::vector<uint8_t>  red_difference;
    std::vector<uint8_t>  alpha;
    std::vector<uint64_t> colors;
};

static inline int HQ2xSourceIndex(int index, int size, bool wrap)
{
    if (wrap)
        return ((index % size) + size) % size;

    if (index < 0)
        return 0;

    if (index >= size)
        return size - 1;

    return index;
}

static inline epi::SimdU8x16 HQ2xAbsoluteDifference(const std::vector<uint8_t> &plane, size_t a, size_t b)
{
    epi::SimdU8x16 first  = epi::LoadU8x16(plane.data() + a);
    epi::SimdU8x16 second = epi::LoadU8x16(plane.data() + b);

    return epi::OrU8x16(epi::SubSaturateU8x16(first, second), epi::SubSaturateU8x16(second, first));
}

static inline epi::SimdU8x16 HQ2xFlag(const HQ2xPlanes &planes, size_t a, size_t b, uint8_t bit)
{
    epi::SimdU8x16 luma =
        epi::SubSaturateU8x16(HQ2xAbsoluteDifference(planes.luma, a, b), epi::SplatU8x16(kHQ2xLumaThreshold));
    epi::SimdU8x16 blue_difference = epi::SubSaturateU8x16(HQ2xAbsoluteDifference(planes.blue_difference, a, b),
                                                           epi::SplatU8x16(kHQ2xChromaThreshold));
    epi::SimdU8x16 red_difference  = epi::SubSaturateU8x16(HQ2xAbsoluteDifference(planes.red_difference, a, b),
                                                           epi::SplatU8x16(kHQ2xChromaThreshold));
    epi::SimdU8x16 alpha           = HQ2xAbsoluteDifference(planes.alpha, a, b);

    epi::SimdU8x16 exceeded = epi::OrU8x16(epi::OrU8x16(luma, blue_difference), epi::OrU8x16(red_difference, alpha));
    epi::SimdU8x16 same     = epi::CmpEqU8x16(exceeded, epi::SplatU8x16(0));

    return epi::AndNotU8x16(epi::SplatU8x16(bit), same);
}

static inline uint64_t HQ2xSpread(uint32_t color)
{
    uint64_t spread = color;
    spread          = (spread | (spread << 16)) & 0x0000FFFF0000FFFFull;
    return (spread | (spread << 8)) & kHQ2xChannelMask;
}

static inline int HQ2xAbsolute(int value)
{
    return value < 0 ? -value : value;
}

static inline uint8_t HQ2xDifferentScalar(const HQ2xPlanes &planes, size_t a, size_t b)
{
    int luma            = HQ2xAbsolute(planes.luma[a] - planes.luma[b]);
    int blue_difference = HQ2xAbsolute(planes.blue_difference[a] - planes.blue_difference[b]);
    int red_difference  = HQ2xAbsolute(planes.red_difference[a] - planes.red_difference[b]);

    return (uint8_t)((luma > kHQ2xLumaThreshold) | (blue_difference > kHQ2xChromaThreshold) |
                     (red_difference > kHQ2xChromaThreshold) | (planes.alpha[a] != planes.alpha[b]));
}

static inline void HQ2xBuildEdges(const HQ2xPlanes &planes, int width, int height, std::vector<uint8_t> &edges)
{
    edges.assign(planes.luma.size(), 0);

    for (int py = 0; py <= height; py++)
    {
        for (int px = 0; px <= width; px++)
        {
            size_t index = (size_t)py * planes.stride + px;
            size_t right = index + 1;
            size_t below = index + planes.stride;

            edges[index] =
                (uint8_t)(HQ2xDifferentScalar(planes, index, right) | (HQ2xDifferentScalar(planes, index, below) << 1) |
                          (HQ2xDifferentScalar(planes, index, below + 1) << 2) |
                          (HQ2xDifferentScalar(planes, right, below) << 3));
        }
    }
}

static inline void HQ2xScalarKey(const std::vector<uint8_t> &edges, size_t center, size_t stride, uint8_t *pattern,
                                 uint8_t *condition)
{
    uint8_t upper_left = edges[center - stride - 1];
    uint8_t upper      = edges[center - stride];
    uint8_t left       = edges[center - 1];
    uint8_t here       = edges[center];

    *pattern   = (uint8_t)(((upper_left >> 2) & 1) | (upper & 2) | ((upper >> 1) & 4) | ((left << 3) & 8) |
                         ((here << 4) & 0x10) | ((left << 2) & 0x20) | ((here << 5) & 0x40) | ((here << 5) & 0x80));
    *condition = (uint8_t)(((upper_left >> 3) & 1) | ((upper >> 1) & 2) | (left & 4) | (here & 8));
}

struct HQ2xRuleLookup
{
    uint8_t rule[4096][4];
};

static constexpr HQ2xRuleLookup HQ2xBuildRuleLookup()
{
    HQ2xRuleLookup lookup = {};

    for (int key = 0; key < 4096; key++)
    {
        int pattern = key & 0xFF;
        int flags   = key >> 8;

        for (int subpixel = 0; subpixel < 4; subpixel++)
        {
            const HQ2xPatternRule &entry = kHQ2xPatterns[pattern][subpixel];
            lookup.rule[key][subpixel]   = entry.rule[(flags >> entry.condition) & 1];
        }
    }

    return lookup;
}

static constexpr HQ2xRuleLookup kHQ2xRuleLookup = HQ2xBuildRuleLookup();

struct HQ2xRuleTaps
{
    ptrdiff_t offset[3];
    uint64_t  weight[3];
};

static inline uint32_t HQ2xBlend(const uint64_t *center, const HQ2xRuleTaps &taps)
{
    uint64_t sum = taps.weight[0] * center[taps.offset[0]] + taps.weight[1] * center[taps.offset[1]] +
                   taps.weight[2] * center[taps.offset[2]];

    sum = (sum >> 4) & kHQ2xChannelMask;
    sum = (sum | (sum >> 8)) & 0x0000FFFF0000FFFFull;
    return (uint32_t)(sum | (sum >> 16));
}

ImageData *ImageHQ2x(const ImageData *image, bool wrap)
{
    int width  = image->width_;
    int height = image->height_;
    int depth  = image->depth_;

    bool opaque = true;

    if (depth == 4)
    {
        for (int index = 0; index < width * height && opaque; index++)
            opaque = image->pixels_[index * 4 + 3] == 255;
    }

    HQ2xPlanes planes;
    planes.stride = ((width + kHQ2xGroup - 1) & ~(kHQ2xGroup - 1)) + kHQ2xBorder * 2;

    size_t count = (size_t)planes.stride * (height + kHQ2xBorder * 2);

    planes.luma.assign(count, 0);
    planes.blue_difference.assign(count, 0);
    planes.red_difference.assign(count, 0);
    planes.alpha.assign(count, 0);
    planes.colors.assign(count, 0);

    std::vector<uint8_t>  row_luma(width);
    std::vector<uint8_t>  row_blue_difference(width);
    std::vector<uint8_t>  row_red_difference(width);
    std::vector<uint8_t>  row_alpha(width);
    std::vector<uint64_t> row_colors(width);

    for (int py = 0; py < height + kHQ2xBorder * 2; py++)
    {
        int            sy  = HQ2xSourceIndex(py - kHQ2xBorder, height, wrap);
        const uint8_t *src = image->pixels_ + (size_t)sy * width * depth;

        for (int x = 0; x < width; x++, src += depth)
        {
            int32_t red   = src[0];
            int32_t green = src[1];
            int32_t blue  = src[2];
            int32_t alpha = (depth == 4) ? src[3] : 255;

            if (alpha == 0)
                red = green = blue = 0;
            else if (alpha != 255)
            {
                red   = (red * alpha + 127) / 255;
                green = (green * alpha + 127) / 255;
                blue  = (blue * alpha + 127) / 255;
            }

            row_luma[x]            = (uint8_t)((red * 77 + green * 150 + blue * 29) >> 8);
            row_blue_difference[x] = (uint8_t)(128 + ((-red * 38 - green * 74 + blue * 111) >> 9));
            row_red_difference[x]  = (uint8_t)(128 + ((red * 157 - green * 132 - blue * 26) >> 9));
            row_alpha[x]           = (uint8_t)alpha;
            row_colors[x] = HQ2xSpread(epi::MakeRGBA((uint8_t)red, (uint8_t)green, (uint8_t)blue, (uint8_t)alpha));
        }

        size_t row = (size_t)py * planes.stride + kHQ2xBorder;

        memcpy(planes.luma.data() + row, row_luma.data(), width);
        memcpy(planes.blue_difference.data() + row, row_blue_difference.data(), width);
        memcpy(planes.red_difference.data() + row, row_red_difference.data(), width);
        memcpy(planes.alpha.data() + row, row_alpha.data(), width);
        memcpy(planes.colors.data() + row, row_colors.data(), width * sizeof(uint64_t));

        for (int border = 0; border < kHQ2xBorder; border++)
        {
            int left_column  = -1 - border;
            int right_column = width + border;

            int left_source  = HQ2xSourceIndex(left_column, width, wrap);
            int right_source = HQ2xSourceIndex(right_column, width, wrap);

            planes.luma[row + left_column]             = row_luma[left_source];
            planes.blue_difference[row + left_column]  = row_blue_difference[left_source];
            planes.red_difference[row + left_column]   = row_red_difference[left_source];
            planes.alpha[row + left_column]            = row_alpha[left_source];
            planes.colors[row + left_column]           = row_colors[left_source];
            planes.luma[row + right_column]            = row_luma[right_source];
            planes.blue_difference[row + right_column] = row_blue_difference[right_source];
            planes.red_difference[row + right_column]  = row_red_difference[right_source];
            planes.alpha[row + right_column]           = row_alpha[right_source];
            planes.colors[row + right_column]          = row_colors[right_source];
        }
    }

    ImageData *result = new ImageData(width * 2, height * 2, 4);

    uint8_t patterns[kHQ2xGroup];
    uint8_t conditions[kHQ2xGroup];

    ptrdiff_t up   = -(ptrdiff_t)planes.stride;
    ptrdiff_t down = planes.stride;

    const ptrdiff_t offsets[9] = {up - 1, up, up + 1, -1, 0, 1, down - 1, down, down + 1};

    HQ2xRuleTaps rule_taps[kHQ2xRuleCount];

    for (int rule = 0; rule < kHQ2xRuleCount; rule++)
    {
        for (int tap = 0; tap < 3; tap++)
        {
            rule_taps[rule].offset[tap] = offsets[kHQ2xRules[rule].neighbor[tap]];
            rule_taps[rule].weight[tap] = kHQ2xRules[rule].weight[tap];
        }
    }

    std::vector<uint8_t> edges;

    if (!epi::kSimdAvailable)
        HQ2xBuildEdges(planes, width, height, edges);

    for (int y = 0; y < height; y++)
    {
        size_t row = (size_t)(y + kHQ2xBorder) * planes.stride + kHQ2xBorder;

        uint32_t *top    = (uint32_t *)result->pixels_ + (size_t)(y * 2) * (width * 2);
        uint32_t *bottom = top + width * 2;

        for (int x = 0; x < width; x += kHQ2xGroup)
        {
            size_t center = row + x;

            int lanes = (width - x < kHQ2xGroup) ? width - x : kHQ2xGroup;

            if (epi::kSimdAvailable)
            {
                epi::SimdU8x16 pattern = HQ2xFlag(planes, center, center + up - 1, 0x01);
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + up, 0x02));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + up + 1, 0x04));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center - 1, 0x08));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + 1, 0x10));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + down - 1, 0x20));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + down, 0x40));
                pattern                = epi::OrU8x16(pattern, HQ2xFlag(planes, center, center + down + 1, 0x80));

                epi::SimdU8x16 condition = HQ2xFlag(planes, center - 1, center + up, 0x01);
                condition                = epi::OrU8x16(condition, HQ2xFlag(planes, center + up, center + 1, 0x02));
                condition                = epi::OrU8x16(condition, HQ2xFlag(planes, center + down, center - 1, 0x04));
                condition                = epi::OrU8x16(condition, HQ2xFlag(planes, center + 1, center + down, 0x08));

                epi::StoreU8x16(patterns, pattern);
                epi::StoreU8x16(conditions, condition);
            }
            else
            {
                for (int lane = 0; lane < lanes; lane++)
                    HQ2xScalarKey(edges, center + lane, planes.stride, &patterns[lane], &conditions[lane]);
            }

            for (int lane = 0; lane < lanes; lane++)
            {
                const uint64_t *colors = planes.colors.data() + center + lane;
                const uint8_t  *rules  = kHQ2xRuleLookup.rule[patterns[lane] | (conditions[lane] << 8)];

                uint32_t *out_top    = top + (x + lane) * 2;
                uint32_t *out_bottom = bottom + (x + lane) * 2;

                out_top[0]    = HQ2xBlend(colors, rule_taps[rules[0]]);
                out_top[1]    = HQ2xBlend(colors, rule_taps[rules[1]]);
                out_bottom[0] = HQ2xBlend(colors, rule_taps[rules[2]]);
                out_bottom[1] = HQ2xBlend(colors, rule_taps[rules[3]]);
            }
        }
    }

    if (!opaque)
    {
        uint8_t *pixel = result->pixels_;

        for (int index = 0; index < width * height * 4; index++, pixel += 4)
        {
            int alpha = pixel[3];

            if (alpha == 0 || alpha == 255)
                continue;

            for (int channel = 0; channel < 3; channel++)
            {
                int value      = (pixel[channel] * 255 + alpha / 2) / alpha;
                pixel[channel] = (uint8_t)(value > 255 ? 255 : value);
            }
        }
    }

    return result;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
