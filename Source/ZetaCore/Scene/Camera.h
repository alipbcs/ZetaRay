#pragma once

#include "../Math/Matrix.h"
#include "../Math/CollisionTypes.h"

namespace ZetaRay::Support
{
    struct ParamVariant;
}

namespace ZetaRay::Scene
{
    struct Motion
    {
        void Reset()
        {
            dt = 0.0f;
            Acceleration = Math::float3(0.0f);
            dMouse_x = 0;
            dMouse_y = 0;
        }

        float dt;
        Math::float3 Acceleration;
        int16_t dMouse_x;
        int16_t dMouse_y;
    };

    class Camera
    {
    public:
        Camera() = default;
        ~Camera() = default;

        void Init(Math::float3 pos, float aspectRatio, float fov, float nearZ = 1.0f, bool jitter = false, 
            Math::float3 focusOrViewDir = Math::float3(0.0f), bool lookAt = true);
        void Update(const Motion& m);
        void OnWindowSizeChanged();

        const Math::float4x4a& GetCurrView() const { return m_view; }
        const Math::float4x4a& GetViewInv() const { return m_viewInv; }
        const Math::float4x4a& GetCurrProj() const { return m_proj; }
        const Math::float3 GetPos() const { return Math::float3(m_posW.x, m_posW.y, m_posW.z); }
        float GetAspectRatio() const { return m_aspectRatio; }
        float GetFOV() const { return m_FOV; }
        float GetNearZ() const { return m_nearZ; }
        float GetFarZ() const { return m_farZ; }
        float GetTanHalfFOV() const { return m_tanHalfFOV; }
        float GetPixelSpreadAngle() const { return m_pixelSpreadAngle; }
        float GetFocalLength() const { return m_focalLength; }
        float GetFStop() const { return m_fStop; }
        float GetFocusDepth() const { return m_focusDepth; }
        Math::float2 GetCurrJitter() const { return m_currJitter; }
        Math::float3 GetBasisX() const { return Math::float3(m_basisX.x, m_basisX.y, m_basisX.z); }
        Math::float3 GetBasisY() const { return Math::float3(m_basisY.x, m_basisY.y, m_basisY.z); }
        Math::float3 GetBasisZ() const { return Math::float3(m_basisZ.x, m_basisZ.y, m_basisZ.z); }
        const Math::ViewFrustum& GetCameraFrustumViewSpace() const { return m_viewFrustum; }

    private:
        static constexpr int BASE_PHASE_COUNT = 16;

        void UpdateProj();
        void RotateX(float theta);
        void RotateY(float theta);

        // param callbacks
        void SetFOV(const Support::ParamVariant& p);
        void SetJitteringEnabled(const Support::ParamVariant& p);
        void SetFrictionCoeff(const Support::ParamVariant& p);
        void SetAngularFrictionCoeff(const Support::ParamVariant& p);
        void ClampSmallV0To0(const Support::ParamVariant& p);
        void SetAngularAcceleration(const Support::ParamVariant& p);
        void FocusDepthCallback(const Support::ParamVariant& p);
        void FStopCallback(const Support::ParamVariant& p);
        void FocalLengthCallback(const Support::ParamVariant& p);

        Math::float4x4a m_view;
        Math::float4x4a m_viewInv;
        Math::float4x4a m_proj;
        Math::float4a m_posW;
        Math::float4a m_initialVelocity = Math::float4a(0.0f);
        Math::float2 m_initialAngularVelocity = Math::float2(0.0f);
        Math::ViewFrustum m_viewFrustum;
        Math::float4a m_upW = Math::float4a(0.0f, 1.0f, 0.0f, 0.0f);

        Math::float4a m_basisX;
        Math::float4a m_basisY;
        Math::float4a m_basisZ;

        float m_FOV;
        float m_aspectRatio;
        float m_nearZ;
        float m_farZ;
        float m_tanHalfFOV;
        float m_pixelSpreadAngle;
        // - Focal point: point where incident rays that are parallel to the optical axis 
        //   and pass through the lens focus at 
        // - Focal length (f): distance from the focal point to the lens (in mm)
        float m_focalLength = 50;
        // f-number n expresses lens diameter as a fraction of focal length, d = f / n
        float m_fStop = 1.4f;
        // The distance that camera is focusing at
        float m_focusDepth = 5.0f;
        Math::float2 m_currJitter = Math::float2(0);
        float m_pixelSampleAreaWidth;
        float m_pixelSampleAreaHeight;
        int m_jitterPhaseCount;
        bool m_jitteringEnabled = false;
        float m_frictionCoeff = 10.0f;
        bool m_clampSmallV0ToZero = true;
        Math::float2 m_rotAccScale = Math::float2(31.0f, 25.0f);
        Math::float2 m_rotFrictionCoeff = Math::float2(27.0f, 22.0f);
    };
}
