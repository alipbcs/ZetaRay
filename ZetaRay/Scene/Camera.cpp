#include "Camera.h"
#include "../Win32/App.h"
#include "../Win32/Timer.h"
#include "../Core/Renderer.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Sampling.h"
#include "../SupportSystem/Param.h"

using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::Math;

namespace
{
	__forceinline void __vectorcall setCamPos(const __m128 vNewCamPos, float4x4a& view, float4x4a& viewInv) noexcept
	{
		const __m128 vT = minus(vNewCamPos);
		viewInv.m[3] = store(vNewCamPos);

		__m128 vRow0 = _mm_load_ps(reinterpret_cast<float*>(&view.m[0]));
		__m128 vRow1 = _mm_load_ps(reinterpret_cast<float*>(&view.m[1]));
		__m128 vRow2 = _mm_load_ps(reinterpret_cast<float*>(&view.m[2]));

		__m128 v4thRow = _mm_mul_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(0, 0, 0, 0)), vRow0);
		v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(1, 1, 1, 0)), vRow1, v4thRow);
		v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(2, 2, 2, 0)), vRow2, v4thRow);

		// set the 4th element to 1.0
		view.m[3] = store(_mm_insert_ps(v4thRow, _mm_set1_ps(1.0f), 0x30));
	}

	__forceinline v_float4x4 __vectorcall resetViewMatrix(const __m128 vBasisX, const __m128 vBasisY,
		const __m128 vBasisZ, const __m128 vEye, float4x4a& viewInv) noexcept
	{
		v_float4x4 vViewInv;
		vViewInv.vRow[0] = vBasisX;
		vViewInv.vRow[1] = vBasisY;
		vViewInv.vRow[2] = vBasisZ;
		vViewInv.vRow[3] = _mm_setzero_ps();

		v_float4x4 vNewView = transpose(vViewInv);
		// transforms from view-space to world-space
		vViewInv.vRow[3] = vEye;
		viewInv = store(vViewInv);

		const __m128 vT = minus(vEye);
		__m128 v4thRow = _mm_mul_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(0, 0, 0, 0)), vNewView.vRow[0]);
		v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(1, 1, 1, 0)), vNewView.vRow[1], v4thRow);
		v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(2, 2, 2, 0)), vNewView.vRow[2], v4thRow);

		// set the 4th element to 1.0
		vNewView.vRow[3] = _mm_insert_ps(v4thRow, _mm_set1_ps(1.0f), 0x30);

		return vNewView;
	}
}

//--------------------------------------------------------------------------------------
// Camera
//--------------------------------------------------------------------------------------

void Camera::Init(float3 posw, float aspectRatio, float fov, float nearZ, float farZ, bool jitter) noexcept
{
	m_posW = float4a(posw, 1.0f);
	m_FOV = fov;
	m_nearZ = nearZ;
	m_farZ = RendererConstants::USE_REVERSE_Z ? FLT_MAX : farZ;
	m_aspectRatio = aspectRatio;
	m_viewFrustum = ViewFrustum(fov, aspectRatio, nearZ, farZ);
	m_jitteringEnabled = jitter;

	m_pixelSampleAreaWidth = 1.0f / App::GetRenderer().GetRenderWidth();
	m_pixelSampleAreaHeight = 1.0f / App::GetRenderer().GetRenderHeight();

	// Eq. (30) in Ray Tracing Gems chapter 20
	m_pixelSpreadAngle = atanf(2 * tanf(0.5f * m_FOV) / App::GetRenderer().GetRenderHeight());
	float4a focus(0.0f, 0.0f, 0.0f, 0.0f);

	v_float4x4 vView = lookAtLH(m_posW, focus, m_upW);
	m_view = store(vView);

	// extract the basis vectors from the view matrix. make sure the 4th element is zero
	v_float4x4 vT = transpose(vView);
	__m128 vBasisX = _mm_insert_ps(vT.vRow[0], vT.vRow[0], 0x8);
	__m128 vBasisY = _mm_insert_ps(vT.vRow[1], vT.vRow[1], 0x8);
	__m128 vBasisZ = _mm_insert_ps(vT.vRow[2], vT.vRow[2], 0x8);
	__m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	Math::v_float4x4 vViewToWorld(vBasisX, vBasisY, vBasisZ, vEye);
	m_viewInv = store(vViewToWorld);

	UpdateProj();

	m_basisX = store(vBasisX);
	m_basisY = store(vBasisY);
	m_basisZ = store(vBasisZ);

	ParamVariant jitterCamera;
	jitterCamera.InitBool("Scene", "Camera", "Jitter", fastdelegate::MakeDelegate(this, &Camera::SetJitteringEnabled),
		m_jitteringEnabled);
	App::AddParam(jitterCamera);

	m_jitterPhaseCount = int(8 * powf(App::GetUpscalingFactor(), 2.0f));
}

void Camera::Update() noexcept
{
	if (m_jitteringEnabled)
	{
		const uint64_t frame = App::GetTimer().GetTotalFrameCount();
		//m_currJitter = k_halton[frame & 0x7];	// frame % 8
		m_currJitter = k_halton[frame % m_jitterPhaseCount];	// frame % jitterPhaseCount
		
		// shift each pixel by a value in [-0.5 / PixelWidth, 0.5 / PixelWidth] * [-0.5 / PixelHeight, 0.5 / PixelHeight]
		m_currProjOffset = m_currJitter * float2(m_pixelSampleAreaWidth, m_pixelSampleAreaHeight) * float2(2.0f, -2.0f);

		m_proj.m[2].x = m_currProjOffset.x;
		m_proj.m[2].y = m_currProjOffset.y;
	}
}

void Camera::UpdateProj() noexcept
{
	v_float4x4 vP;

	vP = perspectiveReverseZ(m_aspectRatio, m_FOV, m_nearZ);
	m_proj = store(vP);

	m_viewFrustum = ViewFrustum(m_FOV, m_aspectRatio, m_nearZ, m_farZ);
}

void Camera::SetJitteringEnabled(const ParamVariant& p) noexcept
{
	m_jitteringEnabled = p.GetBool();
	m_proj.m[2].x = 0.0f;
	m_proj.m[2].y = 0.0f;
}

void Camera::OnWindowSizeChanged() noexcept
{
	const int renderWidth = App::GetRenderer().GetRenderWidth();
	const int renderfHeight = App::GetRenderer().GetRenderHeight();

	m_aspectRatio = (float)renderWidth / renderfHeight;

	UpdateProj();

	m_pixelSpreadAngle = atanf(2 * tanf(0.5f * m_FOV) / renderfHeight);

	m_pixelSampleAreaWidth = 1.0f / renderWidth;
	m_pixelSampleAreaHeight = 1.0f / renderfHeight;

	m_jitterPhaseCount = int(8 * powf(App::GetUpscalingFactor(), 2.0f));
}

void Camera::MoveX(float dt) noexcept
{
	const __m128 vDt = _mm_set1_ps(dt);
	const __m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
	__m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	vEye = _mm_fmadd_ps(vDt, vBasisX, vEye);
	setCamPos(vEye, m_view, m_viewInv);

	m_posW = store(vEye);
}

void Camera::MoveY(float dt) noexcept
{
	const __m128 vUp = _mm_load_ps(reinterpret_cast<float*>(&m_upW));
	__m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	vEye = _mm_fmadd_ps(_mm_set1_ps(dt), vUp, vEye);
	setCamPos(vEye, m_view, m_viewInv);

	m_posW = store(vEye);
}

void Camera::MoveZ(float dt) noexcept
{
	const __m128 vDt = _mm_set1_ps(dt);
	const __m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
	__m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	vEye = _mm_fmadd_ps(vDt, vBasisZ, vEye);
	setCamPos(vEye, m_view, m_viewInv);

	m_posW = store(vEye);
}

void Camera::RotateX(float dt) noexcept
{
	__m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
	__m128 vBasisY = _mm_load_ps(reinterpret_cast<float*>(&m_basisY));
	__m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
	const __m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	v_float4x4 vR = rotate(vBasisX, dt);
	vBasisY = mul(vR, vBasisY);
	vBasisZ = mul(vR, vBasisZ);

	// orthonormalize
	vBasisZ = normalize(vBasisZ);
	vBasisX = normalize(cross(vBasisY, vBasisZ));
	vBasisY = cross(vBasisZ, vBasisX);

	v_float4x4 vNewView = resetViewMatrix(vBasisX, vBasisY, vBasisZ, vEye, m_viewInv);
	m_view = store(vNewView);

	m_basisX = store(vBasisX);
	m_basisY = store(vBasisY);
	m_basisZ = store(vBasisZ);
}

void Camera::RotateY(float dt) noexcept
{
	__m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
	__m128 vBasisY = _mm_load_ps(reinterpret_cast<float*>(&m_basisY));
	__m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
	const __m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	v_float4x4 vR = rotateY(dt);

	vBasisX = mul(vR, vBasisX);
	vBasisY = mul(vR, vBasisY);
	vBasisZ = mul(vR, vBasisZ);

	// orthonormalize
	vBasisZ = normalize(vBasisZ);
	vBasisX = normalize(cross(vBasisY, vBasisZ));
	vBasisY = cross(vBasisZ, vBasisX);

	v_float4x4 vNewView = resetViewMatrix(vBasisX, vBasisY, vBasisZ, vEye, m_viewInv);
	m_view = store(vNewView);

	m_basisX = store(vBasisX);
	m_basisY = store(vBasisY);
	m_basisZ = store(vBasisZ);
}

void Camera::SetPos(float3 posW) noexcept
{
	m_posW = float4a(posW, 1.0f);
	__m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

	setCamPos(vEye, m_view, m_viewInv);
}
