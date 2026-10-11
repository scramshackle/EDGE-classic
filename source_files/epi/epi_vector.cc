#include "epi_vector.h"

namespace epi
{

Mat4 MultiplyMatrices(const Mat4 &left, const Mat4 &right)
{
    Mat4 result;
    for (int column = 0; column < 4; column++)
    {
        Vec4 combined              = TransformVector(left, MatrixColumn(right, column));
        result.elements[column][0] = combined.x;
        result.elements[column][1] = combined.y;
        result.elements[column][2] = combined.z;
        result.elements[column][3] = combined.w;
    }
    return result;
}

Mat4 RotationMatrix(float radians, Vec3 axis)
{
    Mat4 result = IdentityMatrix();

    axis = NormalizeVector(axis);

    float sine             = sinf(radians);
    float cosine           = cosf(radians);
    float one_minus_cosine = 1.0f - cosine;

    result.elements[0][0] = (axis.x * axis.x * one_minus_cosine) + cosine;
    result.elements[0][1] = (axis.x * axis.y * one_minus_cosine) + (axis.z * sine);
    result.elements[0][2] = (axis.x * axis.z * one_minus_cosine) - (axis.y * sine);

    result.elements[1][0] = (axis.y * axis.x * one_minus_cosine) - (axis.z * sine);
    result.elements[1][1] = (axis.y * axis.y * one_minus_cosine) + cosine;
    result.elements[1][2] = (axis.y * axis.z * one_minus_cosine) + (axis.x * sine);

    result.elements[2][0] = (axis.z * axis.x * one_minus_cosine) + (axis.y * sine);
    result.elements[2][1] = (axis.z * axis.y * one_minus_cosine) - (axis.x * sine);
    result.elements[2][2] = (axis.z * axis.z * one_minus_cosine) + cosine;

    return result;
}

static Vec3 ColumnXYZ(const Mat4 &matrix, int column)
{
    return Vec3{matrix.elements[column][0], matrix.elements[column][1], matrix.elements[column][2]};
}

static void SetRow(Mat4 &matrix, int row, Vec3 xyz, float w)
{
    matrix.elements[0][row] = xyz.x;
    matrix.elements[1][row] = xyz.y;
    matrix.elements[2][row] = xyz.z;
    matrix.elements[3][row] = w;
}

Mat4 InverseMatrix(const Mat4 &matrix)
{
    Vec3  column_0 = ColumnXYZ(matrix, 0);
    Vec3  column_1 = ColumnXYZ(matrix, 1);
    Vec3  column_2 = ColumnXYZ(matrix, 2);
    Vec3  column_3 = ColumnXYZ(matrix, 3);
    float w_0      = matrix.elements[0][3];
    float w_1      = matrix.elements[1][3];
    float w_2      = matrix.elements[2][3];
    float w_3      = matrix.elements[3][3];

    Vec3 cross_01 = CrossProduct(column_0, column_1);
    Vec3 cross_23 = CrossProduct(column_2, column_3);
    Vec3 b_10     = SubtractVectors(ScaleVector(column_0, w_1), ScaleVector(column_1, w_0));
    Vec3 b_32     = SubtractVectors(ScaleVector(column_2, w_3), ScaleVector(column_3, w_2));

    float inverse_determinant = 1.0f / (DotProduct(cross_01, b_32) + DotProduct(cross_23, b_10));
    cross_01                  = ScaleVector(cross_01, inverse_determinant);
    cross_23                  = ScaleVector(cross_23, inverse_determinant);
    b_10                      = ScaleVector(b_10, inverse_determinant);
    b_32                      = ScaleVector(b_32, inverse_determinant);

    Mat4 result;
    SetRow(result, 0, AddVectors(CrossProduct(column_1, b_32), ScaleVector(cross_23, w_1)),
           -DotProduct(column_1, cross_23));
    SetRow(result, 1, SubtractVectors(CrossProduct(b_32, column_0), ScaleVector(cross_23, w_0)),
           DotProduct(column_0, cross_23));
    SetRow(result, 2, AddVectors(CrossProduct(column_3, b_10), ScaleVector(cross_01, w_3)),
           -DotProduct(column_3, cross_01));
    SetRow(result, 3, SubtractVectors(CrossProduct(b_10, column_2), ScaleVector(cross_01, w_2)),
           DotProduct(column_2, cross_01));
    return result;
}

} // namespace epi
