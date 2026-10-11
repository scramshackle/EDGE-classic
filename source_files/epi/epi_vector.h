#pragma once

#include <math.h>

namespace epi
{

struct Vec2
{
    float x;
    float y;
};

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Vec4
{
    float x;
    float y;
    float z;
    float w;
};

struct Mat4
{
    float elements[4][4];
};

inline Vec2 SubtractVectors(Vec2 left, Vec2 right)
{
    return Vec2{left.x - right.x, left.y - right.y};
}

inline Vec3 SubtractVectors(Vec3 left, Vec3 right)
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

inline Vec3 AddVectors(Vec3 left, Vec3 right)
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

inline Vec2 MultiplyComponents(Vec2 left, Vec2 right)
{
    return Vec2{left.x * right.x, left.y * right.y};
}

inline Vec3 ScaleVector(Vec3 vector, float scale)
{
    return Vec3{vector.x * scale, vector.y * scale, vector.z * scale};
}

inline float DotProduct(Vec2 left, Vec2 right)
{
    return (left.x * right.x) + (left.y * right.y);
}

inline float DotProduct(Vec3 left, Vec3 right)
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

inline float DotProduct(Vec4 left, Vec4 right)
{
    return ((left.x * right.x) + (left.y * right.y)) + ((left.z * right.z) + (left.w * right.w));
}

inline Vec3 CrossProduct(Vec3 left, Vec3 right)
{
    return Vec3{(left.y * right.z) - (left.z * right.y), (left.z * right.x) - (left.x * right.z),
                (left.x * right.y) - (left.y * right.x)};
}

inline float VectorLengthSquared(Vec3 value)
{
    return DotProduct(value, value);
}

inline float VectorLength(Vec2 value)
{
    return sqrtf(DotProduct(value, value));
}

inline float VectorLength(Vec3 value)
{
    return sqrtf(DotProduct(value, value));
}

inline Vec2 NormalizeVector(Vec2 value)
{
    float inverse_length = 1.0f / sqrtf(DotProduct(value, value));
    return Vec2{value.x * inverse_length, value.y * inverse_length};
}

inline Vec3 NormalizeVector(Vec3 value)
{
    return ScaleVector(value, 1.0f / sqrtf(DotProduct(value, value)));
}

inline Vec2 RotateVector(Vec2 value, float radians)
{
    float sine   = sinf(radians);
    float cosine = cosf(radians);
    return Vec2{value.x * cosine - value.y * sine, value.x * sine + value.y * cosine};
}

inline float VectorComponent(Vec3 value, int axis)
{
    return (axis == 0) ? value.x : ((axis == 1) ? value.y : value.z);
}

inline Vec4 MatrixColumn(const Mat4 &matrix, int column)
{
    return Vec4{matrix.elements[column][0], matrix.elements[column][1], matrix.elements[column][2],
                matrix.elements[column][3]};
}

inline Mat4 IdentityMatrix()
{
    Mat4 result           = {};
    result.elements[0][0] = 1.0f;
    result.elements[1][1] = 1.0f;
    result.elements[2][2] = 1.0f;
    result.elements[3][3] = 1.0f;
    return result;
}

inline Mat4 TranslationMatrix(Vec3 translation)
{
    Mat4 result           = IdentityMatrix();
    result.elements[3][0] = translation.x;
    result.elements[3][1] = translation.y;
    result.elements[3][2] = translation.z;
    return result;
}

inline Mat4 ScaleMatrix(Vec3 scale)
{
    Mat4 result           = IdentityMatrix();
    result.elements[0][0] = scale.x;
    result.elements[1][1] = scale.y;
    result.elements[2][2] = scale.z;
    return result;
}

inline Vec4 TransformVector(const Mat4 &matrix, Vec4 vector)
{
    Vec4 result;
    result.x = vector.x * matrix.elements[0][0];
    result.y = vector.x * matrix.elements[0][1];
    result.z = vector.x * matrix.elements[0][2];
    result.w = vector.x * matrix.elements[0][3];

    result.x += vector.y * matrix.elements[1][0];
    result.y += vector.y * matrix.elements[1][1];
    result.z += vector.y * matrix.elements[1][2];
    result.w += vector.y * matrix.elements[1][3];

    result.x += vector.z * matrix.elements[2][0];
    result.y += vector.z * matrix.elements[2][1];
    result.z += vector.z * matrix.elements[2][2];
    result.w += vector.z * matrix.elements[2][3];

    result.x += vector.w * matrix.elements[3][0];
    result.y += vector.w * matrix.elements[3][1];
    result.z += vector.w * matrix.elements[3][2];
    result.w += vector.w * matrix.elements[3][3];
    return result;
}

Mat4 MultiplyMatrices(const Mat4 &left, const Mat4 &right);
Mat4 RotationMatrix(float radians, Vec3 axis);
Mat4 InverseMatrix(const Mat4 &matrix);

} // namespace epi
