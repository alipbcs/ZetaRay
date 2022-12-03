#pragma once

#include "../Math/Common.h"
#include "../App/App.h"

namespace ZetaRay::Util
{
	// Open-set addressing with linear probing
	// 
	//  - Assumes keys are already hashed; key itself is not stored, only its hash (uint64_t). Consequently,
	//	  collisions on keys could lead to wrong results. By using a decent hash function, chances of 
	//    such collisions should be low.
	//  - Iterators (pointers) are NOT stable; pointer to an entry found earlier might not be valid
	//	  anymore due to subsequent insertions and possible resize.
	//  - Not thread-safe
	template<typename T>
	class HashTable
	{
		static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");

	public:

		struct Entry
		{
			uint64_t Key;
			T Val;
		};

		HashTable() noexcept = default;
		explicit HashTable(size_t initaliSize) noexcept
		{
			static_assert(std::is_default_constructible_v<T>);
			resize(initaliSize);
		}

		// returns NULL if element with given key is not found
		// Note: in contrast to find(), find_entry() only return NULL when the table is empty
		T* find(uint64_t key) noexcept
		{
			Entry* e = find_entry(key);
			if (e && e->Key != NULL_KEY)
				return &e->Val;

			return nullptr;
		}

		template<typename... Args>
		Entry& emplace_or_assign(uint64_t key, Args&&... args) noexcept
		{
			Assert(key != NULL_KEY, "Invalid key");
			const size_t numBuckets = bucket_count();
			const float load = load_factor();

			if (!m_beg || load >= MAX_LOAD)
				resize(Math::max(numBuckets << 1, MIN_NUM_BUCKETS));

			Entry* elem = find_entry(key);
			if (elem->Key == NULL_KEY)
			{
				m_numEntries++;

				if constexpr (!std::is_trivially_destructible_v<T>)
					elem->Val.~T();
			}

			elem->Key = key;
			new (&elem->Val) T(ZetaForward(args)...);

			return *elem;
		}

		inline size_t bucket_count() const noexcept
		{
			return m_end - m_beg;
		}

		inline size_t size() const noexcept
		{
			return m_numEntries;
		}		
		
		inline float load_factor() const noexcept
		{
			// necessary to avoid divide-by-zero
			if (empty())
				return 0.0f;

			return (float)m_numEntries / bucket_count();
		}

		inline bool empty() const noexcept
		{
			return m_end - m_beg == 0;
		}

		void clear() noexcept
		{
			Entry* curr = m_beg;

			while (curr != m_end)
			{
				if constexpr (!std::is_trivially_destructible_v<T>)
					curr->~Entry();

				curr++->Key = NULL_KEY;
			}
			
			m_numEntries = 0;
			// don't free the memory
		}

		void free() noexcept
		{
			Entry* curr = m_beg;

			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				while (curr != m_end)
				{
					curr++->~Entry();
				}
			}

			m_numEntries = 0;

			// free the previously allocated memory
			if(bucket_count())
				App::FreeMemoryPool(m_beg, bucket_count() * sizeof(Entry), alignof(Entry));
		}

		inline void swap(HashTable& other) noexcept
		{
			std::swap(m_beg, other.m_beg);
			std::swap(m_end, other.m_end);
			std::swap(m_numEntries, other.m_numEntries);
		}

		inline T& operator[](uint64_t key) noexcept
		{
			static_assert(std::is_default_constructible_v<T>, "T must be default-constructible");

			Entry* e = find_entry(key);
			if (!e || e->Key == NULL_KEY)
				return emplace_or_assign(key).Val;

			return e->Val;
		}

		inline Entry* begin_it() noexcept
		{
			return m_beg;
		}

		inline Entry* next_it(Entry* curr) noexcept
		{
			Entry* next = curr + 1;
			while (next != m_end && next->Key == NULL_KEY)
				next++;
			
			return next;
		}

		inline Entry* end_it() noexcept
		{
			return m_end;
		}

	private:
		Entry* find_entry(uint64_t key) noexcept
		{
			const size_t n = bucket_count();
			if (n == 0)
				return nullptr;

			Assert(Math::IsPow2(n), "#buckets must be a power of two");
			const size_t origPos = key & (n - 1);	// == key % n
			size_t nextPos = origPos;
			Entry* curr = m_beg + origPos;

			// which bucket the entry belongs to
			while (curr->Key != key && curr->Key != NULL_KEY)
			{
				nextPos++;									// linear probing
				nextPos = nextPos < n ? nextPos : 0;		// wrap around to zero
				curr = m_beg + nextPos;
				Assert(nextPos != origPos, "infinite loop");	// should never happen due to load_factor < 1
			}

			return m_beg + nextPos;
		}

		void resize(size_t n) noexcept
		{
			Assert(Math::IsPow2(n), "n must be a power of 2");
			Assert(n > bucket_count(), "n must greater than current bucket count.");
			Entry* oldTable = m_beg;
			const size_t oldBucketCount = bucket_count();

			m_beg = reinterpret_cast<Entry*>(App::AllocateFromMemoryPool(n * sizeof(Entry), alignof(Entry)));
			// adjust the end pointer
			m_end = m_beg + n;

			// initialize the new table
			Entry* curr = m_beg;
			const Entry* end = m_beg + n;

			while (curr != end)
			{
				if constexpr (!std::is_trivially_default_constructible_v<Entry>)
					new (curr) Entry;

				curr->Key = NULL_KEY;
				curr++;
			}

			// reinsert all the elements
			for (Entry* curr = oldTable; curr < oldTable + oldBucketCount; curr++)
			{
				if (curr->Key == NULL_KEY)
					continue;

				Entry* elem = find_entry(curr->Key);
				Assert(elem->Key == NULL_KEY, "duplicate keys");
				elem->Key = curr->Key;
				elem->Val = ZetaMove(curr->Val);
			}

			// destruct previous elements
			if constexpr (!std::is_trivially_destructible_v<Entry>)
			{
				for (Entry* curr = oldTable; curr < oldTable + oldBucketCount; curr++)
				{
					curr->~Entry();
				}
			}

			// free the previously allocated memory
			App::FreeMemoryPool(oldTable, oldBucketCount * sizeof(Entry), alignof(Entry));
		}

		static constexpr size_t MIN_NUM_BUCKETS = 4;
		static constexpr float MAX_LOAD = 0.75f;
		//static constexpr float GROWTH_RATE = 1.5f;
		static constexpr uint64_t NULL_KEY = -1;

		Entry* m_beg = nullptr;		// pointer to the begining of memory-block
		Entry* m_end = nullptr;		// pointer to the end of memory-block
		size_t m_numEntries = 0;
	};
}