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
			RotationDegreesX = 0.0f;
			RotationDegreesY = 0.0f;
		}

		float dt;
		Math::float3 Acceleration;
		float RotationDegreesX;
		float RotationDegreesY;
	};

	class Camera
	{
	public:
		Camera() = default;
		~Camera() = default;

		void Init(Math::float3 posw, float aspectRatio, float fov, float nearZ = 0.1f, bool jitter = false, 
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
		Math::float2 GetCurrJitter() const { return m_currJitter; }
		Math::float3 GetBasisX() const { return Math::float3(m_basisX.x, m_basisX.y, m_basisX.z); }
		Math::float3 GetBasisY() const { return Math::float3(m_basisY.x, m_basisY.y, m_basisY.z); }
		Math::float3 GetBasisZ() const { return Math::float3(m_basisZ.x, m_basisZ.y, m_basisZ.z); }
		const Math::ViewFrustum& GetCameraFrustumViewSpace() const { return m_viewFrustum; }

	private:
		void UpdateProj();
		void SetFOV(const Support::ParamVariant& p);
		void SetJitteringEnabled(const Support::ParamVariant& p);
		void SetFrictionCoeff(const Support::ParamVariant& p);
		void ClampSmallV0To0(const Support::ParamVariant& p);

		void RotateX(float dt);
		void RotateY(float dt);

		Math::float4x4a m_view;
		Math::float4x4a m_viewInv;
		Math::float4x4a m_proj;

		Math::float4a m_posW;
		Math::float4a m_initialVelocity = Math::float4a(0.0f);

		Math::float4a m_upW = Math::float4a(0.0f, 1.0f, 0.0f, 0.0f);

		Math::float4a m_basisX;
		Math::float4a m_basisY;
		Math::float4a m_basisZ;

		Math::ViewFrustum m_viewFrustum;

		// Halton (2, 3) sequence (starting from offset 1) shifted to [-0.5, 0.5]
		inline static const Math::float2 k_halton[18] =
		{
			Math::float2(0.0f, -0.16666666666666669f),
			Math::float2(-0.25f, 0.16666666666666663f),
			Math::float2(0.25f, -0.3888888888888889f),
			Math::float2(-0.375f, -0.05555555555555558f),
			Math::float2(0.125f, 0.2777777777777777f),
			Math::float2(-0.125f, -0.2777777777777778f),
			Math::float2(0.375f, 0.05555555555555558f),
			Math::float2(-0.4375f, 0.38888888888888884f),
			Math::float2(0.0625f, -0.46296296296296297f),
			Math::float2(-0.1875f, -0.12962962962962965f),
			Math::float2(0.3125f, 0.20370370370370372f),
			Math::float2(-0.3125f, -0.35185185185185186f),
			Math::float2(0.1875f, -0.018518518518518545f),
			Math::float2(-0.0625f, 0.31481481481481466f),
			Math::float2(0.4375f, -0.24074074074074076f),
			Math::float2(-0.46875f, 0.09259259259259256f),
			Math::float2(0.03125f, 0.4259259259259258f),
			Math::float2(-0.21875f, -0.42592592592592593f)
		};
		
		float m_FOV;
		float m_aspectRatio;
		float m_nearZ;
		float m_farZ;
		float m_tanHalfFOV;
		float m_pixelSpreadAngle;
		Math::float2 m_currJitter = Math::float2(0);
		float m_pixelSampleAreaWidth;
		float m_pixelSampleAreaHeight;
		int m_jitterPhaseCount;
		bool m_jitteringEnabled = false;
		float m_frictionCoeff = 10.0f;
		bool m_clampSmallV0ToZero = true;
	};
}
