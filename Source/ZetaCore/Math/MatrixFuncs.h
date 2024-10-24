#pragma once

#include "Matrix.h"
#include "VectorFuncs.h"

//--------------------------------------------------------------------------------------
// Matrix Functions
//--------------------------------------------------------------------------------------

namespace ZetaRay::Math
{
    ZetaInline v_float4x4 zero()
    {
        const __m256 vZero = _mm256_setzero_ps();

        return v_float4x4(_mm_setzero_ps(),
            _mm_setzero_ps(),
            _mm_setzero_ps(),
            _mm_setzero_ps());
    }

    ZetaInline v_float4x4 identity()
    {
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set1_ps(1.0f);

        return v_float4x4(_mm_insert_ps(vZero, vOne, 0x0e),
            _mm_insert_ps(vZero, vOne, 0x1d),
            _mm_insert_ps(vZero, vOne, 0x2b),
            _mm_insert_ps(vZero, vOne, 0x37));
    }

    ZetaInline v_float4x4 __vectorcall add(const v_float4x4 M1, const v_float4x4& M2)
    {
        return v_float4x4(_mm_add_ps(M1.vRow[0], M2.vRow[0]),
            _mm_add_ps(M1.vRow[1], M2.vRow[1]),
            _mm_add_ps(M1.vRow[2], M2.vRow[2]),
            _mm_add_ps(M1.vRow[3], M2.vRow[3]));
    }

    ZetaInline v_float4x4 __vectorcall sub(const v_float4x4 M1, const v_float4x4& M2)
    {
        return v_float4x4(_mm_sub_ps(M1.vRow[0], M2.vRow[0]),
            _mm_sub_ps(M1.vRow[1], M2.vRow[1]),
            _mm_sub_ps(M1.vRow[2], M2.vRow[2]),
            _mm_sub_ps(M1.vRow[3], M2.vRow[3]));
    }

    ZetaInline v_float4x4 __vectorcall transpose(const v_float4x4 M)
    {
        //
        //        0  1  2  3
        // M =    4  5  6  7
        //      8  9  10 11
        //        12 13 14 15
        //
        __m128 vTemp0 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(0, 1, 0, 1));    // 0  1  4  5
        __m128 vTemp1 = _mm_shuffle_ps(M.vRow[2], M.vRow[3], V_SHUFFLE_XYZW(0, 1, 0, 1));    // 8  9  12 13
        __m128 vTemp2 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], V_SHUFFLE_XYZW(2, 3, 2, 3));    // 2  3  6  7
        __m128 vTemp3 = _mm_shuffle_ps(M.vRow[2], M.vRow[3], V_SHUFFLE_XYZW(2, 3, 2, 3));    // 10 11 14 15

        return v_float4x4(_mm_shuffle_ps(vTemp0, vTemp1, 0x88),
            _mm_shuffle_ps(vTemp0, vTemp1, 0xdd),
            _mm_shuffle_ps(vTemp2, vTemp3, 0x88),
            _mm_shuffle_ps(vTemp2, vTemp3, 0xdd));
    }

    // Transposes the 3x3 submatrix in vM, sets the last element of each row to M[2][3] and
    // the last row itself to (0, 0, 0, 1).
    ZetaInline v_float4x4 __vectorcall transpose3x3(const v_float4x4 vM)
    {
        //      0  1  2              0  3  6
        // M =  3  4  5     -->  M = 1  4  7
        //      6  7  8              2  5  8
        v_float4x4 ret;
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vZero = _mm_setzero_ps();

        // 0  1  3  4
        const __m128 vTemp0 = _mm_shuffle_ps(vM.vRow[0], vM.vRow[1], V_SHUFFLE_XYZW(0, 1, 0, 1));
        // 2  _  5  _
        const __m128 vTemp1 = _mm_shuffle_ps(vM.vRow[0], vM.vRow[1], V_SHUFFLE_XYZW(2, 0, 2, 0));

        ret.vRow[0] = _mm_shuffle_ps(vTemp0, vM.vRow[2], V_SHUFFLE_XYZW(0, 2, 0, 3));
        ret.vRow[1] = _mm_shuffle_ps(vTemp0, vM.vRow[2], V_SHUFFLE_XYZW(1, 3, 1, 3));
        ret.vRow[2] = _mm_shuffle_ps(vTemp1, vM.vRow[2], V_SHUFFLE_XYZW(0, 2, 2, 3));
        ret.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

        return ret;
    }

    // TODO: Order of arguments may imply computation of M * v, whereas it's v * M.
    ZetaInline __m128 __vectorcall mul(const v_float4x4 M, const __m128& v)
    {
        // (v.x, v.x, v.x, v.x)
        const __m128 vX = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(0, 0, 0, 0));
        __m128 result = _mm_mul_ps(vX, M.vRow[0]);

        // (v.y, v.y, v.y, v.y)
        const __m128 vY = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(1, 1, 1, 1));
        result = _mm_fmadd_ps(vY, M.vRow[1], result);

        // (v.z, v.z, v.z, v.z)
        const __m128 vZ = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(2, 2, 2, 2));
        result = _mm_fmadd_ps(vZ, M.vRow[2], result);

        // (v.w, v.w, v.w, v.w)
        const __m128 vW = _mm_shuffle_ps(v, v, V_SHUFFLE_XYZW(3, 3, 3, 3));
        result = _mm_fmadd_ps(vW, M.vRow[3], result);

        return result;
    }

    ZetaInline v_float4x4 __vectorcall mul(const v_float4x4 M1, const v_float4x4& M2)
    {
        v_float4x4 M3;

        // Ref: https://github.com/microsoft/DirectXMath/blob/main/Inc/DirectXMathMatrix.inl
        __m256 t0 = _mm256_castps128_ps256(M1.vRow[0]);
        t0 = _mm256_insertf128_ps(t0, M1.vRow[1], 1);
        __m256 t1 = _mm256_castps128_ps256(M1.vRow[2]);
        t1 = _mm256_insertf128_ps(t1, M1.vRow[3], 1);

        __m256 u0 = _mm256_castps128_ps256(M2.vRow[0]);
        u0 = _mm256_insertf128_ps(u0, M2.vRow[1], 1);
        __m256 u1 = _mm256_castps128_ps256(M2.vRow[2]);
        u1 = _mm256_insertf128_ps(u1, M2.vRow[3], 1);

        __m256 a0 = _mm256_shuffle_ps(t0, t0, _MM_SHUFFLE(0, 0, 0, 0));
        __m256 a1 = _mm256_shuffle_ps(t1, t1, _MM_SHUFFLE(0, 0, 0, 0));
        __m256 b0 = _mm256_permute2f128_ps(u0, u0, 0x00);
        __m256 c0 = _mm256_mul_ps(a0, b0);
        __m256 c1 = _mm256_mul_ps(a1, b0);

        a0 = _mm256_shuffle_ps(t0, t0, _MM_SHUFFLE(1, 1, 1, 1));
        a1 = _mm256_shuffle_ps(t1, t1, _MM_SHUFFLE(1, 1, 1, 1));
        b0 = _mm256_permute2f128_ps(u0, u0, 0x11);
        __m256 c2 = _mm256_fmadd_ps(a0, b0, c0);
        __m256 c3 = _mm256_fmadd_ps(a1, b0, c1);

        a0 = _mm256_shuffle_ps(t0, t0, _MM_SHUFFLE(2, 2, 2, 2));
        a1 = _mm256_shuffle_ps(t1, t1, _MM_SHUFFLE(2, 2, 2, 2));
        __m256 b1 = _mm256_permute2f128_ps(u1, u1, 0x00);
        __m256 c4 = _mm256_mul_ps(a0, b1);
        __m256 c5 = _mm256_mul_ps(a1, b1);

        a0 = _mm256_shuffle_ps(t0, t0, _MM_SHUFFLE(3, 3, 3, 3));
        a1 = _mm256_shuffle_ps(t1, t1, _MM_SHUFFLE(3, 3, 3, 3));
        b1 = _mm256_permute2f128_ps(u1, u1, 0x11);
        __m256 c6 = _mm256_fmadd_ps(a0, b1, c4);
        __m256 c7 = _mm256_fmadd_ps(a1, b1, c5);

        t0 = _mm256_add_ps(c2, c6);
        t1 = _mm256_add_ps(c3, c7);

        M3.vRow[0] = _mm256_castps256_ps128(t0);
        M3.vRow[1] = _mm256_extractf128_ps(t0, 1);
        M3.vRow[2] = _mm256_castps256_ps128(t1);
        M3.vRow[3] = _mm256_extractf128_ps(t1, 1);

        return M3;
    }

    ZetaInline __m128 __vectorcall det3x3(const v_float4x4 M)
    {
        // Given M = [a b c], scalar triple product a.(b x c) gives the determinant
        const __m128 vRow1xRow2 = cross(M.vRow[1], M.vRow[2]);
        const __m128 vRow0 = _mm_blend_ps(M.vRow[0], _mm_setzero_ps(), V_BLEND_XYZW(0, 0, 0, 1));
        const __m128 det = _mm_dp_ps(M.vRow[0], vRow1xRow2, 0xff);

        return det;
    }

    // TODO this can be done more efficiently
    // Given a transformation matrix of the following form, returns its inverse,
    // M = S * R * T,
    // where S is a scaling, R is a rotation and T is a translation transformation
    ZetaInline v_float4x4 __vectorcall inverseSRT(const v_float4x4 M)
    {
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vZero = _mm_setzero_ps();

        //      0  1  2              0  4  8
        // M =  4  5  6     -->  M = 1  5  9
        //      8  9  10             2  6  10
        const __m128 vTemp0 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], 
            V_SHUFFLE_XYZW(0, 1, 0, 1));    // 0  1  4  5
        const __m128 vTemp1 = _mm_shuffle_ps(M.vRow[0], M.vRow[1], 
            V_SHUFFLE_XYZW(2, 0, 2, 0));    // 2  _  6  _

        // for 3x3 matrix M = [u, v, w] where u,v,w are columns vectors, M^(-1) is given by
        // M = [a b c]^T
        // where 
        //        a = (v * w) / u.(v * w)
        //        b = (w * u) / u.(v * w)
        //        c = (u * v) / u.(v * w)
        // 
        // Reminder: determinant of M is equal to scalar triple product:
        // |u v w| = [u v w] = u.(v * w) == v.(w * u) == w.(u * v)

        //  M = [u v w] -> extract u, v, w
        const __m128 u = _mm_shuffle_ps(vTemp0, M.vRow[2], V_SHUFFLE_XYZW(0, 2, 0, 0));
        const __m128 v = _mm_shuffle_ps(vTemp0, M.vRow[2], V_SHUFFLE_XYZW(1, 3, 1, 0));
        const __m128 w = _mm_shuffle_ps(vTemp1, M.vRow[2], V_SHUFFLE_XYZW(0, 2, 2, 0));

        const __m128 vCrossW = cross(v, w);
        const __m128 uDotvCrossW = _mm_dp_ps(u, vCrossW, 0xff);
        const __m128 detRcp = _mm_div_ps(vOne, uDotvCrossW);

        __m128 wCrossu = cross(w, u);
        __m128 uCrossv = cross(u, v);
        const __m128 vTinv = _mm_insert_ps(negate(M.vRow[3]), vOne, 0x30);

        v_float4x4 vI;
        vI.vRow[0] = _mm_insert_ps(_mm_mul_ps(vCrossW, detRcp), vZero, 0x30);
        vI.vRow[1] = _mm_insert_ps(_mm_mul_ps(wCrossu, detRcp), vZero, 0x30);
        vI.vRow[2] = _mm_insert_ps(_mm_mul_ps(uCrossv, detRcp), vZero, 0x30);

        vI.vRow[3] = _mm_mul_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(0, 0, 0, 0)), vI.vRow[0]);
        vI.vRow[3] = _mm_fmadd_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(1, 1, 1, 1)), vI.vRow[1], vI.vRow[3]);
        vI.vRow[3] = _mm_fmadd_ps(_mm_shuffle_ps(vTinv, vTinv, V_SHUFFLE_XYZW(2, 2, 2, 2)), vI.vRow[2], vI.vRow[3]);
        vI.vRow[3] = _mm_insert_ps(vI.vRow[3], vOne, 0x30);

        return vI;
    }

    ZetaInline v_float4x4 __vectorcall scale(float sx, float sy, float sz)
    {
        float4a f(sx, sy, sz, 1.0f);

        const __m128 vZero = _mm_setzero_ps();
        const __m128 vS = _mm_load_ps(reinterpret_cast<float*>(&f));

        return v_float4x4(_mm_blend_ps(vZero, vS, V_BLEND_XYZW(1, 0, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 1, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 0, 1, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 0, 0, 1)));
    }

    ZetaInline v_float4x4 __vectorcall scale(float4a s)
    {
        s.w = 1.0f;
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vS = _mm_load_ps(reinterpret_cast<float*>(&s));

        return v_float4x4(_mm_blend_ps(vZero, vS, V_BLEND_XYZW(1, 0, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 1, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 0, 1, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 0, 0, 1)));
    }

    ZetaInline v_float4x4 __vectorcall scale(const __m128 vS)
    {
        const __m128 vZero = _mm_setzero_ps();

        return v_float4x4(_mm_blend_ps(vZero, vS, V_BLEND_XYZW(1, 0, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 1, 0, 0)),
            _mm_blend_ps(vZero, vS, V_BLEND_XYZW(0, 0, 1, 0)),
            _mm_blend_ps(vZero, _mm_set1_ps(1.0f), V_BLEND_XYZW(0, 0, 0, 1)));
    }

    ZetaInline v_float4x4 __vectorcall rotate(const __m128 vN, float angle)
    {
        v_float4x4 vR;

        const float c = cosf(angle);
        const float s = sinf(angle);

        const __m128 vC = _mm_set1_ps(c);
        const __m128 v1subC = _mm_set1_ps(1.0f - c);
        const __m128 vS = _mm_set1_ps(s);
        //__m128 vN = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<double*>(&n)));
        //vN = _mm_insert_ps(vN, _mm_load_ss(&n.z), 0x20);

        __m128 vYZX = _mm_shuffle_ps(vN, vN, V_SHUFFLE_XYZW(1, 2, 0, 0));
        __m128 vTemp0 = _mm_mul_ps(vN, vN);
        vTemp0 = _mm_mul_ps(vTemp0, v1subC);    // ((1 - c)x^2, (1 - c)y^2, (1 - c)z^2)
        __m128 vTemp1 = _mm_mul_ps(vN, vYZX);
        vTemp1 = _mm_mul_ps(vTemp1, v1subC);    // ((1 - c)xy, (1 - c)yz, (1 - c)xz)
        __m128 vTemp2 = _mm_mul_ps(vN, vS);     // (sx, sy, sz)

        __m128 vTemp3 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(2, 0, 1, 0));

        vTemp2 = _mm_sub_ps(vTemp1, vTemp3);    // ((1 - c)xy - sz, (1 - c)yz - sx, (1 - c)xz - sy)
        vTemp3 = _mm_add_ps(vTemp1, vTemp3);    // ((1 - c)xy + sz, (1 - c)yz + sx, (1 - c)xz + sy)
        vTemp0 = _mm_add_ps(vTemp0, vC);        // (c + (1 - c)x^2, c + (1 - c)y^2, c + (1 - c)z^2)
        vTemp1 = _mm_add_ps(vC, v1subC);

        vR.vRow[0] = _mm_insert_ps(_mm_shuffle_ps(vTemp3, vTemp2, V_SHUFFLE_XYZW(0, 0, 2, 2)), vTemp0, 0x8);
        vR.vRow[1] = _mm_insert_ps(_mm_shuffle_ps(vTemp2, vTemp3, V_SHUFFLE_XYZW(0, 0, 1, 1)), vTemp0, 0x58);
        vR.vRow[2] = _mm_insert_ps(_mm_insert_ps(vTemp0, vTemp3, 0x8a), vTemp2, 0x58);
        vR.vRow[3] = _mm_insert_ps(vTemp1, vTemp1, 0x7);

        return vR;
    }

    ZetaInline v_float4x4 __vectorcall rotateX(float angle)
    {
        v_float4x4 vR;

        const float c = cosf(angle);
        const float s = sinf(angle);
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vC = _mm_broadcast_ss(&c);
        const __m128 vS = _mm_broadcast_ss(&s);
        const __m128 vMinusS = negate(vS);

        vR.vRow[0] = _mm_insert_ps(vZero, vOne, 0x0);
        vR.vRow[1] = _mm_insert_ps(vC, vS, 0x29);
        vR.vRow[2] = _mm_insert_ps(vC, vMinusS, 0x19);
        vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

        return vR;
    }

    ZetaInline v_float4x4 __vectorcall rotateY(float angle)
    {
        v_float4x4 vR;

        const float c = cosf(angle);
        const float s = sinf(angle);
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vC = _mm_broadcast_ss(&c);
        const __m128 vS = _mm_broadcast_ss(&s);
        const __m128 vMinusS = negate(vS);

        vR.vRow[0] = _mm_insert_ps(vC, vMinusS, 0x2a);
        vR.vRow[1] = _mm_insert_ps(vZero, vOne, 0x10);
        vR.vRow[2] = _mm_insert_ps(vC, vS, 0xa);
        vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

        return vR;
    }

    ZetaInline v_float4x4 __vectorcall rotateZ(float angle)
    {
        v_float4x4 vR;

        const float c = cosf(angle);
        const float s = sinf(angle);
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set1_ps(1.0f);
        const __m128 vC = _mm_broadcast_ss(&c);
        const __m128 vS = _mm_broadcast_ss(&s);
        const __m128 vMinusS = negate(vS);

        vR.vRow[0] = _mm_insert_ps(vC, vS, 0x1c);
        vR.vRow[1] = _mm_insert_ps(vC, vMinusS, 0xc);
        vR.vRow[2] = _mm_insert_ps(vZero, vOne, 0x20);
        vR.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);

        return vR;
    }

    // Returns a rotation matrix from the given unit quaternion
    ZetaInline v_float4x4 __vectorcall rotationMatFromQuat(const __m128 vQ)
    {
        // (q1^2, q2^2, q3^2, q4^2)
        const __m128 vQ2 = _mm_mul_ps(vQ, vQ);
        const __m128 vMin2 = _mm_set1_ps(-2.0f);
        const __m128 v2 = _mm_set1_ps(2.0f);
        const __m128 vOne = _mm_set1_ps(1.0f);

        // (q3^2, q3^2, q2^2, _)
        __m128 vTemp0 = _mm_shuffle_ps(vQ2, vQ2, V_SHUFFLE_XYZW(2, 2, 1, 0));

        // (q1^2 + q3^2, q2^2 + q3^2, q1^2 + q2^2, _)
        __m128 vTemp1 = _mm_add_ps(_mm_shuffle_ps(vQ2, vQ2, V_SHUFFLE_XYZW(0, 1, 0, 0)), vTemp0);

        // (1 - 2 * q1^2 - 2 * q3^2, 1 - 2 * q2^2 - 2 * q3^2, 1 - 2 *q1^2 - 2 * q2^2, _)
        const __m128 vDiag = _mm_fmadd_ps(vTemp1, vMin2, vOne);

        // (2q1q4, 2q2q4, 2q1q3, 2q3q4)
        __m128 vTemp3 = _mm_mul_ps(vQ, _mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(3, 3, 0, 2)));
        vTemp3 = _mm_mul_ps(vTemp3, v2);
        // (2q1q2, 2q2q3, 2q3q4, 2q1q3)
        __m128 vTemp2 = _mm_mul_ps(vQ, _mm_shuffle_ps(vQ, vQ, V_SHUFFLE_XYZW(1, 2, 3, 0)));
        vTemp2 = _mm_mul_ps(vTemp2, v2);
        vTemp2 = _mm_insert_ps(vTemp2, vTemp3, 0xb0);

        // (2q1q2, 2q1q3, 2q2q3, 2q1q2)
        __m128 vTemp4 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(0, 3, 1, 0));
        // (2q3q4, 2q2q4, 2q1q4, 2q3q4)
        __m128 vTemp5 = _mm_shuffle_ps(vTemp3, vTemp3, V_SHUFFLE_XYZW(3, 1, 0, 3));

        // (2q1q2 + 2q3q4, 2q1q3 - 2q2q4, 2q2q3 + 2q1q4, 2q1q2 - 2q3q4)
        vTemp0 = _mm_addsub_ps(vTemp4, negate(vTemp5));

        // (2q1q3, 2q2q3, _, _)
        __m128 vTemp6 = _mm_shuffle_ps(vTemp2, vTemp2, V_SHUFFLE_XYZW(3, 1, 0, 0));
        // (2q2q4, 2q1q4, _, _)
        __m128 vTemp7 = _mm_shuffle_ps(vTemp3, vTemp3, V_SHUFFLE_XYZW(1, 0, 0, 0));

        // (2q1q3 + 2q2q4, 2q2q3 - 2q1q4, _, _)
        vTemp1 = _mm_addsub_ps(vTemp6, negate(vTemp7));

        v_float4x4 vR;
        vR.vRow[0] = _mm_insert_ps(_mm_shuffle_ps(vTemp0, vTemp0, V_SHUFFLE_XYZW(0, 0, 1, 0)),
            vDiag, 0x48);
        vR.vRow[1] = _mm_insert_ps(_mm_shuffle_ps(vTemp0, vTemp0, V_SHUFFLE_XYZW(3, 0, 2, 0)),
            vDiag, 0x18);
        vR.vRow[2] = _mm_insert_ps(vTemp1, vDiag, 0xa8);
        vR.vRow[3] = _mm_insert_ps(vOne, vOne, 0xf7);

        return vR;
    }

    // Ported from DirectXMath (under MIT License).
    ZetaInline __m128 __vectorcall quaternionFromRotationMat(const v_float4x4 vM)
    {
        __m128 r0 = vM.vRow[0];  // (r00, r01, r02, 0)
        __m128 r1 = vM.vRow[1];  // (r10, r11, r12, 0)
        __m128 r2 = vM.vRow[2];  // (r20, r21, r22, 0)

        // (r00, r00, r00, r00)
        __m128 r00 = _mm_permute_ps(r0, _MM_SHUFFLE(0, 0, 0, 0));
        // (r11, r11, r11, r11)
        __m128 r11 = _mm_permute_ps(r1, _MM_SHUFFLE(1, 1, 1, 1));
        // (r22, r22, r22, r22)
        __m128 r22 = _mm_permute_ps(r2, _MM_SHUFFLE(2, 2, 2, 2));

        // x^2 >= y^2 equivalent to r11 - r00 <= 0
        // (r11 - r00, r11 - r00, r11 - r00, r11 - r00)
        __m128 r11mr00 = _mm_sub_ps(r11, r00);
        __m128 x2gey2 = _mm_cmple_ps(r11mr00, _mm_setzero_ps());

        // z^2 >= w^2 equivalent to r11 + r00 <= 0
        // (r11 + r00, r11 + r00, r11 + r00, r11 + r00)
        __m128 r11pr00 = _mm_add_ps(r11, r00);
        __m128 z2gew2 = _mm_cmple_ps(r11pr00, _mm_setzero_ps());

        // x^2 + y^2 >= z^2 + w^2 equivalent to r22 <= 0
        __m128 x2py2gez2pw2 = _mm_cmple_ps(r22, _mm_setzero_ps());

        // (4*x^2, 4*y^2, 4*z^2, 4*w^2)
        __m128 XMPMMP = _mm_setr_ps(+1.0f, -1.0f, -1.0f, +1.0f);
        __m128 XMMPMP = _mm_setr_ps(-1.0f, +1.0f, -1.0f, +1.0f);
        __m128 XMMMPP = _mm_setr_ps(-1.0f, -1.0f, +1.0f, +1.0f);

        __m128 t0 = _mm_fmadd_ps(XMPMMP, r00, _mm_set1_ps(1.0f));
        __m128 t1 = _mm_mul_ps(XMMPMP, r11);
        __m128 t2 = _mm_fmadd_ps(XMMMPP, r22, t0);
        __m128 x2y2z2w2 = _mm_add_ps(t1, t2);

        // (r01, r02, r12, r11)
        t0 = _mm_shuffle_ps(r0, r1, _MM_SHUFFLE(1, 2, 2, 1));
        // (r10, r10, r20, r21)
        t1 = _mm_shuffle_ps(r1, r2, _MM_SHUFFLE(1, 0, 0, 0));
        // (r10, r20, r21, r10)
        t1 = _mm_permute_ps(t1, _MM_SHUFFLE(1, 3, 2, 0));
        // (4*x*y, 4*x*z, 4*y*z, unused)
        __m128 xyxzyz = _mm_add_ps(t0, t1);

        // (r21, r20, r10, r10)
        t0 = _mm_shuffle_ps(r2, r1, _MM_SHUFFLE(0, 0, 0, 1));
        // (r12, r12, r02, r01)
        t1 = _mm_shuffle_ps(r1, r0, _MM_SHUFFLE(1, 2, 2, 2));
        // (r12, r02, r01, r12)
        t1 = _mm_permute_ps(t1, _MM_SHUFFLE(1, 3, 2, 0));
        // (4*x*w, 4*y*w, 4*z*w, unused)
        __m128 xwywzw = _mm_sub_ps(t0, t1);
        xwywzw = _mm_mul_ps(XMMPMP, xwywzw);

        // (4*x^2, 4*y^2, 4*x*y, unused)
        t0 = _mm_shuffle_ps(x2y2z2w2, xyxzyz, _MM_SHUFFLE(0, 0, 1, 0));
        // (4*z^2, 4*w^2, 4*z*w, unused)
        t1 = _mm_shuffle_ps(x2y2z2w2, xwywzw, _MM_SHUFFLE(0, 2, 3, 2));
        // (4*x*z, 4*y*z, 4*x*w, 4*y*w)
        t2 = _mm_shuffle_ps(xyxzyz, xwywzw, _MM_SHUFFLE(1, 0, 2, 1));

        // (4*x*x, 4*x*y, 4*x*z, 4*x*w)
        __m128 tensor0 = _mm_shuffle_ps(t0, t2, _MM_SHUFFLE(2, 0, 2, 0));
        // (4*y*x, 4*y*y, 4*y*z, 4*y*w)
        __m128 tensor1 = _mm_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 1, 1, 2));
        // (4*z*x, 4*z*y, 4*z*z, 4*z*w)
        __m128 tensor2 = _mm_shuffle_ps(t2, t1, _MM_SHUFFLE(2, 0, 1, 0));
        // (4*w*x, 4*w*y, 4*w*z, 4*w*w)
        __m128 tensor3 = _mm_shuffle_ps(t2, t1, _MM_SHUFFLE(1, 2, 3, 2));

        // Select the row of the tensor-product matrix that has the largest
        // magnitude.
        t0 = _mm_and_ps(x2gey2, tensor0);
        t1 = _mm_andnot_ps(x2gey2, tensor1);
        t0 = _mm_or_ps(t0, t1);
        t1 = _mm_and_ps(z2gew2, tensor2);
        t2 = _mm_andnot_ps(z2gew2, tensor3);
        t1 = _mm_or_ps(t1, t2);
        t0 = _mm_and_ps(x2py2gez2pw2, t0);
        t1 = _mm_andnot_ps(x2py2gez2pw2, t1);
        t2 = _mm_or_ps(t0, t1);

        // Normalize the row.  No division by zero is possible because the
        // quaternion is unit-length (and the row is a nonzero multiple of
        // the quaternion).
        t0 = normalize(t2);
        return t0;
    }

    // Ref: https://math.stackexchange.com/questions/893984/conversion-of-rotation-matrix-to-quaternion
    // "Converting a Rotation Matrix to a Quaternion", Mike Day, Insomniac Games.
    ZetaInline float4 __vectorcall quaternionFromRotationMat1(const v_float4x4 vM)
    {
        float4a row0 = store(vM.vRow[0]);
        float4a row1 = store(vM.vRow[1]);
        float4a row2 = store(vM.vRow[2]);

        float t[4];
        float4 q[4];

        t[0] = 1 + row0.x - row1.y - row2.z;
        t[1] = 1 - row0.x + row1.y - row2.z;
        t[2] = 1 - row0.x - row1.y + row2.z;
        t[3] = 1 + row0.x + row1.y + row2.z;

        q[0] = float4(t[0], row0.y + row1.x, row2.x + row0.z, row1.z - row2.y);
        q[1] = float4(row0.y + row1.x, t[1], row1.z + row2.y, row2.x - row0.z);
        q[2] = float4(row2.x + row0.z, row1.z + row2.y, t[2], row0.y - row1.x);
        q[3] = float4(row1.z - row2.y, row2.x - row0.z, row0.y - row1.x, t[3]);

        int i = (row2.z >= 0) * (2 + (row0.x >= -row1.y)) + (row2.z < 0) * (row1.y >= row0.x);
        float4 q_i = q[i];
        float t_i = t[i];
        q_i *= 0.5f / sqrtf(t_i);

        return q_i;
    }

    ZetaInline v_float4x4 __vectorcall translate(float x, float y, float z)
    {
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set_ps1(1.0f);
        const __m128 vT = _mm_setr_ps(x, y, z, 0.0f);

        return v_float4x4(_mm_blend_ps(vZero, vOne, V_BLEND_XYZW(1, 0, 0, 0)),
            _mm_blend_ps(vZero, vOne, V_BLEND_XYZW(0, 1, 0, 0)),
            _mm_blend_ps(vZero, vOne, V_BLEND_XYZW(0, 0, 1, 0)),
            _mm_blend_ps(vT, vOne, V_BLEND_XYZW(0, 0, 0, 1)));
    }

    ZetaInline v_float4x4 __vectorcall translate(float4a t)
    {
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set_ps1(1.0f);
        const __m128 vT = _mm_load_ps(reinterpret_cast<float*>(&t));

        return v_float4x4(_mm_blend_ps(vZero, vOne, V_BLEND_XYZW(1, 0, 0, 0)),
            _mm_blend_ps(vZero, vOne, V_BLEND_XYZW(0, 1, 0, 0)),
            _mm_blend_ps(vZero, vOne, V_BLEND_XYZW(0, 0, 1, 0)),
            _mm_blend_ps(vT, vOne, V_BLEND_XYZW(0, 0, 0, 1)));
    }

    ZetaInline v_float4x4 __vectorcall affineTransformation(float3& s, float4& q, float3& t)
    {
        v_float4x4 vSRT;
        v_float4x4 vR = rotationMatFromQuat(loadFloat4(q));

        // Since scale matrix is diagonal, matrix multiplication has a simple form
        const __m128 vS = loadFloat3(s);    // vS[3] = 0
        vSRT.vRow[0] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(0, 0, 0, 3)), vR.vRow[0]);
        vSRT.vRow[1] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(1, 1, 1, 3)), vR.vRow[1]);
        vSRT.vRow[2] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(2, 2, 2, 3)), vR.vRow[2]);

        vSRT.vRow[3] = loadFloat3(t);

        // SRT_{3,3} = 1.0
        const __m128 vOne = _mm_set_ps1(1.0f);
        vSRT.vRow[3] = _mm_blend_ps(vSRT.vRow[3], vOne, V_BLEND_XYZW(0, 0, 0, 1));

        return vSRT;
    }

    ZetaInline v_float4x4 __vectorcall affineTransformation(const __m128 vS, const __m128 vQ, const __m128 vT)
    {
        v_float4x4 vSRT;
        v_float4x4 vR = rotationMatFromQuat(vQ);

        // Since scale matrix is diagonal, matrix multiplication has a simple form
        vSRT.vRow[0] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(0, 0, 0, 0)), vR.vRow[0]);    // vR.vRow[0].w = 0
        vSRT.vRow[1] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(1, 1, 1, 0)), vR.vRow[1]);    // vR.vRow[1].w = 0
        vSRT.vRow[2] = _mm_mul_ps(_mm_shuffle_ps(vS, vS, V_SHUFFLE_XYZW(2, 2, 2, 0)), vR.vRow[2]);    // vR.vRow[2].w = 0

        // M_{3,3} = 1.0
        const __m128 vOne = _mm_set_ps1(1.0f);
        vSRT.vRow[3] = _mm_blend_ps(vT, vOne, V_BLEND_XYZW(0, 0, 0, 1));

        return vSRT;
    }

    // Note: doesn't support negative scaling
    ZetaInline void __vectorcall decomposeTRS(const v_float4x4 vM, float3& s, float4& r, float3& t)
    {
        // Given the transformation matrix M = TRS, M is easily decomposed into T and RS. That just leaves the
        // RS part.
        __m128 vT = _mm_insert_ps(vM.vRow[0], vM.vRow[0], 0xce);    // (m03, 0, 0, 0)
        vT = _mm_insert_ps(vT, vM.vRow[1], 0xd0);                   // (m03, m13, 0, 0)
        vT = _mm_insert_ps(vT, vM.vRow[2], 0xe0);                   // (m03, m13, m23, 0)
        t = storeFloat3(vT);

        // Columns of linear transformation matrices are the transformations of (orthogonal) standard basis 
        // vectors; for RS, columns of R are the rotated standard basis vectors and diagonal entries of S 
        // their corresponding length.
        // 
        // Given the Singular value decompoistion of A = U E V^T, columns of U are the orthonormal transformation
        // of standard basis vectors (i.e. R), diagonal elements of E their lengths (scale factors) and rows 
        // of V^T are the standard basis vectors (whose transforms are the columns of U -- AV = UE). While singular 
        // values are unique, U & V aren't, but in this case knowing the singular values is enough.
        // 
        // To compute the singular values, compute the eigenvalues of M^T M, whose square roots are the singular 
        // values and therefore the scale factors. Now that we know S, R can solved for as follows:
        //
        //        RS = R * S
        //        RS * S^-1 = R
        //
        // where S^-1 is a diagonal matrix with diagonal entries (1/s_x, 1/s_y, 1/s_z).

        // M^T M = (RS)^T RS = S^T R^T R S = S^T S = S^2
        const __m128 vZero = _mm_setzero_ps();
        const __m128 vOne = _mm_set1_ps(1.0f);
        v_float4x4 vM3x3 = vM;
        vM3x3.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);    // M[3] = (0, 0, 0, 1)

        v_float4x4 vM3x3T = transpose(vM3x3);
        v_float4x4 vMTxM = mul(vM3x3T, vM3x3);

        // Eigenvalues of diagonal matrices are the diagonal entries
        __m128 vS = _mm_blend_ps(vMTxM.vRow[0], vMTxM.vRow[1], V_BLEND_XYZW(0, 1, 0, 0)); // (s_x^2, s_y^2, _, 0)
        vS = _mm_blend_ps(vS, vMTxM.vRow[2], V_BLEND_XYZW(0, 0, 1, 0));                   // (s_x^2, s_y^2, s_z^2, 0)
        vS = _mm_blend_ps(vS, vOne, V_BLEND_XYZW(0, 0, 0, 1));                            // (s_x^2, s_y^2, s_z^2, 1)

        // Singular values are the square roots of eigenvalues
        vS = _mm_sqrt_ps(vS);
        s = storeFloat3(vS);

        // Solve for R
        __m128 vInvSDiag = _mm_div_ps(vOne, vS);
        v_float4x4 vSinv = scale(vInvSDiag);

        // R = RS * S^-1
        v_float4x4 vR = mul(vM3x3, vSinv);

        // routines below expect "row" matrices.
        vR = transpose3x3(vR);

#if 0
        __m128 vQ = quaternionFromRotationMat(vR);
        r = storeFloat4(vQ);
#else
        r = quaternionFromRotationMat1(vR);
#endif
    }

    // Note: doesn't support negative scaling
    ZetaInline void __vectorcall decomposeSRT(const v_float4x4 vM, float4a& s, float4a& r, float4a& t)
    {
        // Refer to notes in decomposeTRS for explanation

        __m128 vT = vM.vRow[3];
        const __m128 vZero = _mm_setzero_ps();
        vT = _mm_blend_ps(vT, vZero, V_BLEND_XYZW(0, 0, 0, 1));
        t = store(vT);

        v_float4x4 vM3x3 = vM;
        const __m128 vOne = _mm_set1_ps(1.0f);
        vM3x3.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);    // M[3] = (0, 0, 0, 1)

        // For "row" matrices, square roots of eigenvalues of MM^T are the singular values
        // M M^T = (SR)(SR)^T = S R R^T S^T = S S^T = S^2
        const v_float4x4 vM3x3T = transpose3x3(vM3x3);
        const v_float4x4 vMxMT = mul(vM3x3, vM3x3T);

        // Eigenvalues of diagonal matrices are the diagonal entries
        __m128 vS = _mm_blend_ps(vMxMT.vRow[0], vMxMT.vRow[1], V_BLEND_XYZW(0, 1, 0, 0)); // (s_x^2, s_y^2, _, 0)
        vS = _mm_blend_ps(vS, vMxMT.vRow[2], V_BLEND_XYZW(0, 0, 1, 0));                   // (s_x^2, s_y^2, s_z^2, 0)
        vS = _mm_blend_ps(vS, vOne, V_BLEND_XYZW(0, 0, 0, 1));                            // (s_x^2, s_y^2, s_z^2, 1)

        // Singular values are the square roots of eigenvalues
        vS = _mm_sqrt_ps(vS);
        s = store(vS);

        // R = S^-1 * SR
        const __m128 vInvSDiag = _mm_div_ps(vOne, vS);
        const v_float4x4 vSinv = scale(vInvSDiag);
        const v_float4x4 vR = mul(vSinv, vM3x3);
#if 0
        const __m128 vQ = quaternionFromRotationMat(vR);
        r = store(vQ);
#else
        r = quaternionFromRotationMat1(vR);
#endif
    }

    ZetaInline v_float4x4 __vectorcall inverseAndDecomposeSRT(const v_float4x4 vM, float4a& s,
        float4a& r, float4a& t)
    {
        __m128 vT = vM.vRow[3];
        const __m128 vZero = _mm_setzero_ps();
        vT = _mm_blend_ps(vT, vZero, V_BLEND_XYZW(0, 0, 0, 1));
        t = store(vT);

        v_float4x4 vM3x3 = vM;
        const __m128 vOne = _mm_set1_ps(1.0f);
        vM3x3.vRow[3] = _mm_insert_ps(vZero, vOne, 0x30);    // M[3] = (0, 0, 0, 1)

        // For "row" matrices, square roots of eigenvalues of MM^T are the singular values
        // M M^T = (SR)(SR)^T = S R R^T S^T = S S^T = S^2
        const v_float4x4 vM3x3T = transpose3x3(vM3x3);
        const v_float4x4 vMxMT = mul(vM3x3, vM3x3T);

        // Eigenvalues of diagonal matrices are the diagonal entries
        __m128 vS = _mm_blend_ps(vMxMT.vRow[0], vMxMT.vRow[1], V_BLEND_XYZW(0, 1, 0, 0)); // (s_x^2, s_y^2, _, 0)
        vS = _mm_blend_ps(vS, vMxMT.vRow[2], V_BLEND_XYZW(0, 0, 1, 0));                   // (s_x^2, s_y^2, s_z^2, 0)
        vS = _mm_blend_ps(vS, vOne, V_BLEND_XYZW(0, 0, 0, 1));                            // (s_x^2, s_y^2, s_z^2, 1)

        // Singular values are the square roots of eigenvalues
        vS = _mm_sqrt_ps(vS);
        s = store(vS);

        // R = S^-1 * SR
        const __m128 vInvSDiag = _mm_div_ps(vOne, vS);
        const v_float4x4 vSinv = scale(vInvSDiag);
        const v_float4x4 vR = mul(vSinv, vM3x3);
        r = quaternionFromRotationMat1(vR);

        v_float4x4 vInv = mul(transpose(vR), vSinv);
        vInv.vRow[3] = mul(vInv, negate(vT));
        // Set entry (3, 3) to 1
        vInv.vRow[3] = _mm_insert_ps(vInv.vRow[3], vOne, 0x30);

        return vInv;
    }

    ZetaInline v_float4x4 __vectorcall lookAtLH(float4a cameraPos, float4a focus, float4a up)
    {
        v_float4x4 vM = identity();

        // Builds a coordinate system uvw, where w is aligned with the camera direction
        __m128 vCamPos = _mm_load_ps(reinterpret_cast<float*>(&cameraPos));
        __m128 vFocus = _mm_load_ps(reinterpret_cast<float*>(&focus));
        __m128 vUp = _mm_load_ps(reinterpret_cast<float*>(&up));

        __m128 vW = _mm_sub_ps(vFocus, vCamPos);
        vW = normalize(vW);
        __m128 vU = cross(vUp, vW);
        vU = normalize(vU);
        __m128 vV = cross(vW, vU);    // no need to normalize as ||vV|| = 1

        vM.vRow[0] = vU;
        vM.vRow[1] = vV;
        vM.vRow[2] = vW;
        vM = transpose(vM);

        __m128 vTemp = _mm_mul_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(0, 0, 0, 0)), vM.vRow[0]);
        vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(1, 1, 1, 0)), vM.vRow[1], vTemp);
        vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(2, 2, 2, 0)), vM.vRow[2], vTemp);

        vM.vRow[3] = _mm_insert_ps(negate(vTemp), vM.vRow[3], 0xf0);

        return vM;
    }

    ZetaInline v_float4x4 __vectorcall lookToLH(float4a cameraPos, float4a viewDir, float4a up)
    {
        v_float4x4 vM = identity();

        // Builds a coordinate system uvw, where w is aligned with the camera direction
        __m128 vCamPos = _mm_load_ps(reinterpret_cast<float*>(&cameraPos));
        __m128 vUp = _mm_load_ps(reinterpret_cast<float*>(&up));
        __m128 vW = _mm_load_ps(reinterpret_cast<float*>(&viewDir));

        vW = normalize(vW);
        __m128 vU = cross(vUp, vW);
        vU = normalize(vU);
        __m128 vV = cross(vW, vU);    // no need to normalize as ||vV|| = 1

        vM.vRow[0] = vU;
        vM.vRow[1] = vV;
        vM.vRow[2] = vW;
        vM = transpose(vM);

        __m128 vTemp = _mm_mul_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(0, 0, 0, 0)), vM.vRow[0]);
        vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(1, 1, 1, 0)), vM.vRow[1], vTemp);
        vTemp = _mm_fmadd_ps(_mm_shuffle_ps(vCamPos, vCamPos, V_SHUFFLE_XYZW(2, 2, 2, 0)), vM.vRow[2], vTemp);

        vM.vRow[3] = _mm_insert_ps(negate(vTemp), vM.vRow[3], 0xf0);

        return vM;
    }

    ZetaInline v_float4x4 __vectorcall perspective(float aspectRatio, float vFOV, float nearZ, float farZ)
    {
        v_float4x4 P;

        float t = 1.0f / tanf(0.5f * vFOV);
        float fSubn = farZ / (farZ - nearZ);

        __m128 vTemp = _mm_setr_ps(t / aspectRatio, t, fSubn, -nearZ * fSubn);
        const __m128 vOne = _mm_set1_ps(1.0f);
        P.vRow[0] = _mm_insert_ps(vTemp, vTemp, 0xe);
        P.vRow[1] = _mm_insert_ps(vTemp, vTemp, 0xd);
        P.vRow[2] = _mm_insert_ps(vTemp, vOne, 0x33);
        P.vRow[3] = _mm_insert_ps(vTemp, vTemp, 0xeb);

        return P;
    }

    ZetaInline v_float4x4 __vectorcall perspectiveReverseZ(float aspectRatio, float vFOV, float nearZ)
    {
        v_float4x4 P;

        float t = 1.0f / tanf(0.5f * vFOV);

        __m128 vTemp = _mm_setr_ps(t / aspectRatio, t, 0.0f, nearZ);
        const __m128 vOne = _mm_set1_ps(1.0f);
        P.vRow[0] = _mm_insert_ps(vTemp, vTemp, 0xe);
        P.vRow[1] = _mm_insert_ps(vTemp, vTemp, 0xd);
        P.vRow[2] = _mm_insert_ps(vTemp, vOne, 0x33);
        P.vRow[3] = _mm_insert_ps(vTemp, vTemp, 0xeb);

        return P;
    }

    ZetaInline v_float4x4 __vectorcall perspectiveReverseZ(float aspectRatio, float vFOV, float nearZ,
        float farZ)
    {
        v_float4x4 P;

        float t = 1.0f / tanf(0.5f * vFOV);
        float q = nearZ / (farZ - nearZ);

        __m128 vTemp = _mm_setr_ps(t / aspectRatio, t, -nearZ / (farZ - nearZ), q * farZ);
        const __m128 vOne = _mm_set1_ps(1.0f);
        P.vRow[0] = _mm_insert_ps(vTemp, vTemp, 0xe);
        P.vRow[1] = _mm_insert_ps(vTemp, vTemp, 0xd);
        P.vRow[2] = _mm_insert_ps(vTemp, vOne, 0x33);
        P.vRow[3] = _mm_insert_ps(vTemp, vTemp, 0xeb);

        return P;
    }

    ZetaInline bool __vectorcall equal(v_float4x4 vM1, v_float4x4 vM2)
    {
        const __m256 vEps = _mm256_set1_ps(FLT_EPSILON);

        const __m256 vTemp1 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM1.vRow[0]), vM1.vRow[1], 0x1);
        const __m256 vTemp2 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM1.vRow[2]), vM1.vRow[3], 0x1);
        const __m256 vTemp3 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM2.vRow[0]), vM2.vRow[1], 0x1);
        const __m256 vTemp4 = _mm256_insertf128_ps(_mm256_castps128_ps256(vM2.vRow[2]), vM2.vRow[3], 0x1);

        const __m256 vRes1 = _mm256_cmp_ps(vEps, abs(_mm256_sub_ps(vTemp1, vTemp3)), _CMP_GE_OQ);
        const __m256 vRes2 = _mm256_cmp_ps(vEps, abs(_mm256_sub_ps(vTemp2, vTemp4)), _CMP_GE_OQ);

        int r1 = _mm256_movemask_ps(vRes1);
        int r2 = _mm256_movemask_ps(vRes2);

        return (r1 & r2) == 0xff;
    }

    ZetaInline v_float4x4 __vectorcall load4x4(float4x4a M)
    {
        v_float4x4 vM;

        vM.vRow[0] = _mm_load_ps(reinterpret_cast<float*>(&M.m[0]));
        vM.vRow[1] = _mm_load_ps(reinterpret_cast<float*>(&M.m[1]));
        vM.vRow[2] = _mm_load_ps(reinterpret_cast<float*>(&M.m[2]));
        vM.vRow[3] = _mm_load_ps(reinterpret_cast<float*>(&M.m[3]));

        return vM;
    }

    ZetaInline v_float4x4 __vectorcall load4x3(float4x3 M)
    {
        v_float4x4 vM;
        float4x4a temp(M);

        vM.vRow[0] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[0]));
        vM.vRow[1] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[1]));
        vM.vRow[2] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[2]));
        vM.vRow[3] = _mm_load_ps(reinterpret_cast<float*>(&temp.m[3]));

        return vM;
    }

    ZetaInline float4x4a __vectorcall store(v_float4x4 M)
    {
        float4x4a m;

        _mm_store_ps(reinterpret_cast<float*>(&m.m[0]), M.vRow[0]);
        _mm_store_ps(reinterpret_cast<float*>(&m.m[1]), M.vRow[1]);
        _mm_store_ps(reinterpret_cast<float*>(&m.m[2]), M.vRow[2]);
        _mm_store_ps(reinterpret_cast<float*>(&m.m[3]), M.vRow[3]);

        return m;
    }
}