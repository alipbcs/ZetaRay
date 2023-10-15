#pragma once

#include <Utility/Error.h>
#include <type_traits>

namespace ZetaRay::Util
{
	template<typename T>
	struct Optional
	{
		Optional() = default;
		~Optional()
		{
			reset();
		}

		Optional(const T& v)
			requires std::is_copy_constructible_v<T>
			: m_hasValue(true)
		{
			new (reinterpret_cast<T*>(m_value)) T(v);
		}
		Optional& operator=(const T& other)
			requires std::is_move_assignable_v<T>
		{
			reset();
			new (reinterpret_cast<T*>(m_value)) T(other);
			m_hasValue = true;

			return *this;
		}
		Optional(T&& v)
			requires std::is_move_constructible_v<T>
			: m_hasValue(true)
		{
			new (reinterpret_cast<T*>(m_value)) T(ZetaMove(v));
		}
		Optional& operator=(T&& other)
			requires std::is_move_assignable_v<T>
		{
			reset();
			new (reinterpret_cast<T*>(m_value)) T(ZetaMove(other));
			m_hasValue = true;

			return *this;
		}
		Optional(const Optional<T>& other)
			: m_hasValue(other.m_hasValue)
		{
			if (other.m_hasValue)
				new (reinterpret_cast<T*>(m_value)) T(other.value());
		}
		Optional& operator=(const Optional<T>& other)
		{
			reset();

			if (other.m_hasValue)
			{
				new (reinterpret_cast<T*>(m_value)) T(other.value());
				m_hasValue = true;
			}

			return *this;
		}
		Optional(Optional<T>&& other)
			: m_hasValue(other.m_hasValue)
		{
			if (other.m_hasValue)
			{
				new (reinterpret_cast<T*>(m_value)) T(ZetaMove(other.value()));
				other.reset();
			}
		}
		Optional& operator=(Optional<T>&& other)
		{
			reset();

			if (other.m_hasValue)
			{
				new (reinterpret_cast<T*>(m_value)) T(ZetaMove(other.value()));
				m_hasValue = true;
				other.reset();
			}

			return *this;
		}

		ZetaInline explicit operator bool() const { return m_hasValue; }

		void reset()
		{
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				if (m_hasValue)
					value().~T();
			}

			m_hasValue = false;
		}

		// only checks in debug build
		ZetaInline T& value()
		{
			Assert(m_hasValue, "Optional is empty.");
			return *reinterpret_cast<T*>(m_value);
		}
		ZetaInline const T& value() const
		{
			Assert(m_hasValue, "Optional is empty.");
			return *reinterpret_cast<const T*>(m_value);
		}

		// safe version, always checks
		ZetaInline T& value_s()
		{
			Check(m_hasValue, "Optional is empty.");
			return *reinterpret_cast<T*>(m_value);
		}
		ZetaInline const T& value_s() const
		{
			Check(m_hasValue, "Optional is empty.");
			return *reinterpret_cast<const T*>(m_value);
		}

	private:
		alignas(T) uint8_t m_value[sizeof(T)];
		bool m_hasValue = false;
	};
}