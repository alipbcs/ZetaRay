#pragma once

#include "../Math/Common.h"
#include "../Support/Memory.h"
#include <utility>	// std::swap

namespace ZetaRay::Util
{
	//--------------------------------------------------------------------------------------
	// Vector
	// 
	// Vector cannot be directly constructed; it only provides a common interface for SmallVector.
	// As a result, usage doesn't require knowing the inline storage size N.
	//--------------------------------------------------------------------------------------

	template<typename T, typename Allocator = Support::SystemAllocator>
	class Vector
	{
		static_assert(Support::AllocType<Allocator>, "Allocator doesn't meet the requirements for AllocType.");
		static_assert(std::is_copy_constructible_v<Allocator>, "Allocator must be copy-constructible.");
		static constexpr size_t MIN_CAPACITY = Math::Max(64 / sizeof(T), 4llu);

	public:
		~Vector() noexcept
		{
			free_memory();
		}

		bool has_inline_storage() const noexcept
		{
			constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
			return reinterpret_cast<uintptr_t>(m_beg) == reinterpret_cast<uintptr_t>(this) + inlineStorageOffset;
		}

		void swap(Vector& other) noexcept
		{
			if (this == &other)
				return;

			if (empty() && other.empty())
				return;

			if (!has_inline_storage() && !other.has_inline_storage())
			{
				std::swap(m_beg, other.m_beg);
				std::swap(m_end, other.m_end);
				std::swap(m_last, other.m_last);

				return;
			}

			const size_t oldSize = size();
			const size_t oldOtherSize = other.size();
			const bool otherIsLarger = oldOtherSize > oldSize;
			const size_t minSize = Math::Min(oldSize, oldOtherSize);
			const size_t maxSize = Math::Max(oldSize, oldOtherSize);

			if(!other.empty())
				reserve(oldOtherSize);		// doesn't allocate if inline storage happens to be large enough
	
			if(!empty())
				other.reserve(oldSize);		// doesn't allocate if inline storage happens to be large enough

			T* largerBeg = otherIsLarger ? other.m_beg : m_beg;
			T* smallerBeg = otherIsLarger ? m_beg : other.m_beg;

			for (size_t i = 0; i < minSize; i++)
				std::swap(*(m_beg + i), *(other.m_beg + i));

			// copy over the remaining elements to the smaller Vector
			if constexpr (std::is_trivially_copyable_v<T>)
			{
				memcpy(smallerBeg + minSize, largerBeg + minSize, sizeof(T) * (maxSize - minSize));
			}
			else if constexpr (std::is_move_constructible_v<T>)
			{
				for (size_t i = minSize; i < maxSize; i++)
					new (smallerBeg + i) T(ZetaMove(*(largerBeg + i)));
			}
			else if constexpr (std::is_copy_constructible_v<T>)
			{
				for (size_t i = minSize; i < maxSize; i++)
					new (smallerBeg + i) T(*(largerBeg + i));
			}

			m_end = m_beg + oldOtherSize;
			other.m_end = other.m_beg + oldSize;
		}

		ZetaInline T* begin() noexcept
		{
			return m_beg;
		}

		ZetaInline T* end() noexcept
		{
			return m_end;
		}

		ZetaInline const T* begin() const noexcept
		{
			return m_beg;
		}

		ZetaInline const T* end() const noexcept
		{
			return m_end;
		}

		ZetaInline const T* cbegin() const noexcept
		{
			return m_beg;
		}

		ZetaInline const T* cend() const noexcept
		{
			return m_end;
		}

		ZetaInline T* data() noexcept
		{
			return m_beg;
		}

		ZetaInline T& back() noexcept
		{
			Assert(size() > 0, "Vector is empty");
			return *(m_beg + size() - 1);
		}

		ZetaInline const T& back() const noexcept
		{
			Assert(size() > 0, "Vector is empty");
			return *(m_beg + size() - 1);
		}

		ZetaInline T& operator[](size_t pos) noexcept
		{
			Assert(pos < (uintptr_t)(m_end - m_beg), "Out-of-bound access.");
			return *(m_beg + pos);
		}

		ZetaInline const T& operator[](size_t pos) const noexcept
		{
			Assert(pos < (uintptr_t)(m_end - m_beg), "Out-of-bound access.");
			return *(m_beg + pos);
		}

		ZetaInline size_t capacity() const noexcept
		{
			return m_last - m_beg;
		}

		ZetaInline size_t size() const noexcept
		{
			return m_end - m_beg;
		}

		ZetaInline bool empty() const noexcept
		{
			return m_end - m_beg == 0;
		}

		void reserve(size_t n) noexcept
		{
			const size_t currCapacity = capacity();
			const size_t oldSize = size();

			if (n <= currCapacity)
				return;

			void* mem = relocate(n);

			// adjust the pointers
			m_beg = reinterpret_cast<T*>(mem);
			m_end = m_beg + oldSize;
			m_last = m_beg + n;
		}

		void resize(size_t n) noexcept
		{
			const size_t currCapacity = capacity();
			const size_t oldSize = size();

			// just adjust the "end" pointer
			if (n <= currCapacity)
			{
				m_end = m_beg + n;
				return;
			}

			void* mem = relocate(n);

			// adjust the pointers
			m_beg = reinterpret_cast<T*>(mem);
			m_end = m_beg + n;
			m_last = m_beg + n;

			// default construct the newly added elements
			if constexpr (!std::is_trivially_default_constructible_v<T>)
			{
				T* curr = m_beg + oldSize;

				while (curr != m_end)
				{
					new (curr) T;
					curr++;
				}
			}
		}

		void resize(size_t n, const T& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T cannot be copy or move constructed.");

			const size_t oldSize = size();
			void* mem = relocate(n);

			// adjust the pointers
			m_beg = reinterpret_cast<T*>(mem);
			m_end = m_beg + n;
			m_last = m_beg + n;

			T* curr = m_beg + oldSize;

			// copy/move construct the new elements
			while (curr != m_end)
			{
				new (curr) T(ZetaForward(val));
				curr++;
			}
		}

		void pop_back() noexcept
		{
			Assert(size() > 0, "attempting to pop from an empty Vector");

			if constexpr (!std::is_trivially_destructible_v<T>)
				(m_end - 1)->~T();

			m_end--;
		}

		void push_back(const T& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");
			emplace_back(val);
		}

		void push_back(T&& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");
			emplace_back(ZetaMove(val));
		}

		template<typename... Args>
		void emplace_back(Args&&... args) noexcept
		{
			if (m_last == m_end)
			{
				const size_t prevCapacity = capacity();
				const size_t newCapacity = Math::Max(MIN_CAPACITY, prevCapacity + (prevCapacity >> 1));
				Assert(newCapacity > prevCapacity, "Capacity must strictly increase.");

				reserve(newCapacity);
			}

			new (reinterpret_cast<void*>(m_end)) T(ZetaForward(args)...);
			m_end++;
		}

		void append_range(const T* beg, const T* end, bool exact = false) noexcept
		{
			if (!beg || !end || beg == end || beg > end)
				return;

			const size_t num = end - beg;
			const size_t oldSize = size();
			const size_t currCapacity = capacity();

			if (currCapacity < oldSize + num)
			{
				size_t newCapacity = oldSize + num;
				newCapacity = !exact ? Math::Max(MIN_CAPACITY, newCapacity + (newCapacity >> 1)) : 
					Math::Max(MIN_CAPACITY, newCapacity);

				reserve(newCapacity);
			}

			if constexpr (std::is_trivially_constructible_v<T>)
			{
				memcpy(m_beg + oldSize, const_cast<T*>(beg), sizeof(T) * num);
			}
			else if constexpr (std::is_move_constructible_v<T>)
			{
				T* source = const_cast<T*>(beg);
				T* target = m_beg + oldSize;

				while (source != end)
				{
					new (target) T(ZetaMove(*source));

					source++;
					target++;
				}
			}
			else if constexpr (std::is_copy_constructible_v<T>)
			{
				T* source = const_cast<T*>(beg);
				T* target = m_beg + oldSize;

				while (source != end)
				{
					new (target) T(*source);

					source++;
					target++;
				}
			}
			else
				Assert(false, "calling reserve() for a non-copyable and non-movable T when Vector is non-empty is invalid.");

			m_end += num;
		}

		// Erases an element by swapping it with the last element. Returns a pointer to the next element.
		T* erase(size_t pos) noexcept
		{
			static_assert(std::is_swappable_v<T>, "T is not swappable");
			const size_t n = size();
			Assert(pos < n, "Out-of-bound access.");
			Assert(!empty(), "attempting to erase from an empty Vector");
			Assert(pos < n, "invalid arg");

			if (pos == n - 1)
			{
				pop_back();
				return m_beg + pos;
			}

			std::swap(*(m_beg + pos), *(m_beg + n - 1));
			pop_back();

			return m_beg + pos;
		}

		// Erases an element by swapping it with the last element. Returns a pointer to the next element.
		T* erase(T& item) noexcept
		{
			static_assert(std::is_swappable_v<T>, "T is not swappable");
			const size_t n = size();
			const size_t pos = &item - m_beg;
			Assert(!empty(), "attempting to erase from an empty Vector");
			Assert(pos >= 0 && pos < n, "Out-of-bound access.");

			if (pos == n - 1)
			{
				pop_back();
				return m_beg + pos;
			}

			std::swap(*(m_beg + pos), *(m_beg + n - 1));
			pop_back();

			return m_beg + pos;
		}

		void push_front(const T& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");
			static_assert(std::is_swappable_v<T>, "T is not swappable");

			emplace_back(val);

			const size_t n = size();

			if (n > 1)
				std::swap(*m_beg, *(m_beg + n - 1));
		}

		void push_front(T&& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");
			static_assert(std::is_swappable_v<T>, "T is not swappable");

			emplace_back(ZetaMove(val));

			const size_t n = size();

			if (n > 1)
				std::swap(*m_beg, *(m_beg + n - 1));
		}

		void clear() noexcept
		{
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				T* curr = m_beg;

				while (curr != m_end)
					curr++->~T();
			}

			m_end = m_beg;
		}

		void free_memory() noexcept
		{
			clear();

			size_t currCapacity = capacity();

			// free the previously allocated memory
			if (currCapacity && !has_inline_storage())
			{
				m_allocator.FreeAligned(m_beg, currCapacity * sizeof(T), alignof(T));

				// TODO these should reset to pointing to the inline storage
				m_beg = nullptr;
				m_end = nullptr;
				m_last = nullptr;
			}
		}

	protected:
		Vector(const Allocator& a) noexcept
			: m_allocator(a)
		{}

		Vector(size_t N, const Allocator& a) noexcept
			: m_allocator(a)
		{
			static_assert(std::is_default_constructible_v<T>, "T cannot be default-constructed.");

			constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
			m_beg = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + inlineStorageOffset);
			m_end = m_beg;
			m_last = m_beg + N;

			if constexpr (!std::is_trivially_default_constructible_v<T>)
			{
				T* curr = m_beg;

				while (curr != m_last)
				{
					new (curr) T;
					curr++;
				}
			}
		}

		Vector(size_t N, const T& t, const Allocator& a) noexcept
			: m_allocator(a)
		{
			static_assert(std::is_copy_constructible_v<T>, "T cannot be copy-constructed.");

			constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
			m_beg = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + inlineStorageOffset);
			m_end = m_beg;
			m_last = m_beg + N;

			T* curr = m_beg;

			while (curr != m_last)
			{
				new (curr) T(t);
				curr++;
			}
		}

		Vector& operator=(const Vector& other) noexcept
		{
			static_assert(std::is_copy_assignable_v<T>, "T cannot be copy-assigned.");
			static_assert(std::is_copy_assignable_v<Allocator>, "Allocator cannot be copy-assigned.");

			if (this == &other)
				return *this;

			m_allocator = other.m_allocator;

			const size_t currSize = size();
			const size_t currCapacity = capacity();
			const size_t newSize = other.size();

			// destruct old elements
			clear();

			if (newSize == 0)
				return *this;

			if (currCapacity < newSize)
			{
				// free the previously allocated memory
				if (currCapacity > 0 && !has_inline_storage())
					m_allocator.FreeAligned(m_beg, currCapacity * sizeof(T), alignof(T));

				// allocate memory to accomodate new size
				void* mem = m_allocator.AllocateAligned(newSize * sizeof(T), alignof(T));

				// adjust the pointers
				m_beg = reinterpret_cast<T*>(mem);
				m_last = m_beg + newSize;
			}

			// regardless of whether memory was allocated
			m_end = m_beg + newSize;

			// just copy the memory
			if constexpr (std::is_trivially_copyable_v<T>)
			{
				memcpy(m_beg, other.m_beg, sizeof(T) * newSize);
			}
			// call the copy constructor
			else
			{
				T* source = m_beg;
				T* target = other.m_beg;

				for (size_t i = 0; i < newSize; i++)
				{
					new (source) T(*target);

					source++;
					target++;
				}
			}

			return *this;
		}

		Vector& operator=(Vector&& other) noexcept
		{
			static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
				"T cannot be copy or move assigned.");
			static_assert(std::is_move_assignable_v<Allocator> || std::is_copy_assignable_v<Allocator>,
				"Allocator cannot be copy or move assigned.");

			if (this == &other)
				return *this;

			m_allocator = ZetaForward(other.m_allocator);

			// previously allocated memory is not needed anymore and can be released
			free_memory();

			// just switch pointers when:
			// 1. both Vectors are using the heap
			// 2. MovedFrom is using the heap AND MovedTo's inline storage isn't large enough
			if (((capacity() < other.size()) || !has_inline_storage()) && !other.has_inline_storage())
			{
				m_beg = other.m_beg;
				m_end = other.m_end;
				m_last = other.m_last;

				other.m_beg = nullptr;
				other.m_end = nullptr;
				other.m_last = nullptr;
			}
			else if(!other.empty())
			{
				// doesn't allocate if inline storage happens to be large enough
				reserve(other.size());

				if constexpr (std::is_trivially_copyable_v<T>)
				{
					memcpy(m_beg, other.m_beg, sizeof(T) * other.size());
				}
				else if constexpr (std::is_move_constructible_v<T>)
				{
					T* source = other.m_beg;
					T* target = this->m_beg;
					const T* end = target + other.size();

					while (target != end)
					{
						new (target) T(ZetaMove(*source));

						source++;
						target++;
					}
				}
				else if constexpr (std::is_copy_constructible_v<T>)
				{
					T* source = other.m_beg;
					T* target = this->m_beg;
					const T* end = target + other.size();

					while (target != end)
					{
						new (target) T(*source);

						source++;
						target++;
					}
				}

				// adjust the pointers
				m_end = m_beg + other.size();
				Assert(size() == other.size(), "these must be equal");
				
				other.clear();
			}

			return *this;
		}

		void* relocate(size_t n) noexcept
		{
			// allocate memory to accomodate the new size
			void* mem = m_allocator.AllocateAligned(n * sizeof(T), alignof(T));
			const size_t oldSize = size();

			// copy over the old elements
			if (oldSize > 0)
			{
				if constexpr (std::is_trivially_copyable_v<T>)
				{
					// TODO overlap leads to undefined behavior
					memcpy(mem, m_beg, sizeof(T) * oldSize);
				}
				else if constexpr (std::is_move_constructible_v<T>)
				{
					T* source = m_beg;
					T* target = reinterpret_cast<T*>(mem);
					const T* end = target + oldSize;

					while (target != end)
					{
						new (target) T(ZetaMove(*source));

						source++;
						target++;
					}
				}
				else if constexpr (std::is_copy_constructible_v<T>)
				{
					T* source = m_beg;
					T* target = reinterpret_cast<T*>(mem);
					const T* end = target + oldSize;

					while (target != end)
					{
						new (target) T(*source);

						source++;
						target++;
					}
				}
				else
					Assert(false, "calling reserve() for a non-copyable and non-movable type T when Vector is non-empty is invalid.");

				// destruct old elements
				if constexpr (!std::is_trivially_destructible_v<T>)
				{
					for (T* curr = m_beg; curr < m_end; curr++)
						curr->~T();
				}
			}

			// free the previously allocated memory
			const size_t currCapacity = capacity();

			if (currCapacity && !has_inline_storage())
				m_allocator.FreeAligned(m_beg, currCapacity * sizeof(T), alignof(T));

			return mem;
		}

		const Allocator& GetAllocator() { return m_allocator; }

		T* m_beg = nullptr;		// pointer to the begining of memory-block
		T* m_end = nullptr;		// pointer to element to insert at next (one past the last inserted element)
		T* m_last = nullptr;	// pointer to the end of memory-block

		Allocator m_allocator;
	};

	//--------------------------------------------------------------------------------------
	// InlineStorage
	//--------------------------------------------------------------------------------------

	template<typename T, uint32_t N>
	struct InlineStorage
	{
		alignas(T) uint8_t Buffer[sizeof(T) * N];
	};

	// reminder: empty struct has size 1
	template<typename T>
	struct InlineStorage<T, 0>
	{};

	//--------------------------------------------------------------------------------------
	// SmallVector 
	// Dynamic array with inline storage to hold a static number (template argument) of elements 
	// within the object. Inspired by the following talk:
	// “High Performance Code 201: Hybrid Data Structures"
	//--------------------------------------------------------------------------------------

	constexpr uint32_t GetExcessSize(uint32_t sizeofT, uint32_t alignofT)
	{
		return Math::Max(0u, (uint32_t)Math::Min(
			(32 - Math::AlignUp(sizeof(void*) * 3, alignofT)) / sizeofT,
			(64 - Math::AlignUp(sizeof(void*) * 3, alignofT)) / sizeofT));
	}

	template<typename T, Support::AllocType Allocator = Support::SystemAllocator, uint32_t N = GetExcessSize(sizeof(T), alignof(T))>
	class SmallVector : public Vector<T, Allocator>
	{
	public:
		SmallVector(const Allocator& a = Allocator()) noexcept
			: Vector<T, Allocator>(N, a)
		{}

		SmallVector(const T& t, const Allocator& a = Allocator()) noexcept
			: Vector<T, Allocator>(N, t, a)
		{}

		SmallVector(const SmallVector& other) noexcept
			: Vector<T, Allocator>(N, this->GetAllocator())
		{
			Vector<T, Allocator>::operator=(other);
		}

		SmallVector(const Vector<T, Allocator>& other) noexcept
			: Vector<T, Allocator>(N, this->GetAllocator())
		{
			Vector<T, Allocator>::operator=(other);
		}

		SmallVector& operator=(const SmallVector& other) noexcept
		{
			Vector<T, Allocator>::operator=(other);
			return *this;
		}

		SmallVector& operator=(const Vector<T, Allocator>& other) noexcept
		{
			Vector<T, Allocator>::operator=(other);
			return *this;
		}

		SmallVector(SmallVector&& other) noexcept
			: Vector<T, Allocator>(N, this->GetAllocator())
		{
			Vector<T, Allocator>::operator=(ZetaMove(other));
		}

		SmallVector(Vector<T, Allocator>&& other) noexcept
		{
			Vector<T, Allocator>::operator=(ZetaMove(other));
		}

		SmallVector& operator=(SmallVector&& other) noexcept
		{
			Vector<T, Allocator>::operator=(ZetaMove(other));
			return *this;
		}

		SmallVector& operator=(Vector<T, Allocator>&& other) noexcept
		{
			Vector<T, Allocator>::operator=(ZetaMove(other));
			return *this;
		}

	private:
		InlineStorage<T, N> m_inlineStorage;
	};
}