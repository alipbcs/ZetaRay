#include "Camera.h"
#include "../App/Timer.h"
#include "../Core/RendererCore.h"
#include "../Math/CollisionFuncs.h"
#include "../Math/Sampling.h"
#include "../Support/Param.h"

using namespace ZetaRay::Scene;
using namespace ZetaRay::Support;
using namespace ZetaRay::Math;

namespace
{
    ZetaInline void __vectorcall setCamPos(const __m128 vNewCamPos, float4x4a& view, float4x4a& viewInv)
    {
        const __m128 vT = negate(vNewCamPos);
        viewInv.m[3] = store(vNewCamPos);

        __m128 vRow0 = _mm_load_ps(reinterpret_cast<float*>(&view.m[0]));
        __m128 vRow1 = _mm_load_ps(reinterpret_cast<float*>(&view.m[1]));
        __m128 vRow2 = _mm_load_ps(reinterpret_cast<float*>(&view.m[2]));

        __m128 v4thRow = _mm_mul_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(0, 0, 0, 0)), vRow0);
        v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(1, 1, 1, 0)), vRow1, v4thRow);
        v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(2, 2, 2, 0)), vRow2, v4thRow);

        // Set the 4th element to 1.0
        view.m[3] = store(_mm_insert_ps(v4thRow, _mm_set1_ps(1.0f), 0x30));
    }

    ZetaInline v_float4x4 __vectorcall resetViewMatrix(const __m128 vBasisX, const __m128 vBasisY,
        const __m128 vBasisZ, const __m128 vEye, float4x4a& viewInv)
    {
        v_float4x4 vViewInv;
        vViewInv.vRow[0] = vBasisX;
        vViewInv.vRow[1] = vBasisY;
        vViewInv.vRow[2] = vBasisZ;
        vViewInv.vRow[3] = _mm_setzero_ps();

        v_float4x4 vNewView = transpose(vViewInv);
        // Transforms from view space to world space
        vViewInv.vRow[3] = vEye;
        viewInv = store(vViewInv);

        const __m128 vT = negate(vEye);
        __m128 v4thRow = _mm_mul_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(0, 0, 0, 0)), vNewView.vRow[0]);
        v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(1, 1, 1, 0)), vNewView.vRow[1], v4thRow);
        v4thRow = _mm_fmadd_ps(_mm_shuffle_ps(vT, vT, V_SHUFFLE_XYZW(2, 2, 2, 0)), vNewView.vRow[2], v4thRow);

        // Set the 4th element to 1.0
        vNewView.vRow[3] = _mm_insert_ps(v4thRow, _mm_set1_ps(1.0f), 0x30);

        return vNewView;
    }
}

//--------------------------------------------------------------------------------------
// Camera
//--------------------------------------------------------------------------------------

void Camera::Init(float3 posw, float aspectRatio, float fov, float nearZ, bool jitter, 
    float3 focusOrViewDir, bool lookAt)
{
    m_posW = float4a(posw, 1.0f);
    m_FOV = fov;
    m_tanHalfFOV = tanf(0.5f * m_FOV);
    m_nearZ = nearZ;
    m_farZ = FLT_MAX;
    m_aspectRatio = aspectRatio;
    m_viewFrustum = ViewFrustum(fov, aspectRatio, nearZ, m_farZ);
    m_jitteringEnabled = jitter;

    m_pixelSampleAreaWidth = 1.0f / App::GetRenderer().GetRenderWidth();
    m_pixelSampleAreaHeight = 1.0f / App::GetRenderer().GetRenderHeight();

    // "Ray Tracing Gems", ch. 20, eq. (30)
    m_pixelSpreadAngle = atanf(2 * m_tanHalfFOV / App::GetRenderer().GetRenderHeight());

    v_float4x4 vView;

    if (lookAt)
        vView = lookAtLH(m_posW, focusOrViewDir, m_upW);
    else
    {
        Assert(fabs(focusOrViewDir.dot(focusOrViewDir)) > 1e-7,
            "(0, 0, 0) is not a valid view vector.");
        vView = lookToLH(m_posW, focusOrViewDir, m_upW);
    }

    m_view = store(vView);

    // Extract the basis vectors from the view matrix. Make sure the 4th element is zero.
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
    jitterCamera.InitBool("Renderer", "Anti-Aliasing", "Jitter Camera Ray", 
        fastdelegate::MakeDelegate(this, &Camera::SetJitteringEnabled), m_jitteringEnabled);
    App::AddParam(jitterCamera);

    ParamVariant fovParam;
    fovParam.InitFloat("Scene", "Camera", "FOV", fastdelegate::MakeDelegate(this, &Camera::SetFOV),
        Math::RadiansToDegrees(m_FOV), 45, 90, 1, "Lens");
    App::AddParam(fovParam);

    ParamVariant coeff;
    coeff.InitFloat("Scene", "Camera", "Friction Coeff.", 
        fastdelegate::MakeDelegate(this, &Camera::SetFrictionCoeff),
        m_frictionCoeff, 1, 20, 1, "Motion");
    App::AddParam(coeff);

    ParamVariant accAng;
    accAng.InitFloat2("Scene", "Camera", "Acceleration (Angular)", 
        fastdelegate::MakeDelegate(this, &Camera::SetAngularAcceleration),
        m_rotAccScale, 1.0f, 70.0f, 1.0f, "Motion");
    App::AddParam(accAng);

    ParamVariant coeffAng;
    coeffAng.InitFloat2("Scene", "Camera", "Friction Coeff. (Angular)", 
        fastdelegate::MakeDelegate(this, &Camera::SetAngularFrictionCoeff),
        m_rotFrictionCoeff, 1, 50, 1, "Motion");
    App::AddParam(coeffAng);

    ParamVariant clampTo0;
    clampTo0.InitBool("Scene", "Camera", "Snap Small V0 To Zero", 
        fastdelegate::MakeDelegate(this, &Camera::ClampSmallV0To0),
        m_clampSmallV0ToZero, "Motion");
    App::AddParam(clampTo0);

    ParamVariant focusDepth;
    focusDepth.InitFloat("Scene", "Camera", "Focus Depth",
        fastdelegate::MakeDelegate(this, &Camera::FocusDepthCallback),
        m_focusDepth, 0.1f, 25.0f, 1e-2f, "Lens");
    App::AddParam(focusDepth);

    ParamVariant fstop;
    fstop.InitFloat("Scene", "Camera", "F-Stop",
        fastdelegate::MakeDelegate(this, &Camera::FStopCallback),
        m_fStop, 1.0f, 5.0f, 1e-2f, "Lens");
    App::AddParam(fstop);

    ParamVariant focalLen;
    focalLen.InitFloat("Scene", "Camera", "Focal Length (mm)",
        fastdelegate::MakeDelegate(this, &Camera::FocalLengthCallback),
        m_focalLength, 10.0f, 100.0f, 1e-1f, "Lens");
    App::AddParam(focalLen);

    m_jitterPhaseCount = int(BASE_PHASE_COUNT * powf(App::GetUpscalingFactor(), 2.0f));
}

void Camera::Update(const Motion& m)
{
    float2 dMouse = float2(m.dMouse_x, m.dMouse_y) * m_rotAccScale;
    float2 acc = dMouse - m_rotFrictionCoeff * m_initialAngularVelocity;
    float2 newVelocity = acc * m.dt + m_initialAngularVelocity;
    float2 dtheta = acc * m.dt * m.dt / 2 + m_initialAngularVelocity * m.dt;
    m_initialAngularVelocity = newVelocity;

    if (dtheta.x != 0.0)
        RotateY(dtheta.x);
    if (dtheta.y != 0.0)
        RotateX(dtheta.y);

    const __m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
    const __m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
    const __m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));
    __m128 vInitialVelocity = _mm_load_ps(reinterpret_cast<float*>(&m_initialVelocity));

    const __m128 vForce = loadFloat3(const_cast<float3&>(m.Acceleration));
    __m128 vAcc = _mm_mul_ps(vBasisX, _mm_broadcastss_ps(vForce));
    vAcc = _mm_fmadd_ps(vBasisZ, _mm_shuffle_ps(vForce, vForce, V_SHUFFLE_XYZW(2, 2, 2, 2)), vAcc);
    vAcc = _mm_fmadd_ps(_mm_set1_ps(-m_frictionCoeff), vInitialVelocity, vAcc);

    const __m128 vDt = _mm_set1_ps(m.dt);
    const __m128 vVelocity = _mm_fmadd_ps(vAcc, vDt, vInitialVelocity);
    const __m128 vDt2Over2 = _mm_mul_ps(_mm_mul_ps(vDt, vDt), _mm_set1_ps(0.5));
    __m128 vVdt = _mm_mul_ps(vInitialVelocity, vDt);
    __m128 vNewEye = _mm_fmadd_ps(vAcc, vDt2Over2, vVdt);
    vNewEye = _mm_add_ps(vNewEye, vEye);
    vInitialVelocity = vVelocity;

    if (m_clampSmallV0ToZero)
    {
        __m128 vForceIs0 = _mm_cmpeq_ps(vForce, _mm_setzero_ps());
        __m128 vV0Near0 = _mm_cmple_ps(abs(vInitialVelocity), _mm_set1_ps(3e-2f));
        __m128 vClamp = _mm_and_ps(vForceIs0, vV0Near0);
        vInitialVelocity = _mm_blendv_ps(vInitialVelocity, _mm_setzero_ps(), vClamp);
    }

    setCamPos(vNewEye, m_view, m_viewInv);
    m_posW = store(vNewEye);
    m_initialVelocity = store(vInitialVelocity);

    if (m_jitteringEnabled)
    {
        const uint32_t frame = App::GetTimer().GetTotalFrameCount() % m_jitterPhaseCount;
        m_currJitter.x = Halton(frame + 1, 2) - 0.5f;
        m_currJitter.y = Halton(frame + 1, 3) - 0.5f;

#if 0
        // Shift each pixel by a value in [-0.5 / PixelWidth, 0.5 / PixelWidth] * [-0.5 / PixelHeight, 0.5 / PixelHeight]
        // Jitter is relative to unit pixel offset -- [-0.5, -0.5] x [+0.5, +0.5]
        // NDC is relative to [-1, -1] x [+1, +1], therefore multiply by 2
        float2 projOffset = m_currJitter * float2(m_pixelSampleAreaWidth, m_pixelSampleAreaHeight) * float2(2.0f, -2.0f);

        m_proj.m[2].x = projOffset.x;
        m_proj.m[2].y = projOffset.y;
#endif
    }
}

void Camera::UpdateProj()
{
    v_float4x4 vP;

    vP = perspectiveReverseZ(m_aspectRatio, m_FOV, m_nearZ);
    m_proj = store(vP);

    m_viewFrustum = ViewFrustum(m_FOV, m_aspectRatio, m_nearZ, m_farZ);
}

void Camera::OnWindowSizeChanged()
{
    const int renderWidth = App::GetRenderer().GetRenderWidth();
    const int renderfHeight = App::GetRenderer().GetRenderHeight();

    m_aspectRatio = (float)renderWidth / renderfHeight;

    UpdateProj();

    m_pixelSpreadAngle = atanf(2 * m_tanHalfFOV / renderfHeight);

    m_pixelSampleAreaWidth = 1.0f / renderWidth;
    m_pixelSampleAreaHeight = 1.0f / renderfHeight;

    m_jitterPhaseCount = int(BASE_PHASE_COUNT * powf(App::GetUpscalingFactor(), 2.0f));
}

void Camera::RotateX(float theta)
{
    __m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
    __m128 vBasisY = _mm_load_ps(reinterpret_cast<float*>(&m_basisY));
    __m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
    const __m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

    v_float4x4 vR = rotate(vBasisX, theta);
    vBasisY = mul(vR, vBasisY);
    vBasisZ = mul(vR, vBasisZ);

    // Orthonormalize
    vBasisZ = normalize(vBasisZ);
    vBasisX = normalize(cross(vBasisY, vBasisZ));
    vBasisY = cross(vBasisZ, vBasisX);

    v_float4x4 vNewView = resetViewMatrix(vBasisX, vBasisY, vBasisZ, vEye, m_viewInv);
    m_view = store(vNewView);

    m_basisX = store(vBasisX);
    m_basisY = store(vBasisY);
    m_basisZ = store(vBasisZ);
}

void Camera::RotateY(float theta)
{
    __m128 vBasisX = _mm_load_ps(reinterpret_cast<float*>(&m_basisX));
    __m128 vBasisY = _mm_load_ps(reinterpret_cast<float*>(&m_basisY));
    __m128 vBasisZ = _mm_load_ps(reinterpret_cast<float*>(&m_basisZ));
    const __m128 vEye = _mm_load_ps(reinterpret_cast<float*>(&m_posW));

    v_float4x4 vR = rotateY(theta);

    vBasisX = mul(vR, vBasisX);
    vBasisY = mul(vR, vBasisY);
    vBasisZ = mul(vR, vBasisZ);

    // Orthonormalize
    vBasisZ = normalize(vBasisZ);
    vBasisX = normalize(cross(vBasisY, vBasisZ));
    vBasisY = cross(vBasisZ, vBasisX);

    v_float4x4 vNewView = resetViewMatrix(vBasisX, vBasisY, vBasisZ, vEye, m_viewInv);
    m_view = store(vNewView);

    m_basisX = store(vBasisX);
    m_basisY = store(vBasisY);
    m_basisZ = store(vBasisZ);
}

void Camera::SetFOV(const ParamVariant& p)
{
    m_FOV = Math::DegreesToRadians(p.GetFloat().m_value);
    m_tanHalfFOV = tanf(0.5f * m_FOV);

    UpdateProj();
}

void Camera::SetJitteringEnabled(const ParamVariant& p)
{
    m_jitteringEnabled = p.GetBool();

    m_proj.m[2].x = 0.0f;
    m_proj.m[2].y = 0.0f;

    m_currJitter = float2(0.0f, 0.0f);
}

void Camera::SetFrictionCoeff(const Support::ParamVariant& p)
{
    m_frictionCoeff = p.GetFloat().m_value;
}

void Camera::SetAngularFrictionCoeff(const Support::ParamVariant& p)
{
    m_rotFrictionCoeff = p.GetFloat2().m_value;
}

void Camera::ClampSmallV0To0(const Support::ParamVariant& p)
{
    m_clampSmallV0ToZero = p.GetBool();
}

void Camera::SetAngularAcceleration(const Support::ParamVariant& p)
{
    m_rotAccScale = p.GetFloat2().m_value;
}

void Camera::FocusDepthCallback(const Support::ParamVariant& p)
{
    m_focusDepth = p.GetFloat().m_value;
}

void Camera::FStopCallback(const Support::ParamVariant& p)
{
    m_fStop = p.GetFloat().m_value;
}

void Camera::FocalLengthCallback(const Support::ParamVariant& p)
{
    m_focalLength = p.GetFloat().m_value;
}