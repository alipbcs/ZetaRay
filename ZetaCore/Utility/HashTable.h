#pragma once

#include "../Math/Common.h"
#include "../Support/Memory.h"

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
	template<typename T, typename Allocator = Support::SystemAllocator>
	class HashTable
	{
		static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T is not move or copy-constructible.");
		static_assert(Support::AllocType<Allocator>, "Allocator doesn't meet the requirements for AllocType.");
		static_assert(std::is_copy_constructible_v<Allocator>, "Allocator must be copy-constructible.");

	public:

		struct Entry
		{
			uint64_t Key;
			T Val;
		};

		HashTable(const Allocator& a = Allocator()) noexcept
			: m_allocator(a)
		{}

		explicit HashTable(size_t initialSize, const Allocator& a = Allocator()) noexcept
			: m_allocator(a)
		{
			static_assert(std::is_default_constructible_v<T>);
			relocate(initialSize);
		}

		// TODO implement move & copy constructors/assignments
		HashTable(const HashTable&) = delete;
		HashTable& operator=(const HashTable&) = delete;

		void resize(size_t n) noexcept
		{
			const size_t numBuckets = bucket_count();
			if (n <= numBuckets)		// also covers when n == 0
				return;

			n = Math::Max(n, MIN_NUM_BUCKETS);

			// n > #buckets, so the next power of 2 will necessarily respect the max load factor
			n = Math::NextPow2(n);

			relocate(n);
		}

		// returns NULL if an element with the given key is not found
		// Note: in contrast to find(), find_entry() only returns NULL when the table is empty
		T* find(uint64_t key) noexcept
		{
			Entry* e = find_entry(key);
			if (e && e->Key != NULL_KEY)
				return &e->Val;

			return nullptr;
		}

		template<typename... Args>
		bool emplace(uint64_t key, Args&&... args) noexcept
		{
			Assert(key != NULL_KEY, "Invalid key");

			Entry* elem = find_entry(key);
			if (!elem || elem->Key == NULL_KEY)
			{
				const size_t numBuckets = bucket_count();
				const float load = load_factor();

				if (!m_beg || load >= MAX_LOAD)
					relocate(Math::Max(numBuckets << 1, MIN_NUM_BUCKETS));

				// find the new position to construct this Entry
				elem = find_entry(key);
				elem->Key = key;
				new (&elem->Val) T(ZetaForward(args)...);

				m_numEntries++;

				return true;
			}

			return false;
		}

		Entry& insert_or_assign(uint64_t key, const T& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T must be move-or-copy constructible.");

			Assert(key != NULL_KEY, "Invalid key");

			Entry* elem = find_entry(key);
			if (!elem || elem->Key == NULL_KEY)
			{
				const size_t numBuckets = bucket_count();
				const float load = load_factor();

				if (!m_beg || load >= MAX_LOAD)
					relocate(Math::Max(numBuckets << 1, MIN_NUM_BUCKETS));

				// find the new position to insert this Entry
				elem = find_entry(key);
				elem->Key = key;

				m_numEntries++;
			}

			new (&elem->Val) T(val);

			return *elem;
		}

		Entry& insert_or_assign(uint64_t key, T&& val) noexcept
		{
			static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, "T must be move-or-copy constructible.");

			Assert(key != NULL_KEY, "Invalid key");

			Entry* elem = find_entry(key);
			if (!elem || elem->Key == NULL_KEY)
			{
				const size_t numBuckets = bucket_count();
				const float load = load_factor();

				if (!m_beg || load >= MAX_LOAD)
					relocate(Math::Max(numBuckets << 1, MIN_NUM_BUCKETS));

				// find the new position to insert this Entry
				elem = find_entry(key);
				elem->Key = key;

				m_numEntries++;
			}

			new (&elem->Val) T(ZetaForward(val));

			return *elem;
		}

		ZetaInline size_t bucket_count() const noexcept
		{
			return m_end - m_beg;
		}

		ZetaInline size_t size() const noexcept
		{
			return m_numEntries;
		}		
		
		ZetaInline float load_factor() const noexcept
		{
			// necessary to avoid divide-by-zero
			if (empty())
				return 0.0f;

			return (float)m_numEntries / bucket_count();
		}

		ZetaInline bool empty() const noexcept
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
					curr++->~Entry();
			}

			m_numEntries = 0;

			// free the previously allocated memory
			if(bucket_count())
				//App::FreeMemoryPool(m_beg, bucket_count() * sizeof(Entry), alignof(Entry));
				//_aligned_free(m_beg);
				m_allocator.FreeAligned(m_beg, bucket_count() * sizeof(Entry), alignof(Entry));
		}

		void swap(HashTable& other) noexcept
		{
			std::swap(m_beg, other.m_beg);
			std::swap(m_end, other.m_end);
			std::swap(m_numEntries, other.m_numEntries);
			std::swap(m_allocator, other.m_allocator);
		}

		ZetaInline T& operator[](uint64_t key) noexcept
		{
			static_assert(std::is_default_constructible_v<T>, "T must be default-constructible");

			Entry* elem = find_entry(key);
			if (!elem || elem->Key == NULL_KEY)
			{
				const size_t numBuckets = bucket_count();
				const float load = load_factor();

				if (!m_beg || load >= MAX_LOAD)
					relocate(Math::Max(numBuckets << 1, MIN_NUM_BUCKETS));

				// find the new position to insert this Entry
				elem = find_entry(key);
				elem->Key = key;
				new (&elem->Val) T();

				m_numEntries++;
			}

			return elem->Val;
		}

		ZetaInline Entry* begin_it() noexcept
		{
			return m_beg;
		}

		ZetaInline Entry* next_it(Entry* curr) noexcept
		{
			Entry* next = curr + 1;
			while (next != m_end && next->Key == NULL_KEY)
				next++;
			
			return next;
		}

		ZetaInline Entry* end_it() noexcept
		{
			return m_end;
		}

	private:
		Entry* find_entry(uint64_t key) noexcept
		{
			const size_t n = bucket_count();
			if (n == 0)
				return nullptr;

			const size_t origPos = key & (n - 1);	// == key % n (n is a power of 2)
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

		void relocate(size_t n) noexcept
		{
			Assert(Math::IsPow2(n), "n must be a power of 2");
			Assert(n > bucket_count(), "n must be greater than the current bucket count.");
			Entry* oldTable = m_beg;
			const size_t oldBucketCount = bucket_count();

			//m_beg = reinterpret_cast<Entry*>(_aligned_malloc(n * sizeof(Entry), alignof(Entry)));
			m_beg = reinterpret_cast<Entry*>(m_allocator.AllocateAligned(n * sizeof(Entry), alignof(Entry)));
			// adjust the end pointer
			m_end = m_beg + n;

			// initialize the new table
			const Entry* end = m_beg + n;

			for (Entry* curr = m_beg; curr != end; curr++)
			{
				if constexpr (!std::is_trivially_default_constructible_v<Entry>)
					new (curr) Entry;

				curr->Key = NULL_KEY;
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

			// destruct previous elements (if necessary)
			if constexpr (!std::is_trivially_destructible_v<Entry>)
			{
				for (Entry* curr = oldTable; curr < oldTable + oldBucketCount; curr++)
					curr->~Entry();
			}

			// free the previously allocated memory
			//App::FreeMemoryPool(oldTable, oldBucketCount * sizeof(Entry), alignof(Entry));
			//_aligned_free(oldTable);
			if(oldTable)
				m_allocator.FreeAligned(oldTable, oldBucketCount * sizeof(Entry), alignof(Entry));
		}

		static constexpr size_t MIN_NUM_BUCKETS = 4;
		static constexpr float MAX_LOAD = 0.75f;
		//static constexpr float GROWTH_RATE = 1.5f;
		static constexpr uint64_t NULL_KEY = uint64_t(-1);

		Entry* m_beg = nullptr;		// pointer to the begining of memory block
		Entry* m_end = nullptr;		// pointer to the end of memory block
		size_t m_numEntries = 0;

		Allocator m_allocator;
	};
}