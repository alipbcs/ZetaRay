#include "Color.h"
#include "Color.h"
#include "Common.h"

using namespace ZetaRay;
using namespace ZetaRay::Math;
using namespace ZetaRay::Util;

float3 Math::sRGBToLinear(const float3& color)
{
    float3 linearLo = color / 12.92f;
    float3 tmp = (color + 0.055f) / 1.055f;
    float3 linearHi;
    linearHi.x = powf(Max(tmp.x, 0.0f), 2.4f);
    linearHi.y = powf(Max(tmp.y, 0.0f), 2.4f);
    linearHi.z = powf(Max(tmp.z, 0.0f), 2.4f);

    float3 ret;
    ret.x = color.x <= 0.0404499993f ? linearLo.x : linearHi.x;
    ret.y = color.y <= 0.0404499993f ? linearLo.y : linearHi.y;
    ret.z = color.z <= 0.0404499993f ? linearLo.z : linearHi.z;

    return ret;
}

// Ref: www.tannerhelland.com/4435/convert-temperature-rgb-algorithm-code/
float3 Math::ColorTemperatureTosRGB(float temperature)
{
    float3 ret;

    float tmpKelvin = Min(Max(temperature, 1000.0f), 40000.0f) / 100.0f;
    if (tmpKelvin <= 66.0f)
    {
        ret.x = 255.0f;
        ret.y = 99.4708025861f * logf(tmpKelvin) - 161.1195681661f;
    }
    else
    {
        float tmpCalc = tmpKelvin - 60.0f;
        ret.x = 329.698727446f * pow(tmpCalc, -0.1332047592f);
        ret.y = 288.1221695283f * pow(tmpCalc, -0.0755148492f);
    }

    if (tmpKelvin >= 66.0f)
        ret.z = 255.0f;
    else if (tmpKelvin <= 19.0f)
        ret.z = 0.0f;
    else
        ret.z = 138.5177312231f * logf(tmpKelvin - 10.0f) - 305.0447927307f;

    ret /= 255.0f;
    ret.x = Min(Max(ret.x, 0.0f), 1.0f);
    ret.y = Min(Max(ret.y, 0.0f), 1.0f);
    ret.z = Min(Max(ret.z, 0.0f), 1.0f);

    return ret;
}
