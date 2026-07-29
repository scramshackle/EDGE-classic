#pragma once

#include "HandmadeMath.h"

static inline HMM_Mat4 GpuFrustumMatrix(float left, float right, float bottom, float top, float z_near, float z_far)
{
    HMM_Mat4 result = {};

    result.Elements[0][0] = (2.0f * z_near) / (right - left);
    result.Elements[1][1] = (2.0f * z_near) / (top - bottom);

    result.Elements[2][0] = (right + left) / (right - left);
    result.Elements[2][1] = (top + bottom) / (top - bottom);
    result.Elements[2][2] = -z_far / (z_far - z_near);
    result.Elements[2][3] = -1.0f;

    result.Elements[3][2] = -(z_far * z_near) / (z_far - z_near);

    return result;
}

static inline HMM_Mat4 GpuOrthographicMatrix(float left, float right, float bottom, float top, float z_near,
                                             float z_far)
{
    HMM_Mat4 result = {};

    result.Elements[0][0] = 2.0f / (right - left);
    result.Elements[1][1] = 2.0f / (top - bottom);
    result.Elements[2][2] = -1.0f / (z_far - z_near);
    result.Elements[3][3] = 1.0f;

    result.Elements[3][0] = -(right + left) / (right - left);
    result.Elements[3][1] = -(top + bottom) / (top - bottom);
    result.Elements[3][2] = -z_near / (z_far - z_near);

    return result;
}
