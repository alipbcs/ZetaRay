//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------

// SH coeffs for projection of f(theta) = max(0, theta).
// c_l^m == 0 when m != 0
static const float COS_THETA_SH_COEFFS[11] = 
{ 
    0.8862268925f, 1.0233267546f, 0.4954159260f, 0.0000000000f, -0.1107783690f, 0.0000000000f, 
    0.0499271341f, 0.0000000000f, -0.0285469331f, 0.0000000000f, 0.0185080823f 
};

static const float LAMBDA_L[11] = 
{ 
    3.544907701f, 2.046653415f, 1.585330919f, 1.339849171f, 1.1816359f, 1.0688298f, 
    0.983180498f, 0.915291232f, 0.859766405f, 0.813257601, 0.773562279f
};

static const float LAMBDA_LxCOS_THETA_SH[11] =
{
    3.141592536f, 2.094395197f, 0.785398185f, 0.0f, -0.130899697f, 0.0f, 0.049087384f, 
    0.0f, -0.024543694f, 0.0f, 0.014317154f
};

//--------------------------------------------------------------------------------------
// Basis Functions in Cartesian Form
// Ref: P. Sloan, "Stupid Spherical Harmonics (SH) Tricks," 2008.
//--------------------------------------------------------------------------------------

//
// band l = 0
//
static const float SHBasis00 = 0.2820947917738781f;

//
// band l = 1
//
float SHBasis1_1(float3 w)
{
    return 0.4886025119029199f * w.y;
}

float SHBasis10(float3 w)
{
    return 0.4886025119029199f * w.z;
}

float SHBasis11(float3 w)
{
    return 0.4886025119029199f * w.x;
}

float4 ProjectToSH1(float3 w, float f)
{
    float4 y = float4(SHBasis00, SHBasis1_1(w), SHBasis10(w), SHBasis11(w));
    return y * f;
}

//
// band l = 2
//
float SHBasis2_2(float3 w)
{
    return 1.0925484305920792f * w.x * w.y;
}

float SHBasis2_1(float3 w)
{
    return 1.0925484305920792f * w.y * w.z;
}

float SHBasis20(float3 w)
{
    return 0.31539156525252f * (3.0f * w.z * w.z - 1.0f);
}

float SHBasis21(float3 w)
{
    return 1.0925484305920792f * w.x * w.z;
}

float SHBasis22(float3 w)
{
    return 0.5462742152960396f * (w.x * w.x - w.y * w.y);
}
