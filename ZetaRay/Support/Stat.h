#pragma once

#include "../Utility/Error.h"
#include "../Math/Common.h"

namespace ZetaRay::Support
{
	struct Stat
	{
		enum class ST_TYPE
		{
			ST_INT,
			ST_UINT,
			ST_FLOAT,
			ST_UINT64,
			ST_RATIO
		};

		Stat() noexcept = default;
		Stat(const char* group, const char* name, int i) noexcept
		{
			InitCommon(group, name);
			m_type = ST_TYPE::ST_INT;
			m_int = i;
		}		
		Stat(const char* group, const char* name, uint32_t u) noexcept
		{
			InitCommon(group, name);
			m_type = ST_TYPE::ST_UINT;
			m_uint = u;
		}		
		Stat(const char* group, const char* name, float f) noexcept
		{
			InitCommon(group, name);
			m_type = ST_TYPE::ST_FLOAT;
			m_float = f;
		}		
		Stat(const char* group, const char* name, uint64_t u) noexcept
		{
			InitCommon(group, name);
			m_type = ST_TYPE::ST_UINT64;
			m_uint64 = u;
		}
		Stat(const char* group, const char* name, uint32_t u, uint32_t total) noexcept
		{
			InitCommon(group, name);
			m_type = ST_TYPE::ST_RATIO;
			m_uint64 = ((uint64_t)u << 32) | total;
		}

		const char* GetGroup() { return m_group; }
		const char* GetName() { return m_name; }
		ST_TYPE GetType() { return m_type; }

		int GetInt() noexcept
		{
			Assert(m_type == ST_TYPE::ST_INT, "invalid type");
			return m_int;
		}

		uint32_t GetUInt() noexcept
		{
			Assert(m_type == ST_TYPE::ST_UINT, "invalid type");
			return m_uint;
		}

		float GetFloat() noexcept
		{
			Assert(m_type == ST_TYPE::ST_FLOAT, "invalid type");
			return m_float;
		}

		uint64_t GetUInt64() noexcept
		{
			Assert(m_type == ST_TYPE::ST_UINT64, "invalid type");
			return m_uint64;
		}

		void GetRatio(uint32_t& num, uint32_t& total) noexcept
		{
			Assert(m_type == ST_TYPE::ST_RATIO, "invalid type");
			num = m_uint64 >> 32;
			total = m_uint64 & 0xffffffff;
		}

	private:
		void InitCommon(const char* group, const char* name) noexcept
		{
			Assert(group, "group can't be NULL");
			Assert(name, "name can't be NULL");

			auto ng = Math::min(GROUP_LEN - 1, strlen(group));
			auto nn = Math::min(NAME_LEN - 1, strlen(name));

			memcpy(m_group, group, ng);
			m_group[ng] = '\0';

			memcpy(m_name, name, nn);
			m_name[nn] = '\0';

			//char temp[GROUP_LEN + NAME_LEN];
			//memcpy(temp, m_group, ng);
			//memcpy(temp + ng, m_name, nn);

			//m_id = XXH3_64bits(temp, ng + nn);
		}

		static constexpr size_t GROUP_LEN = 16;
		static constexpr size_t NAME_LEN = 16;

		char m_group[16];
		char m_name[20];
		ST_TYPE m_type;

		union
		{
			int m_int;
			uint32_t m_uint;
			float m_float;
			uint64_t m_uint64;
		};
	};
}