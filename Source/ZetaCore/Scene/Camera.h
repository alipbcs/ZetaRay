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

        ZetaInline const Math::float4x4a& GetCurrView() const { return m_view; }
        ZetaInline const Math::float4x4a& GetViewInv() const { return m_viewInv; }
        ZetaInline const Math::float4x4a& GetProj() const { return m_proj; }
        ZetaInline const Math::float4x4a& GetProjNonInfiniteFarZ() const { return m_projNonInfinite; }
        ZetaInline const Math::float3 GetPos() const { return Math::float3(m_posW.x, m_posW.y, m_posW.z); }
        ZetaInline float GetAspectRatio() const { return m_aspectRatio; }
        ZetaInline float GetFOV() const { return m_FOV; }
        ZetaInline float GetNearZ() const { return m_nearZ; }
        ZetaInline float GetFarZ() const { return m_farZ; }
        ZetaInline float GetTanHalfFOV() const { return m_tanHalfFOV; }
        ZetaInline float GetPixelSpreadAngle() const { return m_pixelSpreadAngle; }
        // Unit is mm
        ZetaInline float GetFocalLength() const { return m_focalLength; }
        ZetaInline float GetFStop() const { return m_fStop; }
        ZetaInline float GetFocusDepth() const { return m_focusDepth; }
        // Unit is meters
        ZetaInline float GetLensRadius() const
        { 
            // mul by 0.5 to get radius from diameter
            return 0.5f * (m_focalLength / 1000.0f) / m_fStop; 
        }
        ZetaInline Math::float2 GetCurrJitter() const { return m_currJitter; }
        ZetaInline Math::float3 GetBasisX() const { return Math::float3(m_basisX.x, m_basisX.y, m_basisX.z); }
        ZetaInline Math::float3 GetBasisY() const { return Math::float3(m_basisY.x, m_basisY.y, m_basisY.z); }
        ZetaInline Math::float3 GetBasisZ() const { return Math::float3(m_basisZ.x, m_basisZ.y, m_basisZ.z); }
        ZetaInline const Math::ViewFrustum& GetCameraFrustumViewSpace() const { return m_viewFrustum; }

    private:
        static constexpr int BASE_PHASE_COUNT = 64;

        void UpdateProj();
        void UpdateFocalLength();
        void RotateX(float theta);
        void RotateY(float theta);

        // param callbacks
        void SetFOV(const Support::ParamVariant& p);
        void SetJitteringEnabled(const Support::ParamVariant& p);
        void SetFrictionCoeff(const Support::ParamVariant& p);
        void SetAngularFrictionCoeff(const Support::ParamVariant& p);
        void SetAngularAcceleration(const Support::ParamVariant& p);
        void FocusDepthCallback(const Support::ParamVariant& p);
        void FStopCallback(const Support::ParamVariant& p);

        Math::float4x4a m_view;
        Math::float4x4a m_viewInv;
        Math::float4x4a m_proj;
        Math::float4x4a m_projNonInfinite;
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
        float m_farZNonInfinite = 100.0f;
        float m_tanHalfFOV;
        float m_pixelSpreadAngle;
        // Unit is mm
        float m_sensorWidth = 36;
        // - Focal point: point where incident rays that are parallel to the optical axis 
        //   and pass through the lens focus at 
        // - Focal length (f): distance from the focal point to the lens (in mm). Computed
        //   from FOV as: 0.5 * sensor height / tan(0.5 FOV), where 
        //   sensor height = sensor width / aspect ratio (so a wider FOV is achieved by using 
        //   a shorter focal length, leading to less defocus blur and vice versa)
        float m_focalLength;
        // f-number n expresses lens diameter as a fraction of focal length, d = f / n
        float m_fStop = 1.4f;
        // The distance that camera is focusing at
        float m_focusDepth = 5.0f;
        Math::float2 m_currJitter = Math::float2(0);
        int m_jitterPhaseCount;
        bool m_jitteringEnabled = false;
        float m_frictionCoeff = 10.0f;
        Math::float2 m_rotAccScale = Math::float2(31.0f, 25.0f);
        Math::float2 m_rotFrictionCoeff = Math::float2(27.0f, 22.0f);
    };
}
