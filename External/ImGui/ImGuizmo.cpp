// https://github.com/CedricGuillemet/ImGuizmo
// v1.91.3 WIP
//
// The MIT License(MIT)
//
// Copyright(c) 2021 Cedric Guillemet
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "../ZetaCore/App/App.h"
#include "../ZetaCore/Math/MatrixFuncs.h"
#include "../ZetaCore/Core/RendererCore.h"
#include "../ZetaCore/Scene/Camera.h"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif
#if !defined(_MSC_VER) && !defined(__MINGW64_VERSION_MAJOR)
#define _malloca(x) alloca(x)
#define _freea(x)
#endif

// includes patches for multiview from
// https://github.com/CedricGuillemet/ImGuizmo/issues/15

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;

namespace IMGUIZMO_NAMESPACE
{
   static const float ZPI = 3.14159265358979323846f;
   static const float RAD2DEG = (180.f / ZPI);
   static const float DEG2RAD = (ZPI / 180.f);
   const float screenRotateSize = 0.06f;
   // scale a bit so translate axis do not touch when in universal
   const float rotationDisplayFactor = 1.2f;
   static constexpr ImGuiID INVALID_ID = ImGuiID(-1);

   static OPERATION operator&(OPERATION lhs, OPERATION rhs)
   {
     return static_cast<OPERATION>(static_cast<int>(lhs) & static_cast<int>(rhs));
   }

   static bool operator!=(OPERATION lhs, int rhs)
   {
     return static_cast<int>(lhs) != rhs;
   }

   static bool Intersects(OPERATION lhs, OPERATION rhs)
   {
     return (lhs & rhs) != 0;
   }

   // True if lhs contains rhs
   static bool Contains(OPERATION lhs, OPERATION rhs)
   {
     return (lhs & rhs) == rhs;
   }

   ////////////////////////////////////////////////////////////////////////////////

   void FPU_MatrixF_x_MatrixF(const float* a, const float* b, float* r)
   {
      r[0] = a[0] * b[0] + a[1] * b[4] + a[2] * b[8] + a[3] * b[12];
      r[1] = a[0] * b[1] + a[1] * b[5] + a[2] * b[9] + a[3] * b[13];
      r[2] = a[0] * b[2] + a[1] * b[6] + a[2] * b[10] + a[3] * b[14];
      r[3] = a[0] * b[3] + a[1] * b[7] + a[2] * b[11] + a[3] * b[15];

      r[4] = a[4] * b[0] + a[5] * b[4] + a[6] * b[8] + a[7] * b[12];
      r[5] = a[4] * b[1] + a[5] * b[5] + a[6] * b[9] + a[7] * b[13];
      r[6] = a[4] * b[2] + a[5] * b[6] + a[6] * b[10] + a[7] * b[14];
      r[7] = a[4] * b[3] + a[5] * b[7] + a[6] * b[11] + a[7] * b[15];

      r[8] = a[8] * b[0] + a[9] * b[4] + a[10] * b[8] + a[11] * b[12];
      r[9] = a[8] * b[1] + a[9] * b[5] + a[10] * b[9] + a[11] * b[13];
      r[10] = a[8] * b[2] + a[9] * b[6] + a[10] * b[10] + a[11] * b[14];
      r[11] = a[8] * b[3] + a[9] * b[7] + a[10] * b[11] + a[11] * b[15];

      r[12] = a[12] * b[0] + a[13] * b[4] + a[14] * b[8] + a[15] * b[12];
      r[13] = a[12] * b[1] + a[13] * b[5] + a[14] * b[9] + a[15] * b[13];
      r[14] = a[12] * b[2] + a[13] * b[6] + a[14] * b[10] + a[15] * b[14];
      r[15] = a[12] * b[3] + a[13] * b[7] + a[14] * b[11] + a[15] * b[15];
   }

   void Frustum(float left, float right, float bottom, float top, float znear, float zfar, float* m16)
   {
      float temp, temp2, temp3, temp4;
      temp = 2.0f * znear;
      temp2 = right - left;
      temp3 = top - bottom;
      temp4 = zfar - znear;
      m16[0] = temp / temp2;
      m16[1] = 0.0;
      m16[2] = 0.0;
      m16[3] = 0.0;
      m16[4] = 0.0;
      m16[5] = temp / temp3;
      m16[6] = 0.0;
      m16[7] = 0.0;
      m16[8] = (right + left) / temp2;
      m16[9] = (top + bottom) / temp3;
      m16[10] = (-zfar - znear) / temp4;
      m16[11] = -1.0f;
      m16[12] = 0.0;
      m16[13] = 0.0;
      m16[14] = (-temp * zfar) / temp4;
      m16[15] = 0.0;
   }

   void Perspective(float fovyInDegrees, float aspectRatio, float znear, float zfar, float* m16)
   {
      float ymax, xmax;
      ymax = znear * tanf(fovyInDegrees * DEG2RAD);
      xmax = ymax * aspectRatio;
      Frustum(-xmax, xmax, -ymax, ymax, znear, zfar, m16);
   }

   void Cross(const float* a, const float* b, float* r)
   {
      r[0] = a[1] * b[2] - a[2] * b[1];
      r[1] = a[2] * b[0] - a[0] * b[2];
      r[2] = a[0] * b[1] - a[1] * b[0];
   }

   float Dot(const float* a, const float* b)
   {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
   }

   void Normalize(const float* a, float* r)
   {
      float il = 1.f / (sqrtf(Dot(a, a)) + FLT_EPSILON);
      r[0] = a[0] * il;
      r[1] = a[1] * il;
      r[2] = a[2] * il;
   }

   void LookAt(const float* eye, const float* at, const float* up, float* m16)
   {
      float X[3], Y[3], Z[3], tmp[3];

      tmp[0] = eye[0] - at[0];
      tmp[1] = eye[1] - at[1];
      tmp[2] = eye[2] - at[2];
      Normalize(tmp, Z);
      Normalize(up, Y);
      Cross(Y, Z, tmp);
      Normalize(tmp, X);
      Cross(Z, X, tmp);
      Normalize(tmp, Y);

      m16[0] = X[0];
      m16[1] = Y[0];
      m16[2] = Z[0];
      m16[3] = 0.0f;
      m16[4] = X[1];
      m16[5] = Y[1];
      m16[6] = Z[1];
      m16[7] = 0.0f;
      m16[8] = X[2];
      m16[9] = Y[2];
      m16[10] = Z[2];
      m16[11] = 0.0f;
      m16[12] = -Dot(X, eye);
      m16[13] = -Dot(Y, eye);
      m16[14] = -Dot(Z, eye);
      m16[15] = 1.0f;
   }

   template <typename T> T Clamp(T x, T y, T z) { return ((x < y) ? y : ((x > z) ? z : x)); }
   template <typename T> T max(T x, T y) { return (x > y) ? x : y; }
   template <typename T> T min(T x, T y) { return (x < y) ? x : y; }
   template <typename T> bool IsWithin(T x, T y, T z) { return (x >= y) && (x <= z); }

   struct matrix_t;
   struct vec_t
   {
   public:
      float x, y, z, w;

      vec_t() = default;
      vec_t(float v0, float v1, float v2, float v3)
          : x(v0),
          y(v1),
          z(v2),
          w(v3)
      {}
      vec_t(const float3& v)
          : x(v.x),
          y(v.y),
          z(v.z),
          w(0)
      {}
      vec_t(const float4& v)
          : x(v.x),
          y(v.y),
          z(v.z),
          w(v.w)
      {}

      void Lerp(const vec_t& v, float t)
      {
         x += (v.x - x) * t;
         y += (v.y - y) * t;
         z += (v.z - z) * t;
         w += (v.w - w) * t;
      }

      void Set(float v) { x = y = z = w = v; }
      void Set(float _x, float _y, float _z = 0.f, float _w = 0.f) { x = _x; y = _y; z = _z; w = _w; }

      vec_t& operator -= (const vec_t& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
      vec_t& operator += (const vec_t& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
      vec_t& operator *= (const vec_t& v) { x *= v.x; y *= v.y; z *= v.z; w *= v.w; return *this; }
      vec_t& operator *= (float v) { x *= v;    y *= v;    z *= v;    w *= v;    return *this; }

      vec_t operator * (float f) const;
      vec_t operator - () const;
      vec_t operator - (const vec_t& v) const;
      vec_t operator + (const vec_t& v) const;
      vec_t operator * (const vec_t& v) const;

      const vec_t& operator + () const { return (*this); }
      float Length() const { return sqrtf(x * x + y * y + z * z); };
      float LengthSq() const { return (x * x + y * y + z * z); };
      vec_t Normalize() { (*this) *= (1.f / ( Length() > FLT_EPSILON ? Length() : FLT_EPSILON ) ); return (*this); }
      vec_t Normalize(const vec_t& v) { this->Set(v.x, v.y, v.z, v.w); this->Normalize(); return (*this); }
      vec_t Abs() const;

      void Cross(const vec_t& v)
      {
         vec_t res;
         res.x = y * v.z - z * v.y;
         res.y = z * v.x - x * v.z;
         res.z = x * v.y - y * v.x;

         x = res.x;
         y = res.y;
         z = res.z;
         w = 0.f;
      }

      void Cross(const vec_t& v1, const vec_t& v2)
      {
         x = v1.y * v2.z - v1.z * v2.y;
         y = v1.z * v2.x - v1.x * v2.z;
         z = v1.x * v2.y - v1.y * v2.x;
         w = 0.f;
      }

      float Dot(const vec_t& v) const
      {
         return (x * v.x) + (y * v.y) + (z * v.z) + (w * v.w);
      }

      float Dot3(const vec_t& v) const
      {
         return (x * v.x) + (y * v.y) + (z * v.z);
      }

      void Transform(const matrix_t& matrix);
      void Transform(const vec_t& s, const matrix_t& matrix);

      void TransformVector(const matrix_t& matrix);
      void TransformPoint(const matrix_t& matrix);
      void TransformVector(const vec_t& v, const matrix_t& matrix) { (*this) = v; this->TransformVector(matrix); }
      void TransformPoint(const vec_t& v, const matrix_t& matrix) { (*this) = v; this->TransformPoint(matrix); }

      float& operator [] (size_t index) { return ((float*)&x)[index]; }
      const float& operator [] (size_t index) const { return ((float*)&x)[index]; }
      bool operator!=(const vec_t& other) const { return memcmp(this, &other, sizeof(vec_t)) != 0; }
   };

   vec_t makeVect(float _x, float _y, float _z = 0.f, float _w = 0.f) { vec_t res; res.x = _x; res.y = _y; res.z = _z; res.w = _w; return res; }
   vec_t makeVect(ImVec2 v) { vec_t res; res.x = v.x; res.y = v.y; res.z = 0.f; res.w = 0.f; return res; }
   vec_t vec_t::operator * (float f) const { return makeVect(x * f, y * f, z * f, w * f); }
   vec_t vec_t::operator - () const { return makeVect(-x, -y, -z, -w); }
   vec_t vec_t::operator - (const vec_t& v) const { return makeVect(x - v.x, y - v.y, z - v.z, w - v.w); }
   vec_t vec_t::operator + (const vec_t& v) const { return makeVect(x + v.x, y + v.y, z + v.z, w + v.w); }
   vec_t vec_t::operator * (const vec_t& v) const { return makeVect(x * v.x, y * v.y, z * v.z, w * v.w); }
   vec_t vec_t::Abs() const { return makeVect(fabsf(x), fabsf(y), fabsf(z)); }

   vec_t Normalized(const vec_t& v) { vec_t res; res = v; res.Normalize(); return res; }
   vec_t Cross(const vec_t& v1, const vec_t& v2)
   {
      vec_t res;
      res.x = v1.y * v2.z - v1.z * v2.y;
      res.y = v1.z * v2.x - v1.x * v2.z;
      res.z = v1.x * v2.y - v1.y * v2.x;
      res.w = 0.f;
      return res;
   }

   float Dot(const vec_t& v1, const vec_t& v2)
   {
      return (v1.x * v2.x) + (v1.y * v2.y) + (v1.z * v2.z);
   }

   vec_t BuildPlane(const vec_t& p_point1, const vec_t& p_normal)
   {
      vec_t normal, res;
      normal.Normalize(p_normal);
      res.w = normal.Dot(p_point1);
      res.x = normal.x;
      res.y = normal.y;
      res.z = normal.z;
      return res;
   }

   struct matrix_t
   {
   public:

      union
      {
         float m[4][4];
         float m16[16];
         struct
         {
            vec_t right, up, dir, position;
         } v;
         vec_t component[4];
      };

      operator float* () { return m16; }
      operator const float* () const { return m16; }
      void Translation(float _x, float _y, float _z) { this->Translation(makeVect(_x, _y, _z)); }

      void Translation(const vec_t& vt)
      {
         v.right.Set(1.f, 0.f, 0.f, 0.f);
         v.up.Set(0.f, 1.f, 0.f, 0.f);
         v.dir.Set(0.f, 0.f, 1.f, 0.f);
         v.position.Set(vt.x, vt.y, vt.z, 1.f);
      }

      void Scale(float _x, float _y, float _z)
      {
         v.right.Set(_x, 0.f, 0.f, 0.f);
         v.up.Set(0.f, _y, 0.f, 0.f);
         v.dir.Set(0.f, 0.f, _z, 0.f);
         v.position.Set(0.f, 0.f, 0.f, 1.f);
      }
      void Scale(const vec_t& s) { Scale(s.x, s.y, s.z); }

      matrix_t& operator *= (const matrix_t& mat)
      {
         matrix_t tmpMat;
         tmpMat = *this;
         tmpMat.Multiply(mat);
         *this = tmpMat;
         return *this;
      }
      matrix_t operator * (const matrix_t& mat) const
      {
         matrix_t matT;
         matT.Multiply(*this, mat);
         return matT;
      }

      void Multiply(const matrix_t& matrix)
      {
         matrix_t tmp;
         tmp = *this;

         FPU_MatrixF_x_MatrixF((float*)&tmp, (float*)&matrix, (float*)this);
      }

      void Multiply(const matrix_t& m1, const matrix_t& m2)
      {
         FPU_MatrixF_x_MatrixF((float*)&m1, (float*)&m2, (float*)this);
      }

      float GetDeterminant() const
      {
         return m[0][0] * m[1][1] * m[2][2] + m[0][1] * m[1][2] * m[2][0] + m[0][2] * m[1][0] * m[2][1] -
            m[0][2] * m[1][1] * m[2][0] - m[0][1] * m[1][0] * m[2][2] - m[0][0] * m[1][2] * m[2][1];
      }

      float Inverse(const matrix_t& srcMatrix, bool affine = false);
      void SetToIdentity()
      {
         v.right.Set(1.f, 0.f, 0.f, 0.f);
         v.up.Set(0.f, 1.f, 0.f, 0.f);
         v.dir.Set(0.f, 0.f, 1.f, 0.f);
         v.position.Set(0.f, 0.f, 0.f, 1.f);
      }
      void Transpose()
      {
         matrix_t tmpm;
         for (int l = 0; l < 4; l++)
         {
            for (int c = 0; c < 4; c++)
            {
               tmpm.m[l][c] = m[c][l];
            }
         }
         (*this) = tmpm;
      }

      void RotationAxis(const vec_t& axis, float angle);

      void OrthoNormalize()
      {
         v.right.Normalize();
         v.up.Normalize();
         v.dir.Normalize();
      }
   };

   void vec_t::Transform(const matrix_t& matrix)
   {
      vec_t out;

      out.x = x * matrix.m[0][0] + y * matrix.m[1][0] + z * matrix.m[2][0] + w * matrix.m[3][0];
      out.y = x * matrix.m[0][1] + y * matrix.m[1][1] + z * matrix.m[2][1] + w * matrix.m[3][1];
      out.z = x * matrix.m[0][2] + y * matrix.m[1][2] + z * matrix.m[2][2] + w * matrix.m[3][2];
      out.w = x * matrix.m[0][3] + y * matrix.m[1][3] + z * matrix.m[2][3] + w * matrix.m[3][3];

      x = out.x;
      y = out.y;
      z = out.z;
      w = out.w;
   }

   void vec_t::Transform(const vec_t& s, const matrix_t& matrix)
   {
      *this = s;
      Transform(matrix);
   }

   void vec_t::TransformPoint(const matrix_t& matrix)
   {
      vec_t out;

      out.x = x * matrix.m[0][0] + y * matrix.m[1][0] + z * matrix.m[2][0] + matrix.m[3][0];
      out.y = x * matrix.m[0][1] + y * matrix.m[1][1] + z * matrix.m[2][1] + matrix.m[3][1];
      out.z = x * matrix.m[0][2] + y * matrix.m[1][2] + z * matrix.m[2][2] + matrix.m[3][2];
      out.w = x * matrix.m[0][3] + y * matrix.m[1][3] + z * matrix.m[2][3] + matrix.m[3][3];

      x = out.x;
      y = out.y;
      z = out.z;
      w = out.w;
   }

   void vec_t::TransformVector(const matrix_t& matrix)
   {
      vec_t out;

      out.x = x * matrix.m[0][0] + y * matrix.m[1][0] + z * matrix.m[2][0];
      out.y = x * matrix.m[0][1] + y * matrix.m[1][1] + z * matrix.m[2][1];
      out.z = x * matrix.m[0][2] + y * matrix.m[1][2] + z * matrix.m[2][2];
      out.w = x * matrix.m[0][3] + y * matrix.m[1][3] + z * matrix.m[2][3];

      x = out.x;
      y = out.y;
      z = out.z;
      w = out.w;
   }

   float matrix_t::Inverse(const matrix_t& srcMatrix, bool affine)
   {
      float det = 0;

      if (affine)
      {
         det = GetDeterminant();
         float s = 1 / det;
         m[0][0] = (srcMatrix.m[1][1] * srcMatrix.m[2][2] - srcMatrix.m[1][2] * srcMatrix.m[2][1]) * s;
         m[0][1] = (srcMatrix.m[2][1] * srcMatrix.m[0][2] - srcMatrix.m[2][2] * srcMatrix.m[0][1]) * s;
         m[0][2] = (srcMatrix.m[0][1] * srcMatrix.m[1][2] - srcMatrix.m[0][2] * srcMatrix.m[1][1]) * s;
         m[1][0] = (srcMatrix.m[1][2] * srcMatrix.m[2][0] - srcMatrix.m[1][0] * srcMatrix.m[2][2]) * s;
         m[1][1] = (srcMatrix.m[2][2] * srcMatrix.m[0][0] - srcMatrix.m[2][0] * srcMatrix.m[0][2]) * s;
         m[1][2] = (srcMatrix.m[0][2] * srcMatrix.m[1][0] - srcMatrix.m[0][0] * srcMatrix.m[1][2]) * s;
         m[2][0] = (srcMatrix.m[1][0] * srcMatrix.m[2][1] - srcMatrix.m[1][1] * srcMatrix.m[2][0]) * s;
         m[2][1] = (srcMatrix.m[2][0] * srcMatrix.m[0][1] - srcMatrix.m[2][1] * srcMatrix.m[0][0]) * s;
         m[2][2] = (srcMatrix.m[0][0] * srcMatrix.m[1][1] - srcMatrix.m[0][1] * srcMatrix.m[1][0]) * s;
         m[3][0] = -(m[0][0] * srcMatrix.m[3][0] + m[1][0] * srcMatrix.m[3][1] + m[2][0] * srcMatrix.m[3][2]);
         m[3][1] = -(m[0][1] * srcMatrix.m[3][0] + m[1][1] * srcMatrix.m[3][1] + m[2][1] * srcMatrix.m[3][2]);
         m[3][2] = -(m[0][2] * srcMatrix.m[3][0] + m[1][2] * srcMatrix.m[3][1] + m[2][2] * srcMatrix.m[3][2]);
      }
      else
      {
         // transpose matrix
         float src[16];
         for (int i = 0; i < 4; ++i)
         {
            src[i] = srcMatrix.m16[i * 4];
            src[i + 4] = srcMatrix.m16[i * 4 + 1];
            src[i + 8] = srcMatrix.m16[i * 4 + 2];
            src[i + 12] = srcMatrix.m16[i * 4 + 3];
         }

         // calculate pairs for first 8 elements (cofactors)
         float tmp[12]; // temp array for pairs
         tmp[0] = src[10] * src[15];
         tmp[1] = src[11] * src[14];
         tmp[2] = src[9] * src[15];
         tmp[3] = src[11] * src[13];
         tmp[4] = src[9] * src[14];
         tmp[5] = src[10] * src[13];
         tmp[6] = src[8] * src[15];
         tmp[7] = src[11] * src[12];
         tmp[8] = src[8] * src[14];
         tmp[9] = src[10] * src[12];
         tmp[10] = src[8] * src[13];
         tmp[11] = src[9] * src[12];

         // calculate first 8 elements (cofactors)
         m16[0] = (tmp[0] * src[5] + tmp[3] * src[6] + tmp[4] * src[7]) - (tmp[1] * src[5] + tmp[2] * src[6] + tmp[5] * src[7]);
         m16[1] = (tmp[1] * src[4] + tmp[6] * src[6] + tmp[9] * src[7]) - (tmp[0] * src[4] + tmp[7] * src[6] + tmp[8] * src[7]);
         m16[2] = (tmp[2] * src[4] + tmp[7] * src[5] + tmp[10] * src[7]) - (tmp[3] * src[4] + tmp[6] * src[5] + tmp[11] * src[7]);
         m16[3] = (tmp[5] * src[4] + tmp[8] * src[5] + tmp[11] * src[6]) - (tmp[4] * src[4] + tmp[9] * src[5] + tmp[10] * src[6]);
         m16[4] = (tmp[1] * src[1] + tmp[2] * src[2] + tmp[5] * src[3]) - (tmp[0] * src[1] + tmp[3] * src[2] + tmp[4] * src[3]);
         m16[5] = (tmp[0] * src[0] + tmp[7] * src[2] + tmp[8] * src[3]) - (tmp[1] * src[0] + tmp[6] * src[2] + tmp[9] * src[3]);
         m16[6] = (tmp[3] * src[0] + tmp[6] * src[1] + tmp[11] * src[3]) - (tmp[2] * src[0] + tmp[7] * src[1] + tmp[10] * src[3]);
         m16[7] = (tmp[4] * src[0] + tmp[9] * src[1] + tmp[10] * src[2]) - (tmp[5] * src[0] + tmp[8] * src[1] + tmp[11] * src[2]);

         // calculate pairs for second 8 elements (cofactors)
         tmp[0] = src[2] * src[7];
         tmp[1] = src[3] * src[6];
         tmp[2] = src[1] * src[7];
         tmp[3] = src[3] * src[5];
         tmp[4] = src[1] * src[6];
         tmp[5] = src[2] * src[5];
         tmp[6] = src[0] * src[7];
         tmp[7] = src[3] * src[4];
         tmp[8] = src[0] * src[6];
         tmp[9] = src[2] * src[4];
         tmp[10] = src[0] * src[5];
         tmp[11] = src[1] * src[4];

         // calculate second 8 elements (cofactors)
         m16[8] = (tmp[0] * src[13] + tmp[3] * src[14] + tmp[4] * src[15]) - (tmp[1] * src[13] + tmp[2] * src[14] + tmp[5] * src[15]);
         m16[9] = (tmp[1] * src[12] + tmp[6] * src[14] + tmp[9] * src[15]) - (tmp[0] * src[12] + tmp[7] * src[14] + tmp[8] * src[15]);
         m16[10] = (tmp[2] * src[12] + tmp[7] * src[13] + tmp[10] * src[15]) - (tmp[3] * src[12] + tmp[6] * src[13] + tmp[11] * src[15]);
         m16[11] = (tmp[5] * src[12] + tmp[8] * src[13] + tmp[11] * src[14]) - (tmp[4] * src[12] + tmp[9] * src[13] + tmp[10] * src[14]);
         m16[12] = (tmp[2] * src[10] + tmp[5] * src[11] + tmp[1] * src[9]) - (tmp[4] * src[11] + tmp[0] * src[9] + tmp[3] * src[10]);
         m16[13] = (tmp[8] * src[11] + tmp[0] * src[8] + tmp[7] * src[10]) - (tmp[6] * src[10] + tmp[9] * src[11] + tmp[1] * src[8]);
         m16[14] = (tmp[6] * src[9] + tmp[11] * src[11] + tmp[3] * src[8]) - (tmp[10] * src[11] + tmp[2] * src[8] + tmp[7] * src[9]);
         m16[15] = (tmp[10] * src[10] + tmp[4] * src[8] + tmp[9] * src[9]) - (tmp[8] * src[9] + tmp[11] * src[10] + tmp[5] * src[8]);

         // calculate determinant
         det = src[0] * m16[0] + src[1] * m16[1] + src[2] * m16[2] + src[3] * m16[3];

         // calculate matrix inverse
         float invdet = 1 / det;
         for (int j = 0; j < 16; ++j)
         {
            m16[j] *= invdet;
         }
      }

      return det;
   }

   void matrix_t::RotationAxis(const vec_t& axis, float angle)
   {
      float length2 = axis.LengthSq();
      if (length2 < FLT_EPSILON)
      {
         SetToIdentity();
         return;
      }

      vec_t n = axis * (1.f / sqrtf(length2));
      float s = sinf(angle);
      float c = cosf(angle);
      float k = 1.f - c;

      float xx = n.x * n.x * k + c;
      float yy = n.y * n.y * k + c;
      float zz = n.z * n.z * k + c;
      float xy = n.x * n.y * k;
      float yz = n.y * n.z * k;
      float zx = n.z * n.x * k;
      float xs = n.x * s;
      float ys = n.y * s;
      float zs = n.z * s;

      m[0][0] = xx;
      m[0][1] = xy + zs;
      m[0][2] = zx - ys;
      m[0][3] = 0.f;
      m[1][0] = xy - zs;
      m[1][1] = yy;
      m[1][2] = yz + xs;
      m[1][3] = 0.f;
      m[2][0] = zx + ys;
      m[2][1] = yz - xs;
      m[2][2] = zz;
      m[2][3] = 0.f;
      m[3][0] = 0.f;
      m[3][1] = 0.f;
      m[3][2] = 0.f;
      m[3][3] = 1.f;
   }

   /////////////////////////////////////////////////////////////////////////////////////////

   enum MOVETYPE
   {
      MT_NONE,
      MT_MOVE_X,
      MT_MOVE_Y,
      MT_MOVE_Z,
      MT_MOVE_YZ,
      MT_MOVE_ZX,
      MT_MOVE_XY,
      MT_MOVE_SCREEN,
      MT_ROTATE_X,
      MT_ROTATE_Y,
      MT_ROTATE_Z,
      MT_ROTATE_SCREEN,
      MT_SCALE_X,
      MT_SCALE_Y,
      MT_SCALE_Z,
      MT_SCALE_XYZ
   };

   static bool IsTranslateType(int type)
   {
     return type >= MT_MOVE_X && type <= MT_MOVE_SCREEN;
   }

   static bool IsRotateType(int type)
   {
     return type >= MT_ROTATE_X && type <= MT_ROTATE_SCREEN;
   }

   static bool IsScaleType(int type)
   {
     return type >= MT_SCALE_X && type <= MT_SCALE_XYZ;
   }

   // Matches MT_MOVE_AB order
   static const OPERATION TRANSLATE_PLANS[3] = { TRANSLATE_Y | TRANSLATE_Z, TRANSLATE_X | TRANSLATE_Z, TRANSLATE_X | TRANSLATE_Y };

   Style::Style()
   {
      // default values
      TranslationLineThickness        = 2.0f;
      TranslationLineThicknessHovered = 3.0f;
      TranslationLineArrowSize        = 6.0f;
      RotationLineThickness           = 2.0f;
      RotationLineThicknessHovered    = 3.0f;
      RotationOuterLineThickness      = 3.0f;
      ScaleLineThickness              = 2.0f;
      ScaleLineThicknessHovered       = 3.0f;
      ScaleLineCircleSize             = 6.0f;
      HatchedAxisLineThickness        = 6.0f;
      CenterCircleSize                = 6.0f;

      // initialize default colors
      Colors[DIRECTION_X]           = ImVec4(0.92158204, 0.036889445, 0.084376216, 1.0f);
      Colors[DIRECTION_Y]           = ImVec4(0.16202940, 0.371237785, 0.011612245, 1.0f);
      Colors[DIRECTION_Z]           = ImVec4(0.02842604, 0.230740070, 0.768151283, 1.0f);
      Colors[PLANE_X]               = ImVec4(0.92158204, 0.036889445, 0.084376216, 0.380f);
      Colors[PLANE_Y]               = ImVec4(0.16202940, 0.371237785, 0.011612245, 0.380f);
      Colors[PLANE_Z]               = ImVec4(0.02842604, 0.230740070, 0.768151283, 0.380f);
      Colors[SELECTION]             = ImVec4(1.000f, 0.500f, 0.062f, 0.541f);
      Colors[SELECTION_X]           = ImVec4(1.0f, 0.03688944f, 0.08650046f, 1.0f);
      Colors[SELECTION_Y]           = ImVec4(0.25415218f, 0.70837593f, 0.0f, 1.0f);
      Colors[SELECTION_Z]           = ImVec4(0.02518686f, 0.27467736f, 1.0f, 1.0f);
      Colors[INACTIVE]              = ImVec4(0.600f, 0.600f, 0.600f, 0.600f);
      Colors[TRANSLATION_LINE]      = ImVec4(0.666f, 0.666f, 0.666f, 0.666f);
      Colors[SCALE_LINE]            = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
      Colors[ROTATION_USING_BORDER] = ImVec4(1.000f, 0.500f, 0.062f, 1.000f);
      Colors[ROTATION_USING_FILL]   = ImVec4(1.000f, 0.500f, 0.062f, 0.500f);
      Colors[HATCHED_AXIS_LINES]    = ImVec4(0.000f, 0.000f, 0.000f, 0.500f);
      Colors[TEXT]                  = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
      Colors[TEXT_SHADOW]           = ImVec4(0.000f, 0.000f, 0.000f, 1.000f);
   }

   struct Context
   {
      Context() : mbUsing(false), mbUsingViewManipulate(false), mbEnable(true), mbUsingBounds(false)
      {
          mIDStack.push_back(INVALID_ID);
      }

      ImDrawList* mDrawList;
      Style mStyle;

      MODE mMode;
      matrix_t mModel;
      matrix_t mModelLocal; // orthonormalized model
      matrix_t mModelInverse;
      matrix_t mModelSource;
      //matrix_t mModelSourceInverse;
      matrix_t mMVP;
      matrix_t mMVPLocal; // MVP with full model matrix whereas mMVP's model matrix might only be translation in case of World space edition
      matrix_t mViewProjection;

      vec_t mModelScaleOrigin;
      vec_t mRayOrigin;
      vec_t mRayVector;

      float  mRadiusSquareCenter;
      ImVec2 mScreenSquareCenter;
      ImVec2 mScreenSquareMin;
      ImVec2 mScreenSquareMax;

      float mScreenFactor;
      vec_t mRelativeOrigin;

      bool mbUsing;
      bool mbUsingViewManipulate;
      bool mbEnable;
      bool mbMouseOver;
      bool mReversed; // reversed projection matrix

      // translation
      vec_t mTranslationPlan;
      vec_t mTranslationPlanOrigin;
      vec_t mMatrixOrigin;
      vec_t mTranslationLastDelta;

      // rotation
      vec_t mRotationVectorSource;
      float mRotationAngle;
      float mRotationAngleOrigin;
      //vec_t mWorldToLocalAxis;

      // scale
      vec_t mScale;
      vec_t mScaleValueOrigin;
      vec_t mScaleLast;
      float mSaveMousePosx;

      // save axis factor when using gizmo
      bool mBelowAxisLimit[3];
      int mAxisMask = 0;
      bool mBelowPlaneLimit[3];
      float mAxisFactor[3];

      float mAxisLimit=0.0025f;
      float mPlaneLimit=0.02f;

      // bounds stretching
      vec_t mBoundsPivot;
      vec_t mBoundsAnchor;
      vec_t mBoundsPlan;
      vec_t mBoundsLocalPivot;
      int mBoundsBestAxis;
      int mBoundsAxis[2];
      bool mbUsingBounds;
      matrix_t mBoundsMatrix;

      //
      int mCurrentOperation;

      float mX = 0.f;
      float mY = 0.f;
      float mWidth = 0.f;
      float mHeight = 0.f;
      float mXMax = 0.f;
      float mYMax = 0.f;
      float mDisplayRatio = 1.f;

      // check to not have multiple gizmo highlighted at the same time
      bool mbOverGizmoHotspot = false;

      ImGuiWindow* mAlternativeWindow = nullptr;
      ImVector<ImGuiID> mIDStack;
      ImGuiID mEditingID = INVALID_ID;
      OPERATION mOperation = OPERATION(-1);

      bool mAllowAxisFlip = true;
      float mGizmoSizeClipSpace = 0.1f;

      inline ImGuiID GetCurrentID() {return mIDStack.back();}
   };

   static Context gContext;

   static const vec_t directionUnary[3] = { makeVect(1.f, 0.f, 0.f), makeVect(0.f, 1.f, 0.f), makeVect(0.f, 0.f, 1.f) };
   static const char* translationInfoMask[] = { "X : %5.3f", "Y : %5.3f", "Z : %5.3f",
      "Y : %5.3f Z : %5.3f", "X : %5.3f Z : %5.3f", "X : %5.3f Y : %5.3f",
      "X : %5.3f Y : %5.3f Z : %5.3f" };
   static const char* scaleInfoMask[] = { "X : %5.2f", "Y : %5.2f", "Z : %5.2f", "XYZ : %5.2f" };
   static const char* rotationInfoMask[] = { "X : %5.2f deg (%5.2f rad)", "Y : %5.2f deg (%5.2f rad)", "Z : %5.2f deg (%5.2f rad)", "Screen : %5.2f deg (%5.2f rad)" };
   static const int translationInfoIndex[] = { 0,0,0, 1,0,0, 2,0,0, 1,2,0, 0,2,0, 0,1,0, 0,1,2 };
   static const float quadMin = 0.5f;
   static const float quadMax = 0.8f;
   static const float quadUV[8] = { quadMin, quadMin, quadMin, quadMax, quadMax, quadMax, quadMax, quadMin };
   static const int halfCircleSegmentCount = 64;
   static const float snapTension = 0.5f;

   /////////////////////////////////////////////////////////////////////////////////////////

   static int GetMoveType(OPERATION op, vec_t* gizmoHitProportion);
   static int GetRotateType(OPERATION op);
   static int GetScaleType(OPERATION op);

   Style& GetStyle()
   {
      return gContext.mStyle;
   }

   static ImU32 GetColorU32(int idx)
   {
      IM_ASSERT(idx < COLOR::COUNT);
      return ImGui::ColorConvertFloat4ToU32(gContext.mStyle.Colors[idx]);
   }

   static ImVec2 worldToPos(const vec_t& worldPos, const matrix_t& mat, 
       ImVec2 position = ImVec2(gContext.mX, gContext.mY), 
       ImVec2 size = ImVec2(gContext.mWidth, gContext.mHeight))
   {
      vec_t trans;
      trans.TransformPoint(worldPos, mat);
      trans *= 0.5f / trans.w;
      trans += makeVect(0.5f, 0.5f);
      trans.y = 1.f - trans.y;
      trans.x *= size.x;
      trans.y *= size.y;
      trans.x += position.x;
      trans.y += position.y;
      return ImVec2(trans.x, trans.y);
   }

   static void ComputeCameraRay(vec_t& rayOrigin, vec_t& rayDir, 
       ImVec2 position = ImVec2(gContext.mX, gContext.mY), 
       ImVec2 size = ImVec2(gContext.mWidth, gContext.mHeight))
   {
       ImGuiIO& io = ImGui::GetIO();
       const auto& camera = App::GetCamera();

       float2 pixel(io.MousePos.x, io.MousePos.y);
       float2 renderDim = float2(App::GetRenderer().GetDisplayWidth(),
           App::GetRenderer().GetDisplayHeight());
       float aspectRatio = camera.GetAspectRatio();
       float tanHalfFOV = camera.GetTanHalfFOV();
       float3 viewBasisX = camera.GetBasisX();
       float3 viewBasisY = camera.GetBasisY();
       float3 viewBasisZ = camera.GetBasisZ();

       float2 uv = (pixel + 0.5f) / renderDim;
       float2 ndc = uv * 2.0f - 1.0f;
       ndc.y = -ndc.y;
       float3 dirV = float3(ndc.x * aspectRatio * tanHalfFOV, ndc.y * tanHalfFOV, 1);
       float3 dirW = dirV.x * viewBasisX + dirV.y * viewBasisY + dirV.z * viewBasisZ;
       dirW.normalize();
       rayDir = dirW;

       rayOrigin = camera.GetPos();
   }

   static float GetSegmentLengthClipSpace(const vec_t& start, const vec_t& end, 
       const bool localCoordinates = false)
   {
      vec_t startOfSegment = start;
      const matrix_t& mvp = localCoordinates ? gContext.mMVPLocal : gContext.mMVP;
      startOfSegment.TransformPoint(mvp);
      if (fabsf(startOfSegment.w) > FLT_EPSILON) // check for axis aligned with camera direction
         startOfSegment *= 1.f / startOfSegment.w;

      vec_t endOfSegment = end;
      endOfSegment.TransformPoint(mvp);
      if (fabsf(endOfSegment.w) > FLT_EPSILON) // check for axis aligned with camera direction
         endOfSegment *= 1.f / endOfSegment.w;

      vec_t clipSpaceAxis = endOfSegment - startOfSegment;
      if (gContext.mDisplayRatio < 1.0)
         clipSpaceAxis.x *= gContext.mDisplayRatio;
      else
         clipSpaceAxis.y /= gContext.mDisplayRatio;
      float segmentLengthInClipSpace = sqrtf(clipSpaceAxis.x * clipSpaceAxis.x + 
          clipSpaceAxis.y * clipSpaceAxis.y);
      return segmentLengthInClipSpace;
   }

   static float GetParallelogram(const vec_t& ptO, const vec_t& ptA, const vec_t& ptB)
   {
      vec_t pts[] = { ptO, ptA, ptB };
      for (unsigned int i = 0; i < 3; i++)
      {
         pts[i].TransformPoint(gContext.mMVP);
         if (fabsf(pts[i].w) > FLT_EPSILON) // check for axis aligned with camera direction
            pts[i] *= 1.f / pts[i].w;
      }
      vec_t segA = pts[1] - pts[0];
      vec_t segB = pts[2] - pts[0];
      segA.y /= gContext.mDisplayRatio;
      segB.y /= gContext.mDisplayRatio;
      vec_t segAOrtho = makeVect(-segA.y, segA.x);
      segAOrtho.Normalize();
      float dt = segAOrtho.Dot3(segB);
      float surface = sqrtf(segA.x * segA.x + segA.y * segA.y) * fabsf(dt);
      return surface;
   }

   inline vec_t PointOnSegment(const vec_t& point, const vec_t& vertPos1, const vec_t& vertPos2)
   {
      vec_t c = point - vertPos1;
      vec_t V;

      V.Normalize(vertPos2 - vertPos1);
      float d = (vertPos2 - vertPos1).Length();
      float t = V.Dot3(c);

      if (t < 0.f)
         return vertPos1;

      if (t > d)
         return vertPos2;

      return vertPos1 + V * t;
   }

   static float IntersectRayPlane(const vec_t& rOrigin, const vec_t& rVector, const vec_t& plan)
   {
      const float numer = plan.Dot3(rOrigin) - plan.w;
      const float denom = plan.Dot3(rVector);

      if (fabsf(denom) < FLT_EPSILON)  // normal is orthogonal to vector, cant intersect
         return -1.0f;

      return -(numer / denom);
   }

   static bool IsInContextRect(ImVec2 p)
   {
      return IsWithin(p.x, gContext.mX, gContext.mXMax) && IsWithin(p.y, gContext.mY, gContext.mYMax);
   }

   static bool IsHoveringWindow()
   {
      ImGuiContext& g = *ImGui::GetCurrentContext();
      ImGuiWindow* window = ImGui::FindWindowByName(gContext.mDrawList->_OwnerName);
      if (g.HoveredWindow == window)   // Mouse hovering drawlist window
         return true;
      if (gContext.mAlternativeWindow != nullptr && g.HoveredWindow == gContext.mAlternativeWindow)
         return true;
      if (g.HoveredWindow != NULL)     // Any other window is hovered
         return false;
      if (ImGui::IsMouseHoveringRect(window->InnerRect.Min, window->InnerRect.Max, false))   // Hovering drawlist window rect, while no other window is hovered (for _NoInputs windows)
         return true;
      return false;
   }

   void SetRect(float x, float y, float width, float height)
   {
      gContext.mX = x;
      gContext.mY = y;
      gContext.mWidth = width;
      gContext.mHeight = height;
      gContext.mXMax = gContext.mX + gContext.mWidth;
      gContext.mYMax = gContext.mY + gContext.mXMax;
      gContext.mDisplayRatio = width / height;
   }

   void SetDrawlist(ImDrawList* drawlist)
   {
      gContext.mDrawList = drawlist ? drawlist : ImGui::GetWindowDrawList();
   }

   void SetImGuiContext(ImGuiContext* ctx)
   {
      ImGui::SetCurrentContext(ctx);
   }

   void BeginFrame()
   {
      const ImU32 flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | 
          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | 
          ImGuiWindowFlags_NoBringToFrontOnFocus;

#ifdef IMGUI_HAS_VIEWPORT
      ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
      ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
#else
      ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::SetNextWindowPos(ImVec2(0, 0));
#endif

      ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
      ImGui::PushStyleColor(ImGuiCol_Border, 0);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

      ImGui::Begin("gizmo", NULL, flags);
      gContext.mDrawList = ImGui::GetWindowDrawList();
      gContext.mbOverGizmoHotspot = false;
      ImGui::End();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
   }

   bool IsUsing()
   {
      return (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID)) || gContext.mbUsingBounds;
   }

   bool IsUsingViewManipulate()
   {
      return gContext.mbUsingViewManipulate;
   }

   bool IsUsingAny()
   {
      return gContext.mbUsing || gContext.mbUsingBounds;
   }

   bool IsOver()
   {
      return (Intersects(gContext.mOperation, TRANSLATE) && GetMoveType(gContext.mOperation, NULL) != MT_NONE) ||
         (Intersects(gContext.mOperation, ROTATE) && GetRotateType(gContext.mOperation) != MT_NONE) ||
         (Intersects(gContext.mOperation, SCALE) && GetScaleType(gContext.mOperation) != MT_NONE) || IsUsing();
   }

   bool IsOver(OPERATION op)
   {
      if(IsUsing())
         return true;
      if(Intersects(op, SCALE) && GetScaleType(op) != MT_NONE)
         return true;
      if(Intersects(op, ROTATE) && GetRotateType(op) != MT_NONE)
         return true;
      if(Intersects(op, TRANSLATE) && GetMoveType(op, NULL) != MT_NONE)
         return true;
      return false;
   }

   void Enable(bool enable)
   {
      gContext.mbEnable = enable;
      if (!enable)
      {
         gContext.mbUsing = false;
         gContext.mbUsingBounds = false;
      }
   }

   static void ComputeContext(float4x4a& matrix, MODE mode)
   {
        const auto& camera = App::GetCamera();
        auto V = camera.GetCurrView();
        // ImGuizmo doesn't appear to work with infinite far plane
        auto P = camera.GetProjNonInfiniteFarZ();
        v_float4x4 vCurrV = load4x4(const_cast<float4x4a&>(camera.GetCurrView()));
        v_float4x4 vP = load4x4(const_cast<float4x4a&>(camera.GetProj()));
        v_float4x4 vVP = mul(vCurrV, vP);
        auto VP = store(vVP);

        gContext.mMode = mode;
        gContext.mbMouseOver = IsHoveringWindow();

        memcpy(gContext.mModelLocal, &matrix, sizeof(float) * 16);
        gContext.mModelLocal.OrthoNormalize();

        if (mode == LOCAL)
        {
            gContext.mModel = gContext.mModelLocal;
            gContext.mModelInverse.Inverse(gContext.mModel);
        }
        else
        {
            float4x4a M(store(identity()));
            M.m[3] = matrix.m[3];
            memcpy(gContext.mModel, &M, sizeof(float) * 16);

            M.m[3] = float4(-matrix.m[3].x, -matrix.m[3].y, -matrix.m[3].z, 1);
            memcpy(gContext.mModelInverse, &M, sizeof(float) * 16);
        }

        memcpy(gContext.mModelSource, &matrix, sizeof(float) * 16);
        gContext.mModelScaleOrigin.Set(gContext.mModelSource.v.right.Length(), 
            gContext.mModelSource.v.up.Length(), 
            gContext.mModelSource.v.dir.Length());

        memcpy(gContext.mViewProjection, &VP, sizeof(float) * 16);
        gContext.mMVP = gContext.mModel * gContext.mViewProjection;
        gContext.mMVPLocal = gContext.mModelLocal * gContext.mViewProjection;

        gContext.mReversed = true;

        // compute scale from the size of camera right vector projected on screen at the matrix position
        vec_t pointRight = App::GetCamera().GetBasisX();
        pointRight.TransformPoint(gContext.mViewProjection);
        gContext.mScreenFactor = gContext.mGizmoSizeClipSpace / 
            (pointRight.x / pointRight.w - gContext.mMVP.v.position.x / gContext.mMVP.v.position.w);

        vec_t rightViewInverse = App::GetCamera().GetBasisX();
        rightViewInverse.TransformVector(gContext.mModelInverse);
        float rightLength = GetSegmentLengthClipSpace(makeVect(0.f, 0.f), rightViewInverse);
        gContext.mScreenFactor = gContext.mGizmoSizeClipSpace / rightLength;

        ImVec2 centerSSpace = worldToPos(makeVect(0.f, 0.f), gContext.mMVP);
        gContext.mScreenSquareCenter = centerSSpace;
        gContext.mScreenSquareMin = ImVec2(centerSSpace.x - 10.f, centerSSpace.y - 10.f);
        gContext.mScreenSquareMax = ImVec2(centerSSpace.x + 10.f, centerSSpace.y + 10.f);

        ComputeCameraRay(gContext.mRayOrigin, gContext.mRayVector);
   }

   static void ComputeColors(ImU32* colors, int type, OPERATION operation)
   {
      if (gContext.mbEnable)
      {
         ImU32 selectionColor = GetColorU32(SELECTION);

         switch (operation)
         {
         case TRANSLATE:
            colors[0] = (type == MT_MOVE_SCREEN) ? selectionColor : IM_COL32_WHITE;
            for (int i = 0; i < 3; i++)
            {
               colors[i + 1] = (type == (int)(MT_MOVE_X + i)) ? GetColorU32(SELECTION_X + i) : GetColorU32(DIRECTION_X + i);
               colors[i + 4] = (type == (int)(MT_MOVE_YZ + i)) ? selectionColor : GetColorU32(PLANE_X + i);
               colors[i + 4] = (type == MT_MOVE_SCREEN) ? selectionColor : colors[i + 4];
            }
            break;
         case ROTATE:
            colors[0] = (type == MT_ROTATE_SCREEN) ? selectionColor : IM_COL32_WHITE;
            for (int i = 0; i < 3; i++)
               colors[i + 1] = (type == (int)(MT_ROTATE_X + i)) ? GetColorU32(SELECTION_X + i) : GetColorU32(DIRECTION_X + i);
            break;
         case SCALEU:
         case SCALE:
            colors[0] = (type == MT_SCALE_XYZ) ? selectionColor : IM_COL32_WHITE;
            for (int i = 0; i < 3; i++)
               colors[i + 1] = (type == (int)(MT_SCALE_X + i)) ? GetColorU32(SELECTION_X + i) : GetColorU32(DIRECTION_X + i);
            break;
         // note: this internal function is only called with three possible values for operation
         default:
            break;
         }
      }
      else
      {
         ImU32 inactiveColor = GetColorU32(INACTIVE);
         for (int i = 0; i < 7; i++)
            colors[i] = inactiveColor;
      }
   }

   static void ComputeTripodAxisAndVisibility(const int axisIndex, vec_t& dirAxis, vec_t& dirPlaneX, 
       vec_t& dirPlaneY, bool& belowAxisLimit, bool& belowPlaneLimit, const bool localCoordinates = false)
   {
      dirAxis = directionUnary[axisIndex];
      dirPlaneX = directionUnary[(axisIndex + 1) % 3];
      dirPlaneY = directionUnary[(axisIndex + 2) % 3];

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID))
      {
         // when using, use stored factors so the gizmo doesn't flip when we translate

         // Apply axis mask to axes and planes
         belowAxisLimit = gContext.mBelowAxisLimit[axisIndex] && ((1<<axisIndex) & gContext.mAxisMask);
         belowPlaneLimit = gContext.mBelowPlaneLimit[axisIndex] && ((((1<<axisIndex) & gContext.mAxisMask) && 
             !(gContext.mAxisMask & (gContext.mAxisMask - 1))) || !gContext.mAxisMask);

         dirAxis *= gContext.mAxisFactor[axisIndex];
         dirPlaneX *= gContext.mAxisFactor[(axisIndex + 1) % 3];
         dirPlaneY *= gContext.mAxisFactor[(axisIndex + 2) % 3];
      }
      else
      {
         // new method
         float lenDir = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), dirAxis, 
             localCoordinates);
         float lenDirMinus = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), -dirAxis, 
             localCoordinates);

         float lenDirPlaneX = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), dirPlaneX, 
             localCoordinates);
         float lenDirMinusPlaneX = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), 
             -dirPlaneX, localCoordinates);

         float lenDirPlaneY = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), 
             dirPlaneY, localCoordinates);
         float lenDirMinusPlaneY = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), 
             -dirPlaneY, localCoordinates);

         // For readability
         bool & allowFlip = gContext.mAllowAxisFlip;
         float mulAxis = (allowFlip && lenDir < lenDirMinus && 
             fabsf(lenDir - lenDirMinus) > FLT_EPSILON) ? -1.f : 1.f;
         float mulAxisX = (allowFlip && lenDirPlaneX < lenDirMinusPlaneX && 
             fabsf(lenDirPlaneX - lenDirMinusPlaneX) > FLT_EPSILON) ? -1.f : 1.f;
         float mulAxisY = (allowFlip && lenDirPlaneY < lenDirMinusPlaneY && 
             fabsf(lenDirPlaneY - lenDirMinusPlaneY) > FLT_EPSILON) ? -1.f : 1.f;
         dirAxis *= mulAxis;
         dirPlaneX *= mulAxisX;
         dirPlaneY *= mulAxisY;

         // for axis
         float axisLengthInClipSpace = GetSegmentLengthClipSpace(makeVect(0.f, 0.f, 0.f), 
             dirAxis * gContext.mScreenFactor, localCoordinates);

         float paraSurf = GetParallelogram(makeVect(0.f, 0.f, 0.f), dirPlaneX * gContext.mScreenFactor, 
             dirPlaneY * gContext.mScreenFactor);
         // Apply axis mask to axes and planes
         belowPlaneLimit = (paraSurf > gContext.mAxisLimit) && ((((1<<axisIndex)&gContext.mAxisMask) && 
             !(gContext.mAxisMask & (gContext.mAxisMask - 1))) || !gContext.mAxisMask);
         belowAxisLimit = (axisLengthInClipSpace > gContext.mPlaneLimit) && !((1<<axisIndex)&gContext.mAxisMask);

         // and store values
         gContext.mAxisFactor[axisIndex] = mulAxis;
         gContext.mAxisFactor[(axisIndex + 1) % 3] = mulAxisX;
         gContext.mAxisFactor[(axisIndex + 2) % 3] = mulAxisY;
         gContext.mBelowAxisLimit[axisIndex] = belowAxisLimit;
         gContext.mBelowPlaneLimit[axisIndex] = belowPlaneLimit;
      }
   }

   static void ComputeSnap(float* value, float snap)
   {
      if (snap <= FLT_EPSILON)
         return;

      float modulo = fmodf(*value, snap);
      float moduloRatio = fabsf(modulo) / snap;
      if (moduloRatio < snapTension)
         *value -= modulo;
      else if (moduloRatio > (1.f - snapTension))
         *value = *value - modulo + snap * ((*value < 0.f) ? -1.f : 1.f);
   }
   static void ComputeSnap(vec_t& value, const float* snap)
   {
      for (int i = 0; i < 3; i++)
         ComputeSnap(&value[i], snap[i]);
   }

   static float ComputeAngleOnPlan()
   {
      const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
          gContext.mTranslationPlan);
      vec_t localPos = Normalized(gContext.mRayOrigin + gContext.mRayVector * len - 
          gContext.mModel.v.position);

      vec_t perpendicularVector;
      perpendicularVector.Cross(gContext.mRotationVectorSource, gContext.mTranslationPlan);
      perpendicularVector.Normalize();
      float acosAngle = Clamp(Dot(localPos, gContext.mRotationVectorSource), -1.f, 1.f);
      float angle = acosf(acosAngle);
      angle *= (Dot(localPos, perpendicularVector) < 0.f) ? 1.f : -1.f;
      return angle;
   }

   static void DrawRotationGizmo(OPERATION op, int type)
   {
      if(!Intersects(op, ROTATE))
         return;
      ImDrawList* drawList = gContext.mDrawList;

      bool isMultipleAxesMasked = gContext.mAxisMask & (gContext.mAxisMask - 1);
      bool isNoAxesMasked = !gContext.mAxisMask;

      // colors
      ImU32 colors[7];
      ComputeColors(colors, type, ROTATE);

      vec_t cameraToModelNormalized = Normalized(gContext.mModel.v.position - App::GetCamera().GetPos());
      cameraToModelNormalized.TransformVector(gContext.mModelInverse);

      gContext.mRadiusSquareCenter = screenRotateSize * gContext.mHeight;

      bool hasRSC = Intersects(op, ROTATE_SCREEN);
      for (int axis = 0; axis < 3; axis++)
      {
         if(!Intersects(op, static_cast<OPERATION>(ROTATE_Z >> axis)))
            continue;

         bool isAxisMasked = (1 << (2 - axis)) & gContext.mAxisMask;

         if ((!isAxisMasked || isMultipleAxesMasked) && !isNoAxesMasked)
            continue;
         const bool usingAxis = (gContext.mbUsing && type == MT_ROTATE_Z - axis);
         const int circleMul = (hasRSC && !usingAxis) ? 1 : 2;

         //ImVec2* circlePos = (ImVec2*)alloca(sizeof(ImVec2) * (circleMul * halfCircleSegmentCount + 1));
         SmallVector<ImVec2, App::FrameAllocator> circlePosVec;
         circlePosVec.resize(circleMul * halfCircleSegmentCount + 1);
         ImVec2* circlePos = circlePosVec.data();

         float angleStart = atan2f(cameraToModelNormalized[(4 - axis) % 3], 
             cameraToModelNormalized[(3 - axis) % 3]) + ZPI * 0.5f;

         for (int i = 0; i < circleMul * halfCircleSegmentCount + 1; i++)
         {
            float ng = angleStart + (float)circleMul * ZPI * ((float)i / (float)(circleMul * halfCircleSegmentCount));
            vec_t axisPos = makeVect(cosf(ng), sinf(ng), 0.f);
            vec_t pos = makeVect(axisPos[axis], axisPos[(axis + 1) % 3], 
                axisPos[(axis + 2) % 3]) * gContext.mScreenFactor * rotationDisplayFactor;
            circlePos[i] = worldToPos(pos, gContext.mMVP);
         }

         if (!gContext.mbUsing || usingAxis)
         {
             const float thickness = type == (int)MT_ROTATE_Z - axis ?
                 gContext.mStyle.RotationLineThicknessHovered :
                 gContext.mStyle.RotationLineThickness;

             drawList->AddPolyline(circlePos, circleMul* halfCircleSegmentCount + 1,
                colors[3 - axis], false, thickness);
         }

         float radiusAxis = sqrtf((ImLengthSqr(worldToPos(gContext.mModel.v.position, 
             gContext.mViewProjection) - circlePos[0])));
         if (radiusAxis > gContext.mRadiusSquareCenter)
            gContext.mRadiusSquareCenter = radiusAxis;
      }
      if (hasRSC && (!gContext.mbUsing || type == MT_ROTATE_SCREEN) && (!isMultipleAxesMasked && isNoAxesMasked))
      {
         drawList->AddCircle(worldToPos(gContext.mModel.v.position, gContext.mViewProjection), 
             gContext.mRadiusSquareCenter, colors[0], 64, gContext.mStyle.RotationOuterLineThickness);
      }

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && IsRotateType(type))
      {
         ImVec2 circlePos[halfCircleSegmentCount + 1];

         circlePos[0] = worldToPos(gContext.mModel.v.position, gContext.mViewProjection);
         for (unsigned int i = 1; i < halfCircleSegmentCount + 1; i++)
         {
            float ng = gContext.mRotationAngle * ((float)(i - 1) / (float)(halfCircleSegmentCount - 1));
            matrix_t rotateVectorMatrix;
            rotateVectorMatrix.RotationAxis(gContext.mTranslationPlan, ng);
            vec_t pos;
            pos.TransformPoint(gContext.mRotationVectorSource, rotateVectorMatrix);
            pos *= gContext.mScreenFactor * rotationDisplayFactor;
            circlePos[i] = worldToPos(pos + gContext.mModel.v.position, gContext.mViewProjection);
         }

         const COLOR colorBorder = type == (int)MT_ROTATE_X ? SELECTION_X :
             (type == (int)MT_ROTATE_Y ? SELECTION_Y : SELECTION_Z);
         drawList->AddConvexPolyFilled(circlePos, halfCircleSegmentCount + 1, 
             GetColorU32(ROTATION_USING_FILL));
         drawList->AddPolyline(circlePos, halfCircleSegmentCount + 1, GetColorU32(colorBorder),
             true, gContext.mStyle.RotationLineThickness);

         ImVec2 destinationPosOnScreen = circlePos[1];
         char tmps[512];
         ImFormatString(tmps, sizeof(tmps), rotationInfoMask[type - MT_ROTATE_X], 
             (gContext.mRotationAngle / ZPI) * 180.f, gContext.mRotationAngle);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 15, destinationPosOnScreen.y + 15), 
             GetColorU32(TEXT_SHADOW), tmps);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 14, destinationPosOnScreen.y + 14), 
             GetColorU32(TEXT), tmps);
      }
   }

   static void DrawHatchedAxis(const vec_t& axis)
   {
      if (gContext.mStyle.HatchedAxisLineThickness <= 0.0f)
         return;

      for (int j = 1; j < 10; j++)
      {
         ImVec2 baseSSpace2 = worldToPos(axis * 0.05f * (float)(j * 2) * gContext.mScreenFactor, 
             gContext.mMVP);
         ImVec2 worldDirSSpace2 = worldToPos(axis * 0.05f * (float)(j * 2 + 1) * gContext.mScreenFactor, 
             gContext.mMVP);
         gContext.mDrawList->AddLine(baseSSpace2, worldDirSSpace2, GetColorU32(HATCHED_AXIS_LINES), 
             gContext.mStyle.HatchedAxisLineThickness);
      }
   }

   static void DrawScaleGizmo(OPERATION op, int type)
   {
      ImDrawList* drawList = gContext.mDrawList;

      if(!Intersects(op, SCALE))
        return;

      // colors
      ImU32 colors[7];
      ComputeColors(colors, type, SCALE);

      // draw
      vec_t scaleDisplay = { 1.f, 1.f, 1.f, 1.f };

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID))
         scaleDisplay = gContext.mScale;

      for (int i = 0; i < 3; i++)
      {
         if(!Intersects(op, static_cast<OPERATION>(SCALE_X << i)))
            continue;
         const bool usingAxis = (gContext.mbUsing && type == MT_SCALE_X + i);
         if (!gContext.mbUsing || usingAxis)
         {
            vec_t dirPlaneX, dirPlaneY, dirAxis;
            bool belowAxisLimit, belowPlaneLimit;
            ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, 
                belowAxisLimit, belowPlaneLimit, true);

            // draw axis
            if (belowAxisLimit)
            {
               bool hasTranslateOnAxis = Contains(op, static_cast<OPERATION>(TRANSLATE_X << i));
               float markerScale = hasTranslateOnAxis ? 1.4f : 1.0f;
               ImVec2 baseSSpace = worldToPos(dirAxis * 0.1f * gContext.mScreenFactor, 
                   gContext.mMVP);
               ImVec2 worldDirSSpaceNoScale = worldToPos(dirAxis * markerScale * gContext.mScreenFactor, 
                   gContext.mMVP);
               ImVec2 worldDirSSpace = worldToPos((dirAxis * markerScale * scaleDisplay[i]) * gContext.mScreenFactor, 
                   gContext.mMVP);

               if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID))
               {
                  ImU32 scaleLineColor = GetColorU32(SCALE_LINE);
                  drawList->AddLine(baseSSpace, worldDirSSpaceNoScale, scaleLineColor, 
                      gContext.mStyle.ScaleLineThickness);
                  drawList->AddCircleFilled(worldDirSSpaceNoScale, gContext.mStyle.ScaleLineCircleSize, 
                      scaleLineColor);
               }

               if (!hasTranslateOnAxis || gContext.mbUsing)
               {
                    const float thickness = type == (int)MT_SCALE_X + i ?
                        gContext.mStyle.ScaleLineThicknessHovered :
                        gContext.mStyle.ScaleLineThickness;

                    drawList->AddLine(baseSSpace, worldDirSSpace, colors[i + 1], 
                        thickness);
               }
               drawList->AddCircleFilled(worldDirSSpace, gContext.mStyle.ScaleLineCircleSize, 
                   colors[i + 1]);

               if (gContext.mAxisFactor[i] < 0.f)
                  DrawHatchedAxis(dirAxis * scaleDisplay[i]);
            }
         }
      }

      // draw screen cirle
      drawList->AddCircleFilled(gContext.mScreenSquareCenter, gContext.mStyle.CenterCircleSize, 
          colors[0], 32);

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && IsScaleType(type))
      {
         //ImVec2 sourcePosOnScreen = worldToPos(gContext.mMatrixOrigin, gContext.mViewProjection);
         ImVec2 destinationPosOnScreen = worldToPos(gContext.mModel.v.position, gContext.mViewProjection);
         char tmps[512];
         //vec_t deltaInfo = gContext.mModel.v.position - gContext.mMatrixOrigin;
         int componentInfoIndex = (type - MT_SCALE_X) * 3;
         ImFormatString(tmps, sizeof(tmps), scaleInfoMask[type - MT_SCALE_X], 
             scaleDisplay[translationInfoIndex[componentInfoIndex]]);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 15, destinationPosOnScreen.y + 15), 
             GetColorU32(TEXT_SHADOW), tmps);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 14, destinationPosOnScreen.y + 14), 
             GetColorU32(TEXT), tmps);
      }
   }

   static void DrawScaleUniveralGizmo(OPERATION op, int type)
   {
      ImDrawList* drawList = gContext.mDrawList;

      if (!Intersects(op, SCALEU))
         return;

      // colors
      ImU32 colors[7];
      ComputeColors(colors, type, SCALEU);

      // draw
      vec_t scaleDisplay = { 1.f, 1.f, 1.f, 1.f };

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID))
         scaleDisplay = gContext.mScale;

      for (int i = 0; i < 3; i++)
      {
         if (!Intersects(op, static_cast<OPERATION>(SCALE_XU << i)))
            continue;

         const bool usingAxis = (gContext.mbUsing && type == MT_SCALE_X + i);
         if (!gContext.mbUsing || usingAxis)
         {
            vec_t dirPlaneX, dirPlaneY, dirAxis;
            bool belowAxisLimit, belowPlaneLimit;
            ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, belowAxisLimit, 
                belowPlaneLimit, true);

            // draw axis
            if (belowAxisLimit)
            {
               bool hasTranslateOnAxis = Contains(op, static_cast<OPERATION>(TRANSLATE_X << i));
               float markerScale = hasTranslateOnAxis ? 1.4f : 1.0f;
               //ImVec2 baseSSpace = worldToPos(dirAxis * 0.1f * gContext.mScreenFactor, gContext.mMVPLocal);
               //ImVec2 worldDirSSpaceNoScale = worldToPos(dirAxis * markerScale * gContext.mScreenFactor, gContext.mMVP);
               ImVec2 worldDirSSpace = worldToPos((dirAxis * markerScale * scaleDisplay[i]) * 
                   gContext.mScreenFactor, gContext.mMVPLocal);

               drawList->AddCircleFilled(worldDirSSpace, 12.f, colors[i + 1]);
            }
         }
      }

      // draw screen cirle
      drawList->AddCircle(gContext.mScreenSquareCenter, 20.f, colors[0], 32, 
          gContext.mStyle.CenterCircleSize);

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && IsScaleType(type))
      {
         //ImVec2 sourcePosOnScreen = worldToPos(gContext.mMatrixOrigin, gContext.mViewProjection);
         ImVec2 destinationPosOnScreen = worldToPos(gContext.mModel.v.position, gContext.mViewProjection);

         char tmps[512];
         //vec_t deltaInfo = gContext.mModel.v.position - gContext.mMatrixOrigin;
         int componentInfoIndex = (type - MT_SCALE_X) * 3;
         ImFormatString(tmps, sizeof(tmps), scaleInfoMask[type - MT_SCALE_X], 
             scaleDisplay[translationInfoIndex[componentInfoIndex]]);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 15, destinationPosOnScreen.y + 15), 
             GetColorU32(TEXT_SHADOW), tmps);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 14, destinationPosOnScreen.y + 14), 
             GetColorU32(TEXT), tmps);
      }
   }

   static void DrawTranslationGizmo(OPERATION op, int type)
   {
      ImDrawList* drawList = gContext.mDrawList;
      if (!drawList)
         return;

      if(!Intersects(op, TRANSLATE))
         return;

      // colors
      ImU32 colors[7];
      ComputeColors(colors, type, TRANSLATE);

      const ImVec2 origin = worldToPos(gContext.mModel.v.position, gContext.mViewProjection);

      // draw
      bool belowAxisLimit = false;
      bool belowPlaneLimit = false;
      for (int i = 0; i < 3; ++i)
      {
         vec_t dirPlaneX, dirPlaneY, dirAxis;
         ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, 
             belowAxisLimit, belowPlaneLimit);

         if (!gContext.mbUsing || (gContext.mbUsing && type == MT_MOVE_X + i))
         {
             const float thickness = type == (int)MT_MOVE_X + i ?
                 gContext.mStyle.TranslationLineThicknessHovered :
                 gContext.mStyle.TranslationLineThickness;

            // draw axis
            if (belowAxisLimit && Intersects(op, static_cast<OPERATION>(TRANSLATE_X << i)))
            {
               ImVec2 baseSSpace = worldToPos(dirAxis * 0.1f * gContext.mScreenFactor, gContext.mMVP);
               ImVec2 worldDirSSpace = worldToPos(dirAxis * gContext.mScreenFactor, gContext.mMVP);

               drawList->AddLine(baseSSpace, worldDirSSpace, colors[i + 1], thickness);

               // Arrow head begin
               ImVec2 dir(origin - worldDirSSpace);

               float d = sqrtf(ImLengthSqr(dir));
               dir /= d; // Normalize
               dir *= gContext.mStyle.TranslationLineArrowSize;

               ImVec2 ortogonalDir(dir.y, -dir.x); // Perpendicular vector
               ImVec2 a(worldDirSSpace + dir);
               drawList->AddTriangleFilled(worldDirSSpace - dir, a + ortogonalDir, 
                   a - ortogonalDir, colors[i + 1]);
               // Arrow head end

               if (gContext.mAxisFactor[i] < 0.f)
                  DrawHatchedAxis(dirAxis);
            }
         }
         // draw plane
         if (!gContext.mbUsing || (gContext.mbUsing && type == MT_MOVE_YZ + i))
         {
            if (belowPlaneLimit && Contains(op, TRANSLATE_PLANS[i]))
            {
               ImVec2 screenQuadPts[4];
               for (int j = 0; j < 4; ++j)
               {
                  vec_t cornerWorldPos = (dirPlaneX * quadUV[j * 2] + dirPlaneY * 
                      quadUV[j * 2 + 1]) * gContext.mScreenFactor;
                  screenQuadPts[j] = worldToPos(cornerWorldPos, gContext.mMVP);
               }
               drawList->AddPolyline(screenQuadPts, 4, GetColorU32(DIRECTION_X + i), true, 1.0f);
               drawList->AddConvexPolyFilled(screenQuadPts, 4, colors[i + 4]);
            }
         }
      }

      drawList->AddCircleFilled(gContext.mScreenSquareCenter, gContext.mStyle.CenterCircleSize, 
          colors[0], 32);

      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && 
          IsTranslateType(type))
      {
         ImU32 translationLineColor = GetColorU32(TRANSLATION_LINE);

         ImVec2 sourcePosOnScreen = worldToPos(gContext.mMatrixOrigin, gContext.mViewProjection);
         ImVec2 destinationPosOnScreen = worldToPos(gContext.mModel.v.position, gContext.mViewProjection);
         vec_t dif = { destinationPosOnScreen.x - sourcePosOnScreen.x, 
             destinationPosOnScreen.y - sourcePosOnScreen.y, 0.f, 0.f };
         dif.Normalize();
         dif *= 5.f;
         drawList->AddCircle(sourcePosOnScreen, 6.f, translationLineColor);
         drawList->AddCircle(destinationPosOnScreen, 6.f, translationLineColor);
         drawList->AddLine(ImVec2(sourcePosOnScreen.x + dif.x, sourcePosOnScreen.y + dif.y), 
             ImVec2(destinationPosOnScreen.x - dif.x, destinationPosOnScreen.y - dif.y), 
             translationLineColor, 2.f);

         char tmps[512];
         vec_t deltaInfo = gContext.mModel.v.position - gContext.mMatrixOrigin;
         int componentInfoIndex = (type - MT_MOVE_X) * 3;
         ImFormatString(tmps, sizeof(tmps), translationInfoMask[type - MT_MOVE_X], 
             deltaInfo[translationInfoIndex[componentInfoIndex]], 
             deltaInfo[translationInfoIndex[componentInfoIndex + 1]], 
             deltaInfo[translationInfoIndex[componentInfoIndex + 2]]);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 15, destinationPosOnScreen.y + 15), 
             GetColorU32(TEXT_SHADOW), tmps);
         drawList->AddText(ImVec2(destinationPosOnScreen.x + 14, destinationPosOnScreen.y + 14), 
             GetColorU32(TEXT), tmps);
      }
   }

   static bool CanActivate()
   {
      if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
         return true;
      return false;
   }

   ///////////////////////////////////////////////////////////////////////////////////////

   static int GetScaleType(OPERATION op)
   {
      if (gContext.mbUsing)
         return MT_NONE;

      ImGuiIO& io = ImGui::GetIO();
      int type = MT_NONE;

      // screen
      if (io.MousePos.x >= gContext.mScreenSquareMin.x && io.MousePos.x <= gContext.mScreenSquareMax.x &&
         io.MousePos.y >= gContext.mScreenSquareMin.y && io.MousePos.y <= gContext.mScreenSquareMax.y &&
         Contains(op, SCALE))
      {
         type = MT_SCALE_XYZ;
      }

      // compute
      for (int i = 0; i < 3 && type == MT_NONE; i++)
      {
         if(!Intersects(op, static_cast<OPERATION>(SCALE_X << i)))
            continue;

         bool isAxisMasked = (1 << i) & gContext.mAxisMask;

         vec_t dirPlaneX, dirPlaneY, dirAxis;
         bool belowAxisLimit, belowPlaneLimit;
         ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, belowAxisLimit, belowPlaneLimit, true);
         dirAxis.TransformVector(gContext.mModelLocal);
         dirPlaneX.TransformVector(gContext.mModelLocal);
         dirPlaneY.TransformVector(gContext.mModelLocal);

         const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
             BuildPlane(gContext.mModelLocal.v.position, dirAxis));
         vec_t posOnPlan = gContext.mRayOrigin + gContext.mRayVector * len;

         const float startOffset = Contains(op, static_cast<OPERATION>(TRANSLATE_X << i)) ? 1.0f : 0.1f;
         const float endOffset = Contains(op, static_cast<OPERATION>(TRANSLATE_X << i)) ? 1.4f : 1.0f;
         const ImVec2 posOnPlanScreen = worldToPos(posOnPlan, gContext.mViewProjection);
         const ImVec2 axisStartOnScreen = worldToPos(gContext.mModelLocal.v.position + 
             dirAxis * gContext.mScreenFactor * startOffset, gContext.mViewProjection);
         const ImVec2 axisEndOnScreen = worldToPos(gContext.mModelLocal.v.position + 
             dirAxis * gContext.mScreenFactor * endOffset, gContext.mViewProjection);

         vec_t closestPointOnAxis = PointOnSegment(makeVect(posOnPlanScreen), 
             makeVect(axisStartOnScreen), makeVect(axisEndOnScreen));

         if ((closestPointOnAxis - makeVect(posOnPlanScreen)).Length() < 12.f) // pixel size
         {
            if (!isAxisMasked)
               type = MT_SCALE_X + i;
         }
      }

      // universal

      vec_t deltaScreen = { io.MousePos.x - gContext.mScreenSquareCenter.x,
          io.MousePos.y - gContext.mScreenSquareCenter.y, 0.f, 0.f };
      float dist = deltaScreen.Length();
      if (Contains(op, SCALEU) && dist >= 17.0f && dist < 23.0f)
         type = MT_SCALE_XYZ;

      for (int i = 0; i < 3 && type == MT_NONE; i++)
      {
         if (!Intersects(op, static_cast<OPERATION>(SCALE_XU << i)))
            continue;

         vec_t dirPlaneX, dirPlaneY, dirAxis;
         bool belowAxisLimit, belowPlaneLimit;
         ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, belowAxisLimit, 
             belowPlaneLimit, true);

         // draw axis
         if (belowAxisLimit)
         {
            bool hasTranslateOnAxis = Contains(op, static_cast<OPERATION>(TRANSLATE_X << i));
            float markerScale = hasTranslateOnAxis ? 1.4f : 1.0f;
            //ImVec2 baseSSpace = worldToPos(dirAxis * 0.1f * gContext.mScreenFactor, gContext.mMVPLocal);
            //ImVec2 worldDirSSpaceNoScale = worldToPos(dirAxis * markerScale * gContext.mScreenFactor, gContext.mMVP);
            ImVec2 worldDirSSpace = worldToPos((dirAxis * markerScale) * gContext.mScreenFactor, gContext.mMVPLocal);

            float distance = sqrtf(ImLengthSqr(worldDirSSpace - io.MousePos));
            if (distance < 12.f)
               type = MT_SCALE_X + i;
         }
      }
      return type;
   }

   static int GetRotateType(OPERATION op)
   {
      if (gContext.mbUsing)
         return MT_NONE;

      bool isNoAxesMasked = !gContext.mAxisMask;
      bool isMultipleAxesMasked = gContext.mAxisMask & (gContext.mAxisMask - 1);

      ImGuiIO& io = ImGui::GetIO();
      int type = MT_NONE;

      vec_t deltaScreen = { io.MousePos.x - gContext.mScreenSquareCenter.x, 
          io.MousePos.y - gContext.mScreenSquareCenter.y, 0.f, 0.f };
      float dist = deltaScreen.Length();
      if (Intersects(op, ROTATE_SCREEN) && dist >= (gContext.mRadiusSquareCenter - 4.0f) && 
          dist < (gContext.mRadiusSquareCenter + 4.0f))
      {
         if (!isNoAxesMasked)
            return MT_NONE;
         type = MT_ROTATE_SCREEN;
      }

      const vec_t planNormals[] = { gContext.mModel.v.right, gContext.mModel.v.up, 
          gContext.mModel.v.dir };

      auto V = App::GetCamera().GetCurrView();
         matrix_t viewMat;
         memcpy(&viewMat, &V, sizeof(float) * 16);

      vec_t modelViewPos;
      modelViewPos.TransformPoint(gContext.mModel.v.position, viewMat);

      for (int i = 0; i < 3 && type == MT_NONE; i++)
      {
         if(!Intersects(op, static_cast<OPERATION>(ROTATE_X << i)))
            continue;
         bool isAxisMasked = (1 << i) & gContext.mAxisMask;
         // pickup plan
         vec_t pickupPlan = BuildPlane(gContext.mModel.v.position, planNormals[i]);

         const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, pickupPlan);
         const vec_t intersectWorldPos = gContext.mRayOrigin + gContext.mRayVector * len;
         vec_t intersectViewPos;
         intersectViewPos.TransformPoint(intersectWorldPos, viewMat);

         if (ImAbs(modelViewPos.z) - ImAbs(intersectViewPos.z) < -FLT_EPSILON)
            continue;

         const vec_t localPos = intersectWorldPos - gContext.mModel.v.position;
         vec_t idealPosOnCircle = Normalized(localPos);
         idealPosOnCircle.TransformVector(gContext.mModelInverse);
         const ImVec2 idealPosOnCircleScreen = worldToPos(idealPosOnCircle * rotationDisplayFactor * 
             gContext.mScreenFactor, gContext.mMVP);

         //gContext.mDrawList->AddCircle(idealPosOnCircleScreen, 5.f, IM_COL32_WHITE);
         const ImVec2 distanceOnScreen = idealPosOnCircleScreen - io.MousePos;

         const float distance = makeVect(distanceOnScreen).Length();
         if (distance < 8.f) // pixel size
         {
            if ((!isAxisMasked || isMultipleAxesMasked) && !isNoAxesMasked)
               break;
            type = MT_ROTATE_X + i;
         }
      }

      return type;
   }

   static int GetMoveType(OPERATION op, vec_t* gizmoHitProportion)
   {
      if(!Intersects(op, TRANSLATE) || gContext.mbUsing || !gContext.mbMouseOver)
        return MT_NONE;

      bool isNoAxesMasked = !gContext.mAxisMask;
      bool isMultipleAxesMasked = gContext.mAxisMask & (gContext.mAxisMask - 1);

      ImGuiIO& io = ImGui::GetIO();
      int type = MT_NONE;

      // screen
      if (io.MousePos.x >= gContext.mScreenSquareMin.x && io.MousePos.x <= gContext.mScreenSquareMax.x &&
         io.MousePos.y >= gContext.mScreenSquareMin.y && io.MousePos.y <= gContext.mScreenSquareMax.y &&
         Contains(op, TRANSLATE))
      {
         type = MT_MOVE_SCREEN;
      }

      const vec_t screenCoord = makeVect(io.MousePos - ImVec2(gContext.mX, gContext.mY));

      // compute
      for (int i = 0; i < 3 && type == MT_NONE; i++)
      {
         bool isAxisMasked = (1 << i) & gContext.mAxisMask;
         vec_t dirPlaneX, dirPlaneY, dirAxis;
         bool belowAxisLimit, belowPlaneLimit;
         ComputeTripodAxisAndVisibility(i, dirAxis, dirPlaneX, dirPlaneY, belowAxisLimit, belowPlaneLimit);
         dirAxis.TransformVector(gContext.mModel);
         dirPlaneX.TransformVector(gContext.mModel);
         dirPlaneY.TransformVector(gContext.mModel);

         const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
             BuildPlane(gContext.mModel.v.position, dirAxis));
         vec_t posOnPlan = gContext.mRayOrigin + gContext.mRayVector * len;

         const ImVec2 axisStartOnScreen = worldToPos(gContext.mModel.v.position + dirAxis * 
             gContext.mScreenFactor * 0.1f, gContext.mViewProjection) - ImVec2(gContext.mX, gContext.mY);
         const ImVec2 axisEndOnScreen = worldToPos(gContext.mModel.v.position + dirAxis * 
             gContext.mScreenFactor, gContext.mViewProjection) - ImVec2(gContext.mX, gContext.mY);

         vec_t closestPointOnAxis = PointOnSegment(screenCoord, makeVect(axisStartOnScreen), 
             makeVect(axisEndOnScreen));
         if ((closestPointOnAxis - screenCoord).Length() < 12.f && 
             Intersects(op, static_cast<OPERATION>(TRANSLATE_X << i))) // pixel size
         {
            if (isAxisMasked)
               break;
            type = MT_MOVE_X + i;
         }

         const float dx = dirPlaneX.Dot3((posOnPlan - gContext.mModel.v.position) * 
             (1.f / gContext.mScreenFactor));
         const float dy = dirPlaneY.Dot3((posOnPlan - gContext.mModel.v.position) * 
             (1.f / gContext.mScreenFactor));
         if (belowPlaneLimit && dx >= quadUV[0] && dx <= quadUV[4] && dy >= quadUV[1] && dy <= quadUV[3] && 
             Contains(op, TRANSLATE_PLANS[i]))
         {
            if ((!isAxisMasked || isMultipleAxesMasked) && !isNoAxesMasked)
               break;
            type = MT_MOVE_YZ + i;
         }

         if (gizmoHitProportion)
            *gizmoHitProportion = makeVect(dx, dy, 0.f);
      }
      return type;
   }

   static bool HandleTranslation(float* matrix, float3& dt, OPERATION op, int& type, 
       const float* snap)
   {
      if(!Intersects(op, TRANSLATE) || type != MT_NONE)
        return false;

      const ImGuiIO& io = ImGui::GetIO();
      const bool applyRotationLocaly = gContext.mMode == LOCAL || type == MT_MOVE_SCREEN;
      bool modified = false;

      // move
      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && 
          IsTranslateType(gContext.mCurrentOperation))
      {
         ImGui::SetNextFrameWantCaptureMouse(true);

         const float signedLength = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
             gContext.mTranslationPlan);
         const float len = fabsf(signedLength); // near plane
         const vec_t newPos = gContext.mRayOrigin + gContext.mRayVector * len;

         // compute delta
         const vec_t newOrigin = newPos - gContext.mRelativeOrigin * gContext.mScreenFactor;
         vec_t delta = newOrigin - gContext.mModel.v.position;

         // 1 axis constraint
         if (gContext.mCurrentOperation >= MT_MOVE_X && gContext.mCurrentOperation <= MT_MOVE_Z)
         {
            const int axisIndex = gContext.mCurrentOperation - MT_MOVE_X;
            const vec_t& axisValue = *(vec_t*)&gContext.mModel.m[axisIndex];
            const float lengthOnAxis = Dot(axisValue, delta);
            delta = axisValue * lengthOnAxis;
         }

         // snap
         if (snap)
         {
            vec_t cumulativeDelta = gContext.mModel.v.position + delta - gContext.mMatrixOrigin;
            if (applyRotationLocaly)
            {
               matrix_t modelSourceNormalized = gContext.mModelSource;
               modelSourceNormalized.OrthoNormalize();
               matrix_t modelSourceNormalizedInverse;
               modelSourceNormalizedInverse.Inverse(modelSourceNormalized);
               cumulativeDelta.TransformVector(modelSourceNormalizedInverse);
               ComputeSnap(cumulativeDelta, snap);
               cumulativeDelta.TransformVector(modelSourceNormalized);
            }
            else
               ComputeSnap(cumulativeDelta, snap);

            delta = gContext.mMatrixOrigin + cumulativeDelta - gContext.mModel.v.position;
         }

         if (delta != gContext.mTranslationLastDelta)
            modified = true;

         gContext.mTranslationLastDelta = delta;

         // compute matrix & delta
         matrix_t deltaMatrixTranslation;
         deltaMatrixTranslation.Translation(delta);
         dt = float3(delta.x, delta.y, delta.z);

         const matrix_t res = gContext.mModelSource * deltaMatrixTranslation;
         *(matrix_t*)matrix = res;

         if (!io.MouseDown[0])
            gContext.mbUsing = false;

         type = gContext.mCurrentOperation;
      }
      else
      {
         // find new possible way to move
         vec_t gizmoHitProportion;
         type = gContext.mbOverGizmoHotspot ? MT_NONE : GetMoveType(op, &gizmoHitProportion);
         gContext.mbOverGizmoHotspot |= type != MT_NONE;
         if (type != MT_NONE)
            ImGui::SetNextFrameWantCaptureMouse(true);

         if (CanActivate() && type != MT_NONE)
         {
            gContext.mbUsing = true;
            gContext.mEditingID = gContext.GetCurrentID();
            gContext.mCurrentOperation = type;
            vec_t movePlanNormal[] = { gContext.mModel.v.right, gContext.mModel.v.up, gContext.mModel.v.dir,
               gContext.mModel.v.right, gContext.mModel.v.up, gContext.mModel.v.dir,
               -App::GetCamera().GetBasisZ() };

            vec_t cameraToModelNormalized = Normalized(gContext.mModel.v.position - vec_t(App::GetCamera().GetPos()));
            for (unsigned int i = 0; i < 3; i++)
            {
               vec_t orthoVector = Cross(movePlanNormal[i], cameraToModelNormalized);
               movePlanNormal[i].Cross(orthoVector);
               movePlanNormal[i].Normalize();
            }
            // pickup plan
            gContext.mTranslationPlan = BuildPlane(gContext.mModel.v.position, movePlanNormal[type - MT_MOVE_X]);
            const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
                gContext.mTranslationPlan);
            gContext.mTranslationPlanOrigin = gContext.mRayOrigin + gContext.mRayVector * len;
            gContext.mMatrixOrigin = gContext.mModel.v.position;

            gContext.mRelativeOrigin = (gContext.mTranslationPlanOrigin - gContext.mModel.v.position) * 
                (1.f / gContext.mScreenFactor);
         }
      }
      return modified;
   }

   static bool HandleScale(float* matrix, float3& ds, OPERATION op, int& type, 
       const float* snap)
   {
      if((!Intersects(op, SCALE) && !Intersects(op, SCALEU)) || type != MT_NONE || !gContext.mbMouseOver)
         return false;

      ImGuiIO& io = ImGui::GetIO();
      bool modified = false;

      if (!gContext.mbUsing)
      {
         // find new possible way to scale
         type = gContext.mbOverGizmoHotspot ? MT_NONE : GetScaleType(op);
         gContext.mbOverGizmoHotspot |= type != MT_NONE;

         if (type != MT_NONE)
            ImGui::SetNextFrameWantCaptureMouse(true);

         if (CanActivate() && type != MT_NONE)
         {
            gContext.mbUsing = true;
            gContext.mEditingID = gContext.GetCurrentID();
            gContext.mCurrentOperation = type;
            const vec_t movePlanNormal[] = { gContext.mModelLocal.v.up, gContext.mModelLocal.v.dir, 
                gContext.mModelLocal.v.right, gContext.mModelLocal.v.dir, gContext.mModelLocal.v.up, 
                gContext.mModelLocal.v.right, -App::GetCamera().GetBasisZ()};
            // pickup plan

            gContext.mTranslationPlan = BuildPlane(gContext.mModelLocal.v.position, 
                movePlanNormal[type - MT_SCALE_X]);
            const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
                gContext.mTranslationPlan);
            gContext.mTranslationPlanOrigin = gContext.mRayOrigin + gContext.mRayVector * len;
            gContext.mMatrixOrigin = gContext.mModelLocal.v.position;
            gContext.mScale.Set(1.f, 1.f, 1.f);
            gContext.mRelativeOrigin = (gContext.mTranslationPlanOrigin - gContext.mModelLocal.v.position) * 
                (1.f / gContext.mScreenFactor);
            gContext.mScaleValueOrigin = makeVect(gContext.mModelSource.v.right.Length(), 
                gContext.mModelSource.v.up.Length(), gContext.mModelSource.v.dir.Length());
            gContext.mSaveMousePosx = io.MousePos.x;
         }
      }
      // scale
      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && 
          IsScaleType(gContext.mCurrentOperation))
      {
         ImGui::SetNextFrameWantCaptureMouse(true);

         const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, 
             gContext.mTranslationPlan);
         vec_t newPos = gContext.mRayOrigin + gContext.mRayVector * len;
         vec_t newOrigin = newPos - gContext.mRelativeOrigin * gContext.mScreenFactor;
         vec_t delta = newOrigin - gContext.mModelLocal.v.position;

         // 1 axis constraint
         if (gContext.mCurrentOperation >= MT_SCALE_X && gContext.mCurrentOperation <= MT_SCALE_Z)
         {
            int axisIndex = gContext.mCurrentOperation - MT_SCALE_X;
            const vec_t& axisValue = *(vec_t*)&gContext.mModelLocal.m[axisIndex];
            float lengthOnAxis = Dot(axisValue, delta);
            delta = axisValue * lengthOnAxis;

            vec_t baseVector = gContext.mTranslationPlanOrigin - gContext.mModelLocal.v.position;
            float ratio = Dot(axisValue, baseVector + delta) / Dot(axisValue, baseVector);

            gContext.mScale[axisIndex] = max(ratio, 0.001f);
         }
         else
         {
            float scaleDelta = (io.MousePos.x - gContext.mSaveMousePosx) * 0.01f;
            gContext.mScale.Set(max(1.f + scaleDelta, 0.001f));
         }

         // snap
         if (snap)
         {
            float scaleSnap[] = { snap[0], snap[0], snap[0] };
            ComputeSnap(gContext.mScale, scaleSnap);
         }

         // no 0 allowed
         for (int i = 0; i < 3; i++)
            gContext.mScale[i] = max(gContext.mScale[i], 0.001f);

         if (gContext.mScaleLast != gContext.mScale)
            modified = true;
         gContext.mScaleLast = gContext.mScale;

         // compute matrix & delta
         matrix_t deltaMatrixScale;
         deltaMatrixScale.Scale(gContext.mScale * gContext.mScaleValueOrigin);

         matrix_t res = deltaMatrixScale * gContext.mModelLocal;
         *(matrix_t*)matrix = res;

         {
            vec_t deltaScale = gContext.mScale * gContext.mScaleValueOrigin;

            vec_t originalScaleDivider;
            originalScaleDivider.x = 1 / gContext.mModelScaleOrigin.x;
            originalScaleDivider.y = 1 / gContext.mModelScaleOrigin.y;
            originalScaleDivider.z = 1 / gContext.mModelScaleOrigin.z;

            deltaScale = deltaScale * originalScaleDivider;

            //deltaMatrixScale.Scale(deltaScale);
            //memcpy(&ds, deltaMatrixScale.m16, sizeof(float) * 16);
            //ds = float3(deltaMatrixScale.m[0][0], deltaMatrixScale.m[1][1], deltaMatrixScale.m[2][2]);
            ds = float3(deltaScale.x, deltaScale.y, deltaScale.z);
         }

         if (!io.MouseDown[0])
         {
            gContext.mbUsing = false;
            gContext.mScale.Set(1.f, 1.f, 1.f);
         }

         type = gContext.mCurrentOperation;
      }
      return modified;
   }

   static bool HandleRotation(float* matrix, float4x4a& dr, OPERATION op, int& type, 
       const float* snap)
   {
      if(!Intersects(op, ROTATE) || type != MT_NONE || !gContext.mbMouseOver)
        return false;

      ImGuiIO& io = ImGui::GetIO();
      bool applyRotationLocaly = gContext.mMode == LOCAL;
      bool modified = false;

      if (!gContext.mbUsing)
      {
         type = gContext.mbOverGizmoHotspot ? MT_NONE : GetRotateType(op);
         gContext.mbOverGizmoHotspot |= type != MT_NONE;

         if (type != MT_NONE)
            ImGui::SetNextFrameWantCaptureMouse(true);

         if (type == MT_ROTATE_SCREEN)
            applyRotationLocaly = true;

         if (CanActivate() && type != MT_NONE)
         {
            gContext.mbUsing = true;
            gContext.mEditingID = gContext.GetCurrentID();
            gContext.mCurrentOperation = type;
            const vec_t rotatePlanNormal[] = { gContext.mModel.v.right, gContext.mModel.v.up, 
                gContext.mModel.v.dir, -App::GetCamera().GetBasisZ() };
            // pickup plan
            if (applyRotationLocaly)
            {
               gContext.mTranslationPlan = BuildPlane(gContext.mModel.v.position, 
                   rotatePlanNormal[type - MT_ROTATE_X]);
            }
            else
            {
               gContext.mTranslationPlan = BuildPlane(gContext.mModelSource.v.position, 
                   directionUnary[type - MT_ROTATE_X]);
            }

            const float len = IntersectRayPlane(gContext.mRayOrigin, gContext.mRayVector, gContext.mTranslationPlan);
            vec_t localPos = gContext.mRayOrigin + gContext.mRayVector * len - gContext.mModel.v.position;
            gContext.mRotationVectorSource = Normalized(localPos);
            gContext.mRotationAngleOrigin = ComputeAngleOnPlan();
         }
      }

      // rotation
      if (gContext.mbUsing && (gContext.GetCurrentID() == gContext.mEditingID) && 
          IsRotateType(gContext.mCurrentOperation))
      {
         ImGui::SetNextFrameWantCaptureMouse(true);

         gContext.mRotationAngle = ComputeAngleOnPlan();
         if (snap)
         {
            float snapInRadian = snap[0] * DEG2RAD;
            ComputeSnap(&gContext.mRotationAngle, snapInRadian);
         }
         vec_t rotationAxisLocalSpace;

         rotationAxisLocalSpace.TransformVector(makeVect(gContext.mTranslationPlan.x, 
             gContext.mTranslationPlan.y, gContext.mTranslationPlan.z, 0.f), gContext.mModelInverse);
         rotationAxisLocalSpace.Normalize();

         matrix_t deltaRotation;
         deltaRotation.RotationAxis(rotationAxisLocalSpace, gContext.mRotationAngle - 
             gContext.mRotationAngleOrigin);
         if (gContext.mRotationAngle != gContext.mRotationAngleOrigin)
            modified = true;
         gContext.mRotationAngleOrigin = gContext.mRotationAngle;

         matrix_t scaleOrigin;
         scaleOrigin.Scale(gContext.mModelScaleOrigin);

         if (applyRotationLocaly)
            *(matrix_t*)matrix = scaleOrigin * deltaRotation * gContext.mModelLocal;
         else
         {
            matrix_t res = gContext.mModelSource;
            res.v.position.Set(0.f);

            *(matrix_t*)matrix = res * deltaRotation;
            ((matrix_t*)matrix)->v.position = gContext.mModelSource.v.position;
         }

         //matrix_t deltaMatrix = gContext.mModelInverse * deltaRotation * gContext.mModel;
         //memcpy(&dr, &deltaMatrix, 16 * sizeof(float));
         memcpy(&dr, &deltaRotation, 16 * sizeof(float));

         if (!io.MouseDown[0])
         {
            gContext.mbUsing = false;
            gContext.mEditingID = ImGuiID(-1);
         }
         type = gContext.mCurrentOperation;
      }
      return modified;
   }

   void SetAlternativeWindow(ImGuiWindow* window)
   {
      gContext.mAlternativeWindow = window;
   }

   void SetID(int id)
   {
      gContext.mIDStack.back() = id;
   }

   ImGuiID GetID(const char* str, const char* str_end)
   {
      ImGuiID seed = gContext.mIDStack.back();
      ImGuiID id = ImHashStr(str, str_end ? (str_end - str) : 0, seed);
      return id;
   }

   ImGuiID GetID(const char* str)
   {
      return GetID(str, nullptr);
   }

   ImGuiID GetID(const void* ptr)
   {
      ImGuiID seed = gContext.mIDStack.back();
      ImGuiID id = ImHashData(&ptr, sizeof(void*), seed);
      return id;
   }

   ImGuiID GetID(int n)
   {
      ImGuiID seed = gContext.mIDStack.back();
      ImGuiID id = ImHashData(&n, sizeof(n), seed);
      return id;
   }

   void PushID(const char* str_id)
   {
      ImGuiID id = GetID(str_id);
      gContext.mIDStack.push_back(id);
   }

   void PushID(const char* str_id_begin, const char* str_id_end)
   {
      ImGuiID id = GetID(str_id_begin, str_id_end);
      gContext.mIDStack.push_back(id);
   }

   void PushID(const void* ptr_id)
   {
      ImGuiID id = GetID(ptr_id);
      gContext.mIDStack.push_back(id);
   }

   void PushID(int int_id)
   {
      ImGuiID id = GetID(int_id);
      gContext.mIDStack.push_back(id);
   }

   void PopID()
   {
      IM_ASSERT(gContext.mIDStack.Size > 1); // Too many PopID(), or could be popping in a wrong/different window?
      gContext.mIDStack.pop_back();
   }

   void AllowAxisFlip(bool value)
   {
     gContext.mAllowAxisFlip = value;
   }

   void SetAxisLimit(float value)
   {
     gContext.mAxisLimit=value;
   }

   void SetAxisMask(bool x, bool y, bool z)
   {
      gContext.mAxisMask = (x ? 1 : 0) + (y ? 2 : 0) + (z ? 4 : 0);
   }

   void SetPlaneLimit(float value)
   {
     gContext.mPlaneLimit = value;
   }

   bool IsOver(float* position, float pixelRadius)
   {
      const ImGuiIO& io = ImGui::GetIO();

      float radius = sqrtf((ImLengthSqr(worldToPos({ position[0], position[1], position[2], 0 }, 
          gContext.mViewProjection) - io.MousePos)));
      return radius < pixelRadius;
   }

   bool Manipulate(OPERATION operation, MODE mode, float4x4a& world, float3& dt, float4x4a& dr, 
       float3& ds, const float* snap, const float* localBounds, const float* boundsSnap)
   {
      gContext.mDrawList->PushClipRect (ImVec2(gContext.mX, gContext.mY), 
          ImVec2(gContext.mX + gContext.mWidth, gContext.mY + gContext.mHeight), false);

      // Scale is always local or matrix will be skewed when applying world scale or oriented matrix
      ComputeContext(world, (operation & SCALE) ? LOCAL : mode);

      // behind camera
      //vec_t camSpacePosition;
      //camSpacePosition.TransformPoint(makeVect(0.f, 0.f, 0.f), gContext.mMVP);
      //if (!gContext.mIsOrthographic && camSpacePosition.z < 0.001f && !gContext.mbUsing)
      //   return;

      // --
      int type = MT_NONE;
      float* matrix = reinterpret_cast<float*>(&world);
      bool modified = false;

      // Init to identify transformations
      dt = float3(0);
      dr = store(identity());
      ds = float3(1);

      if (gContext.mbEnable)
      {
         if (!gContext.mbUsingBounds)
         {
             modified = HandleTranslation(matrix, dt, operation, type, snap) ||
                HandleScale(matrix, ds, operation, type, snap) ||
                HandleRotation(matrix, dr, operation, type, snap);
         }
      }

      gContext.mOperation = operation;
      if (!gContext.mbUsingBounds)
      {
         DrawRotationGizmo(operation, type);
         DrawTranslationGizmo(operation, type);
         DrawScaleGizmo(operation, type);
         DrawScaleUniveralGizmo(operation, type);
      }

      gContext.mDrawList->PopClipRect ();
      return modified;
   }

   void SetGizmoSizeClipSpace(float value)
   {
      gContext.mGizmoSizeClipSpace = value;
   }
};
