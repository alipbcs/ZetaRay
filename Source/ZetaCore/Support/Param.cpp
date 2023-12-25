#include "Param.h"
#include "../Utility/Error.h"
#include <xxHash/xxhash.h>
#include <string.h>

using namespace ZetaRay::Support;
using namespace ZetaRay::Math;

void ParamVariant::InitCommon(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg)
{
    Assert(group, "group can't be null");
    Assert(subgroup, "subgroup can't be null");
    Assert(name, "name can't be null");

    m_dlg = dlg;

    size_t lenGroup = Math::Min((int)strlen(group), (MAX_GROUP_LEN - 1));
    Assert(lenGroup >= 1, "zero-length string");
    memcpy(m_group, group, lenGroup);
    m_group[lenGroup] = '\0';

    size_t lenSubgroup = Math::Min((int)strlen(subgroup), (MAX_SUBGROUP_LEN - 1));
    Assert(lenSubgroup >= 1, "zero-length string");
    memcpy(m_subgroup, subgroup, lenSubgroup);
    m_subgroup[lenSubgroup] = '\0';

    size_t lenName = Math::Min((int)strlen(name), (MAX_NAME_LEN - 1));
    Assert(lenName >= 1, "zero-length string");
    memcpy(m_name, name, lenName);
    m_name[lenName] = '\0';

    constexpr int BUFF_SIZE = ParamVariant::MAX_GROUP_LEN + ParamVariant::MAX_SUBGROUP_LEN + ParamVariant::MAX_NAME_LEN;
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
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float val, float min, float max, float step)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float;
    m_float.Init(val, min, max, step);
}

void ParamVariant::InitInt(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, int val, int min, int max, int step)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_int;
    m_int.Init(val, min, max, step);
}

void ParamVariant::InitFloat3(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, Math::float3 val, float min, float max, float step)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float3;
    m_float3.Init(val, min, max, step, false);
}

void ParamVariant::InitUnitDir(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, float pitch, float yaw)
{
    InitCommon(group, subgroup, name, dlg);

    Assert(pitch >= 0 && pitch <= Math::PI, "pitch must be in [0, +PI]");
    Assert(yaw >= 0 && yaw <= Math::TWO_PI, "yaw must be in [0, 2 * PI]");
    m_type = PARAM_TYPE::PT_unit_dir;
    m_unitDir.Init(pitch, yaw);
}

void ParamVariant::InitUnitDir(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, Math::float3 dir)
{
    InitCommon(group, subgroup, name, dlg);

    float theta;
    float phi;
    Math::SphericalFromCartesian(dir, theta, phi);

    m_type = PARAM_TYPE::PT_unit_dir;
    m_unitDir.Init(theta, phi);
}

void ParamVariant::InitNormalizedFloat3(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, Math::float3 val)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_float3;
    m_float3.Init(val, -1.0f, 1.0f, 1e-2f, true);
}

void ParamVariant::InitColor(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, Math::float3 val)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_color;
    m_float3.Init(val, 0.0f, 1.0f, 0.01f, false);
}

void ParamVariant::InitBool(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, bool val)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_bool;
    m_bool = val;
}

void ParamVariant::InitEnum(const char* group, const char* subgroup, const char* name, 
    fastdelegate::FastDelegate1<const ParamVariant&> dlg, const char** vals, int num, int idx)
{
    InitCommon(group, subgroup, name, dlg);

    m_type = PARAM_TYPE::PT_enum;
    Assert(idx < num, "invalid args");
    m_enum.Init(vals, num, idx);
}

const FloatParam& ParamVariant::GetFloat() const
{
    Assert(m_type == PARAM_TYPE::PT_float, "invalid args");
    return m_float;
}

void ParamVariant::SetFloat(float v)
{
    Assert(m_type == PARAM_TYPE::PT_float, "invalid args");
    m_float.m_val = v;
    m_dlg(*this);
}

const Float3Param& ParamVariant::GetFloat3() const
{
    Assert(m_type == PARAM_TYPE::PT_float3 || m_type == PARAM_TYPE::PT_color, "invalid args");
    return m_float3;
}

void ParamVariant::SetFloat3(Math::float3 v)
{
    Assert(m_type == PARAM_TYPE::PT_float3, "invalid args");

    if (m_float3.m_keepNormalized)
        v.normalize();

    m_float3.m_val = v;
    m_dlg(*this);
}

const UnitDirParam& ParamVariant::GetUnitDir() const
{
    Assert(m_type == PARAM_TYPE::PT_unit_dir, "invalid args");
    return m_unitDir;
}

void ParamVariant::SetUnitDir(float pitch, float yaw)
{
    Assert(m_type == PARAM_TYPE::PT_unit_dir, "invalid args");
    m_unitDir.m_pitch = pitch;
    m_unitDir.m_yaw = yaw;
    m_dlg(*this);
}

const Float3Param& ParamVariant::GetColor() const
{
    Assert(m_type == PARAM_TYPE::PT_color, "invalid args");
    return m_float3;
}

void ParamVariant::SetColor(Math::float3 v)
{
    Assert(m_type == PARAM_TYPE::PT_color, "invalid args");
    m_float3.m_val = v;
    m_dlg(*this);
}

const IntParam& ParamVariant::GetInt() const
{
    Assert(m_type == PARAM_TYPE::PT_int, "invalid args");
    return m_int;
}

void ParamVariant::SetInt(int v)
{
    Assert(m_type == PARAM_TYPE::PT_int, "invalid args");
    m_int.m_val = v;
    m_dlg(*this);
}

bool ParamVariant::GetBool() const
{
    Assert(m_type == PARAM_TYPE::PT_bool, "invalid args");
    return m_bool;
}

void ParamVariant::SetBool(bool v)
{
    Assert(m_type == PARAM_TYPE::PT_bool, "invalid args");
    m_bool = v;
    m_dlg(*this);
}

const EnumParam& ParamVariant::GetEnum() const
{
    Assert(m_type == PARAM_TYPE::PT_enum, "invalid args");
    return m_enum;
}

void ParamVariant::SetEnum(int v)
{
    Assert(m_type == PARAM_TYPE::PT_enum, "invalid args");
    Assert(v < m_enum.m_num, "invalid index into enum values");
    m_enum.m_curr = v;
    m_dlg(*this);
}

float3 UnitDirParam::GetDir()
{
    return Math::SphericalToCartesian(m_pitch, m_yaw);
}
