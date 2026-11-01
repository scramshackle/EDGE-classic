#pragma once

#include <stdint.h>

#include <span>

#include "epi_vector.h"
#include "r_units.h"

enum TranslucentKind
{
    kTranslucentUnit = 0,
    kTranslucentStatic,
    kTranslucentSprite,
    kTranslucentModel
};

void TranslucentBegin(void);

void TranslucentAddPolygon(TranslucentKind kind, int payload, uint64_t group, GLuint shape,
                           std::span<const RendererVertex> vertices);
void TranslucentAddPoints(TranslucentKind kind, int payload, uint64_t group, std::span<const epi::Vec3> points);
void TranslucentAddPlanarBox(TranslucentKind kind, int payload, uint64_t group, const epi::Vec3 &normal,
                             float plane_distance, const float low[3], const float high[3]);

void TranslucentDraw(void);

void EmitTranslucentUnits(std::span<const int> payloads);
void EmitTranslucentStatic(std::span<const int> payloads);
void EmitTranslucentSprites(std::span<const int> payloads);
void EmitTranslucentModels(std::span<const int> payloads);
