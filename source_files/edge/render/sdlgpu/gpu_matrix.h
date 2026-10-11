#pragma once

#include "epi_vector.h"

static inline epi::Mat4 GpuFrustumMatrix(float left, float right, float bottom, float top, float z_near, float z_far)
{
    epi::Mat4 result = {};

    result.elements[0][0] = (2.0f * z_near) / (right - left);
    result.elements[1][1] = (2.0f * z_near) / (top - bottom);

    result.elements[2][0] = (right + left) / (right - left);
    result.elements[2][1] = (top + bottom) / (top - bottom);
    result.elements[2][2] = -z_far / (z_far - z_near);
    result.elements[2][3] = -1.0f;

    result.elements[3][2] = -(z_far * z_near) / (z_far - z_near);

    return result;
}

static inline epi::Mat4 GpuOrthographicMatrix(float left, float right, float bottom, float top, float z_near,
                                              float z_far)
{
    epi::Mat4 result = {};

    result.elements[0][0] = 2.0f / (right - left);
    result.elements[1][1] = 2.0f / (top - bottom);
    result.elements[2][2] = -1.0f / (z_far - z_near);
    result.elements[3][3] = 1.0f;

    result.elements[3][0] = -(right + left) / (right - left);
    result.elements[3][1] = -(top + bottom) / (top - bottom);
    result.elements[3][2] = -z_near / (z_far - z_near);

    return result;
}
