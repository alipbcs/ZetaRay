#include "Param.h"
#include <xxHash/xxhash.h>
#include <string.h>

using namespace ZetaRay::Support;
using namespace ZetaRay::Math;

//--------------------------------------------------------------------------------------
// ParamVariant
//--------------------------------------------------------------------------------------

void ParamVariant::InitCommon(const char* group, const char* subgroup, 
    const char* subsubgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg)
{
    Assert(group, "Group can't be null.");
    Assert(subgroup, "Subgroup can't be null.");
    Assert(name, "Name can't be null.");

    m_dlg = dlg;

    size_t lenGroup = Min((int)strlen(group), (MAX_GROUP_LEN - 1));
    Assert(lenGroup >= 1, "Empty group name.");
    memcpy(m_group, group, lenGroup);
    m_group[lenGroup] = '\0';

    size_t lenSubgroup = Min((int)strlen(subgroup), (MAX_SUBGROUP_LEN - 1));
    Assert(lenSubgroup >= 1, "Empty subgroup name.");
    memcpy(m_subgroup, subgroup, lenSubgroup);
    m_subgroup[lenSubgroup] = '\0';

    size_t lenSubSubgroup = subsubgroup ? Min((int)strlen(subsubgroup), (MAX_SUBSUBGROUP_LEN - 1)) : 0;
    if (subsubgroup)
        memcpy(m_subsubgroup, subsubgroup, lenSubSubgroup);
    
    m_subsubgroup[lenSubSubgroup] = '\0';

    size_t lenName = Min((int)strlen(name), (MAX_NAME_LEN - 1));
    Assert(lenName >= 1, "Empty name.");
    memcpy(m_name, name, lenName);
    m_name[lenName] = '\0';

    constexpr int BUFF_SIZE = ParamVariant::MAX_GROUP_LEN + ParamVariant::MAX_SUBGROUP_LEN + 
        ParamVariant::MAX_NAME_LEN;
    char buff[BUFF_SIZE];
    size_t ptr = 0;

    memcpy(buff, group, lenGroup);
    ptr += lenGroup;
    memcpy(buff + ptr, subgroup, lenSubgroup);
    ptr += lenSubgroup;
    memcpy(buff + ptr, name, lenName);

    m_id = XXH3_64bits(buff, ptr + lenName);
}

void ParamVariant::InitFloat(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float val, float min, float max, 
    float step, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float;
    m_float.Init(val, min, max, step);
}

void ParamVariant::InitInt(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, int val, int min, int max, 
    int step, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_int;
    m_int.Init(val, min, max, step);
}

void ParamVariant::InitFloat2(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float2 val, float min, 
    float max, float step, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float2;
    m_float2.Init(val, min, max, step, false);
}

void ParamVariant::InitFloat3(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float3 val, float min, float max, 
    float step, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float3;
    m_float3.Init(val, min, max, step, false);
}

void ParamVariant::InitUnitDir(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float pitch, float yaw, 
    const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    Assert(pitch >= 0 && pitch <= Math::PI, "Pitch must be in [0, +PI].");
    Assert(yaw >= 0 && yaw <= Math::TWO_PI, "Yaw must be in [0, 2 * PI].");
    m_type = PARAM_TYPE::PT_unit_dir;
    m_unitDir.Init(pitch, yaw);
}

void ParamVariant::InitUnitDir(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float3 dir, 
    const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    float theta;
    float phi;
    SphericalFromCartesian(dir, theta, phi);

    m_type = PARAM_TYPE::PT_unit_dir;
    m_unitDir.Init(theta, phi);
}

void ParamVariant::InitNormalizedFloat3(const char* group, const char* subgroup, 
    const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg, 
    float3 val, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float3;
    m_float3.Init(val, -1.0f, 1.0f, 1e-2f, true);
}

void ParamVariant::InitColor(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float3 val, 
    const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_color;
    m_float3.Init(val, 0.0f, 1.0f, 0.01f, false);
}

void ParamVariant::InitBool(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, bool val, 
    const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_bool;
    m_bool = val;
}

void ParamVariant::InitEnum(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, const char** vals, int num, 
    int index, const char* subsubgroup)
{
    InitCommon(group, subgroup, subsubgroup, name, dlg);

    m_type = PARAM_TYPE::PT_enum;
    Assert(index < num, "Out-of-bound index.");
    m_enum.Init(vals, num, index);
}

const FloatParam& ParamVariant::GetFloat() const
{
    Assert(m_type == PARAM_TYPE::PT_float, "Invalid union type.");
    return m_float;
}

void ParamVariant::SetFloat(float v)
{
    Assert(m_type == PARAM_TYPE::PT_float, "Invalid union type.");
    m_float.m_value = v;
    m_dlg(*this);
}

const Float2Param& ParamVariant::GetFloat2() const
{
    Assert(m_type == PARAM_TYPE::PT_float2, "Invalid union type.");
    return m_float2;
}

void ParamVariant::SetFloat2(const float2& v)
{
    Assert(m_type == PARAM_TYPE::PT_float2, "Invalid union type.");
    m_float2.m_value = v;

    if (m_float2.m_keepNormalized)
        m_float2.m_value.normalize();

    m_dlg(*this);
}

const Float3Param& ParamVariant::GetFloat3() const
{
    Assert(m_type == PARAM_TYPE::PT_float3 || m_type == PARAM_TYPE::PT_color, "Invalid union type.");
    return m_float3;
}

void ParamVariant::SetFloat3(const float3& v)
{
    Assert(m_type == PARAM_TYPE::PT_float3, "Invalid union type.");
    m_float3.m_value = v;

    if (m_float3.m_keepNormalized)
        m_float3.m_value.normalize();

    m_dlg(*this);
}

const UnitDirParam& ParamVariant::GetUnitDir() const
{
    Assert(m_type == PARAM_TYPE::PT_unit_dir, "Invalid union type.");
    return m_unitDir;
}

void ParamVariant::SetUnitDir(float pitch, float yaw)
{
    Assert(m_type == PARAM_TYPE::PT_unit_dir, "Invalid union type.");
    m_unitDir.m_pitch = pitch;
    m_unitDir.m_yaw = yaw;
    m_dlg(*this);
}

const Float3Param& ParamVariant::GetColor() const
{
    Assert(m_type == PARAM_TYPE::PT_color, "Invalid union type.");
    return m_float3;
}

void ParamVariant::SetColor(const float3& v)
{
    Assert(m_type == PARAM_TYPE::PT_color, "Invalid union type.");
    m_float3.m_value = v;
    m_dlg(*this);
}

const IntParam& ParamVariant::GetInt() const
{
    Assert(m_type == PARAM_TYPE::PT_int, "Invalid union type.");
    return m_int;
}

void ParamVariant::SetInt(int v)
{
    Assert(m_type == PARAM_TYPE::PT_int, "Invalid union type.");
    m_int.m_value = v;
    m_dlg(*this);
}

bool ParamVariant::GetBool() const
{
    Assert(m_type == PARAM_TYPE::PT_bool, "Invalid union type.");
    return m_bool;
}

void ParamVariant::SetBool(bool v)
{
    Assert(m_type == PARAM_TYPE::PT_bool, "Invalid union type.");
    m_bool = v;
    m_dlg(*this);
}

const EnumParam& ParamVariant::GetEnum() const
{
    Assert(m_type == PARAM_TYPE::PT_enum, "Invalid union type.");
    return m_enum;
}

void ParamVariant::SetEnum(int v)
{
    Assert(m_type == PARAM_TYPE::PT_enum, "Invalid union type.");
    Assert(v < m_enum.m_num, "Out-of-bound index.");
    m_enum.m_curr = v;
    m_dlg(*this);
}

float3 UnitDirParam::GetDir() const
{
    return SphericalToCartesian(m_pitch, m_yaw);
}
