#pragma once

#include "../Math/Vector.h"
#include <type_traits>
#include <FastDelegate/FastDelegate.h>

namespace ZetaRay::Support
{
	//--------------------------------------------------------------------------------------
	// FloatParam
	//--------------------------------------------------------------------------------------

	struct FloatParam
	{
		void Init(float val, float min, float max, float step) noexcept
		{
			m_val = val;
			m_min = min;
			m_max = max;
			m_stepSize = step;
		}

		float m_val;
		float m_min;
		float m_max;
		float m_stepSize;
	};

	//--------------------------------------------------------------------------------------
	// Float3Param
	//--------------------------------------------------------------------------------------

	struct Float3Param
	{
		void Init(Math::float3 val, float min, float max, float step, bool keepNormalized) noexcept
		{
			m_val = val;
			m_min = min;
			m_max = max;
			m_stepSize = step;
			m_keepNormalized = keepNormalized;
		}

		Math::float3 m_val;
		float m_min;
		float m_max;
		float m_stepSize;
		bool m_keepNormalized;
	};

	//--------------------------------------------------------------------------------------
	// UnitDirParam
	//--------------------------------------------------------------------------------------

	struct UnitDirParam
	{
		void Init(float pitch, float yaw) noexcept
		{
			m_pitch = pitch;
			m_yaw = yaw;
		}

		Math::float3 GetDir() noexcept;

		float m_pitch;		// angle of rotation around the x-axis (radians)
		float m_yaw;		// angle of rotation around the y-axis (radians)
	};

	//--------------------------------------------------------------------------------------
	// ColorParam
	//--------------------------------------------------------------------------------------

	struct ColorParam
	{
		void Init(Math::float3 val, float min, float max, float step) noexcept
		{
			m_val = val;
			m_min = min;
			m_max = max;
			m_stepSize = step;
		}

		Math::float3 m_val;
		float m_min;
		float m_max;
		float m_stepSize;
	};

	//--------------------------------------------------------------------------------------
	// IntParam
	//--------------------------------------------------------------------------------------

	struct IntParam
	{
		void Init(int val, int min, int max, int step) noexcept
		{
			m_val = val;
			m_min = min;
			m_max = max;
			m_stepSize = step;
		}

		int m_val;
		int m_min;
		int m_max;
		int m_stepSize;
	};

	//--------------------------------------------------------------------------------------
	// EnumParam
	//--------------------------------------------------------------------------------------

	struct EnumParam
	{
		void Init(const char** vals, int n, int curr) noexcept
		{
			m_vals = vals;
			m_num = n;
			m_curr = curr;
		}

		const char** m_vals;
		int m_num;
		int m_curr;
	};

	static_assert(std::is_trivial_v<FloatParam>);
	static_assert(std::is_trivial_v<Float3Param>);
	static_assert(std::is_trivial_v<UnitDirParam>);
	static_assert(std::is_trivial_v<ColorParam>);
	static_assert(std::is_trivial_v<IntParam>);
	static_assert(std::is_trivial_v<EnumParam>);


	//--------------------------------------------------------------------------------------
	// ParamVariant
	//--------------------------------------------------------------------------------------

	enum class PARAM_TYPE
	{
		PT_float,
		PT_float2,
		PT_float3,
		PT_unit_dir,
		PT_color,
		PT_int,
		PT_bool,
		PT_enum
	};

	struct ParamVariant
	{
		static const int MAX_NAME_LEN = 32;
		static const int MAX_GROUP_LEN = 16;
		static const int MAX_SUBGROUP_LEN = 24;

		void InitFloat(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			float val, float min, float max, float step) noexcept;

		void InitInt(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			int val, int min, int max, int step) noexcept;

		void InitFloat3(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			Math::float3 val, float min, float max, float step) noexcept;

		void InitUnitDir(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			float pitch, float yaw) noexcept;		
		
		void InitUnitDir(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			Math::float3 dir) noexcept;

		void InitNormalizedFloat3(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			Math::float3 val) noexcept;

		void InitColor(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg,
			Math::float3 val) noexcept;

		void InitBool(const char* group, const char* subgroup, const char* name, 
			fastdelegate::FastDelegate1<const ParamVariant&> dlg, bool val) noexcept;

		void InitEnum(const char* group, const char* subgroup, const char* name,
			fastdelegate::FastDelegate1<const ParamVariant&> dlg, const char** enumVals, 
			int num, int curr) noexcept;

		const char* GetGroup() const { return m_group; }
		const char* GetSubGroup() const { return m_subgroup; }
		const char* GetName() const { return m_name; }
		PARAM_TYPE GetType() const { return m_type; }
		uint64_t GetID() const { return m_id; }

		const FloatParam& GetFloat() const noexcept;
		void SetFloat(float v) noexcept;

		const Float3Param& GetFloat3() const noexcept;
		void SetFloat3(Math::float3 v) noexcept;		
		
		const UnitDirParam& GetUnitDir() const noexcept;
		void SetUnitDir(float pitch, float yaw) noexcept;

		const Float3Param& GetColor() const noexcept;
		void SetColor(Math::float3 v) noexcept;

		const IntParam& GetInt() const noexcept;
		void SetInt(int v) noexcept;

		bool GetBool() const;
		void SetBool(bool v) noexcept;

		const EnumParam& GetEnum() const noexcept;
		void SetEnum(int v) noexcept;

	private:
		void InitCommon(const char* group, const char* subgroup, const char* name, fastdelegate::FastDelegate1<const ParamVariant&> dlg) noexcept;

		fastdelegate::FastDelegate1<const ParamVariant&> m_dlg;
		uint64_t m_id;
		PARAM_TYPE m_type;
		char m_group[MAX_GROUP_LEN];
		char m_subgroup[MAX_SUBGROUP_LEN];
		char m_name[MAX_NAME_LEN];

		union
		{
			FloatParam m_float;
			Float3Param m_float3;
			UnitDirParam m_unitDir;
			IntParam m_int;
			EnumParam m_enum;
			bool m_bool;
		};

	};
}
