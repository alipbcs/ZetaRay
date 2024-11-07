#pragma once

#include "../Math/Common.h"
#include "../Support/Memory.h"
#include "../Utility/Optional.h"

namespace ZetaRay::Util
{
    // Open-set addressing with linear probing
    // 
    //  - Assumes keys are already hashed; key itself is not stored, only its hash (uint64_t). Consequently,
    //    collisions on keys could lead to wrong results. By using a decent hash function, chances of 
    //    such collisions should be low.
    //  - Iterators (pointers) are NOT stable; pointer to an entry found earlier might not be valid
    //    anymore due to subsequent insertions and possible resize.
    //  - Not thread-safe
    template<typename ValueType, typename KeyType = uint64_t, Support::AllocatorType Allocator = Support::SystemAllocator>
    requires std::is_integral_v<KeyType>
    class HashTable
    {
        static_assert(std::is_copy_constructible_v<ValueType> || std::is_move_constructible_v<ValueType>, 
            "ValueType is not move or copy-constructible.");

    public:
        struct Entry
        {
            KeyType Key;
            ValueType Val;
        };

        explicit HashTable(const Allocator& a = Allocator())
            : m_allocator(a)
        {}
        explicit HashTable(size_t initialSize, const Allocator& a = Allocator())
            : m_allocator(a)
        {
            static_assert(std::is_default_constructible_v<ValueType>);
            relocate(Math::NextPow2(initialSize));
        }
        ~HashTable()
        {
            free_memory();

            if constexpr (!std::is_trivially_destructible_v<Allocator>)
                this->m_allocator.~Allocator();
        }

        // TODO implement move & copy constructors/assignments
        HashTable(const HashTable&) = delete;
        HashTable& operator=(const HashTable&) = delete;

        // "accountForMaxLoad": A common use case is when the maximum number of 
        // elements is known, so resize is used to allocate all the necessary storage 
        // once. But this doesn't account for load factor, as when size approaches 
        // "n", another allocation takes place so that load factor stays below maximum.
        // When accounting for this, table should be resized to ceil(n / max_load_factor).
        void resize(size_t n, bool accountForMaxLoad = false)
        {
            if (n <= bucket_count()) // also covers when n == 0
                return;

            n = Math::Max(n, MIN_NUM_BUCKETS);
            n = accountForMaxLoad ? Math::Ceil(n / MAX_LOAD) : n;
            n = Math::NextPow2(n);  // n > #buckets, so the next power of two will also respect the max load factor
            relocate(n);
        }

        // Returns NULL if an element with the given key is not found.
        // Note: in contrast to find(), find_entry() returns NULL only when the table is empty.
        Util::Optional<ValueType*> find(KeyType key) const
        {
            Entry* e = find_entry(key);
            if (e && e->Key == key)
                return &e->Val;

            return {};
        }

        // Inserts a new entry only if it doesn't already exist
        template<typename... Args>
        bool try_emplace(KeyType key, Args&&... args)
        {
            Assert(key != NULL_KEY && key != TOMBSTONE_KEY, "Invalid key.");

            Entry* elem = find_entry(key);
            if (!elem || elem->Key != key)
            {
                if (elem && (elem->Key == TOMBSTONE_KEY))
                {
                    elem->Key = key;
                    new (&elem->Val) ValueType(ZetaForward(args)...);
                    m_numNonTombstoneEntries++;

                    return true;
                }

                if (!m_beg || load_factor() >= MAX_LOAD || m_numEntries + 1 == bucket_count())
                {
                    relocate(Math::Max(bucket_count() << 1, MIN_NUM_BUCKETS));
                    // Find the new position to construct this Entry
                    elem = find_entry(key);
                }

                elem->Key = key;
                new (&elem->Val) ValueType(ZetaForward(args)...);
                m_numEntries++;
                m_numNonTombstoneEntries++;
                Assert(m_numEntries < bucket_count(), "Load factor should never be 1.0.");

                return true;
            }

            return false;
        }

        // Assign to the entry if already exists, otherwise inserts a new entry
        Entry& insert_or_assign(KeyType key, const ValueType& val)
        {
            static_assert(std::is_copy_constructible_v<ValueType> || std::is_move_constructible_v<ValueType>,
                "ValueType must be move-or-copy constructible.");
            Assert(key != NULL_KEY && key != TOMBSTONE_KEY, "Invalid key.");

            Entry* elem = find_entry(key);
            if (!elem || elem->Key != key)
            {
                if (elem && elem->Key == TOMBSTONE_KEY)
                {
                    elem->Key = key;
                    new (&elem->Val) ValueType(val);
                    m_numNonTombstoneEntries++;

                    return *elem;
                }

                if (!m_beg || load_factor() >= MAX_LOAD || m_numEntries + 1 == bucket_count())
                {
                    relocate(Math::Max(bucket_count() << 1, MIN_NUM_BUCKETS));
                    elem = find_entry(key);
                }

                elem->Key = key;
                m_numEntries++;
                m_numNonTombstoneEntries++;
                Assert(m_numEntries < bucket_count(), "Load factor should never be 1.0.");
            }

            new (&elem->Val) ValueType(val);

            return *elem;
        }

        Entry& insert_or_assign(KeyType key, ValueType&& val)
        {
            static_assert(std::is_copy_constructible_v<ValueType> || std::is_move_constructible_v<ValueType>,
                "ValueType must be move-or-copy constructible.");
            Assert(key != NULL_KEY && key != TOMBSTONE_KEY, "Invalid key.");

            Entry* elem = find_entry(key);
            if (!elem || elem->Key != key)
            {
                if (elem && elem->Key == TOMBSTONE_KEY)
                {
                    elem->Key = key;
                    new (&elem->Val) ValueType(ZetaForward(val));
                    m_numNonTombstoneEntries++;

                    return *elem;
                }

                if (!m_beg || load_factor() >= MAX_LOAD || m_numEntries + 1 == bucket_count())
                {
                    relocate(Math::Max(bucket_count() << 1, MIN_NUM_BUCKETS));
                    elem = find_entry(key);
                }

                elem->Key = key;
                m_numEntries++;
                m_numNonTombstoneEntries++;
                Assert(m_numEntries < bucket_count(), "Load factor should never be 1.0.");
            }

            new (&elem->Val) ValueType(ZetaForward(val));

            return *elem;
        }

        ZetaInline size_t erase(KeyType key)
        {
            Entry* elem = find_entry(key);
            if (!elem || elem->Key != key || elem->Key == TOMBSTONE_KEY)
                return 0;

            elem->Key = TOMBSTONE_KEY;
            Assert(m_numNonTombstoneEntries >= 1, "Invalid hash table state.");
            m_numNonTombstoneEntries--;
            if constexpr (!std::is_trivially_destructible_v<ValueType>)
                elem->~Entry();

            return 1;
        }

        ZetaInline size_t bucket_count() const
        {
            return m_end - m_beg;
        }

        // Note that tombstone entries count towards #entries
        ZetaInline size_t size() const
        {
            return m_numNonTombstoneEntries;
        }

        ZetaInline float load_factor() const
        {
            // Avoid divide-by-zero
            return m_numEntries == 0 ? 0.0f : (float)m_numEntries / bucket_count();
        }

        ZetaInline bool empty() const
        {
            return m_numNonTombstoneEntries == 0;
        }

        void clear()
        {
            if constexpr (!std::is_trivially_destructible_v<ValueType>)
            {
                size_t i = 0;
                
                for (auto it = begin_it(); it < end_it(); it = next_it(it))
                {
                    it->~Entry();
                    i++;
                }
                
                Assert(i == m_numNonTombstoneEntries, "Number of cleared entries must match the number of entries.");
            }

            for (Entry* curr = m_beg; curr != m_end; curr++)
                curr->Key = NULL_KEY;

            m_numEntries = 0;
            m_numNonTombstoneEntries = 0;
            // Don't free the memory
        }

        void free_memory()
        {
            if constexpr (!std::is_trivially_destructible_v<ValueType>)
            {
                size_t i = 0;

                for (auto it = begin_it(); it < end_it(); it = next_it(it))
                {
                    it->~Entry();
                    i++;
                }

                Assert(i == m_numNonTombstoneEntries, "Number of cleared entries must match the number of entries.");
            }

            // Free the previously allocated memory
            if(bucket_count())
                m_allocator.FreeAligned(m_beg, bucket_count() * sizeof(Entry), alignof(Entry));

            m_numEntries = 0;
            m_numNonTombstoneEntries = 0;
            m_beg = nullptr;
            m_end = nullptr;
        }

        void swap(HashTable& other)
        {
            std::swap(m_beg, other.m_beg);
            std::swap(m_end, other.m_end);
            std::swap(m_numEntries, other.m_numEntries);
            std::swap(m_numNonTombstoneEntries, other.m_numNonTombstoneEntries);
            std::swap(m_allocator, other.m_allocator);
        }

        ValueType& operator[](KeyType key)
        {
            static_assert(std::is_default_constructible_v<ValueType>, "ValueType must be default-constructible");
            Assert(key != NULL_KEY && key != TOMBSTONE_KEY, "Invalid key.");

            Entry* elem = find_entry(key);
            if (!elem || elem->Key != key)
            {
                if (elem && elem->Key == TOMBSTONE_KEY)
                {
                    elem->Key = key;
                    new (&elem->Val) ValueType();
                    m_numNonTombstoneEntries++;

                    return elem->Val;
                }

                if (!m_beg || load_factor() >= MAX_LOAD || m_numEntries + 1 == bucket_count())
                {
                    relocate(Math::Max(bucket_count() << 1, MIN_NUM_BUCKETS));
                    elem = find_entry(key);
                }

                elem->Key = key;
                new (&elem->Val) ValueType();
                m_numEntries++;
                m_numNonTombstoneEntries++;
                Assert(m_numEntries < bucket_count(), "Load factor should never be 1.0.");
            }

            return elem->Val;
        }

        ZetaInline Entry* begin_it()
        {
            // When memory is allocated, but there hasn't been any insertions,
            // m_beg != m_end, which would erroneously indicate table is non-mepty.
            if (m_numEntries == 0)
                return m_end;

            auto it = m_beg;
            while (it != m_end && (it->Key == NULL_KEY || it->Key == TOMBSTONE_KEY))
                it++;

            return it;
        }

        ZetaInline Entry* next_it(Entry* curr)
        {
            Entry* next = curr + 1;
            while (next != m_end && (next->Key == NULL_KEY || next->Key == TOMBSTONE_KEY))
                next++;

            return next;
        }

        ZetaInline Entry* end_it()
        {
            return m_end;
        }

    private:
        Entry* find_entry(KeyType key) const
        {
            const size_t n = bucket_count();
            if (n == 0)
                return nullptr;

            const size_t origPos = key & (n - 1);    // == key % n (n is a power of two)
            size_t nextPos = origPos;
            Entry* curr = m_beg + origPos;
            Entry* tombstone = nullptr;

            // Which bucket the entry belongs to
            while (curr->Key != key && curr->Key != NULL_KEY)
            {
                // Remember tombstone entries but keep probing
                if (curr->Key == TOMBSTONE_KEY)
                    tombstone = curr;
 
                nextPos++;                                  // Linear probing
                nextPos = nextPos < n ? nextPos : 0;        // Wrap around to zero
                curr = m_beg + nextPos;
                Assert(nextPos != origPos, "infinite loop");    // Should never happen due to load_factor < 1
            }

            if (curr->Key == key)
                return curr;

            return tombstone ? tombstone : curr;
        }

        void relocate(size_t n)
        {
            Assert(Math::IsPow2(n), "n must be a power of two.");
            Assert(n > bucket_count(), "n must be greater than the current bucket count.");
            Entry* oldTable = m_beg;
            const size_t oldBucketCount = bucket_count();

            m_beg = reinterpret_cast<Entry*>(m_allocator.AllocateAligned(n * sizeof(Entry), alignof(Entry)));
            m_end = m_beg + n;  // Adjust end pointer
            m_numEntries = 0;
            m_numNonTombstoneEntries = 0;

            // Initialize new table
            for (Entry* curr = m_beg; curr != m_beg + n; curr++)
            {
                if constexpr (!std::is_trivially_default_constructible_v<Entry>)
                    new (curr) Entry;

                curr->Key = NULL_KEY;
            }

            // Reinsert all elements
            for (Entry* curr = oldTable; curr < oldTable + oldBucketCount; curr++)
            {
                if (curr->Key == NULL_KEY || curr->Key == TOMBSTONE_KEY)
                    continue;

                Entry* elem = find_entry(curr->Key);
                Assert(elem->Key == NULL_KEY, "duplicate keys.");
                elem->Key = curr->Key;
                elem->Val = ZetaMove(curr->Val);
                m_numEntries++;
            }

            m_numNonTombstoneEntries = m_numEntries;

            // Destruct previous elements (if necessary)
            if constexpr (!std::is_trivially_destructible_v<Entry>)
            {
                for (Entry* curr = oldTable; curr < oldTable + oldBucketCount; curr++)
                    curr->~Entry();
            }

            // Free the previously allocated memory
            if(oldTable)
                m_allocator.FreeAligned(oldTable, oldBucketCount * sizeof(Entry), alignof(Entry));
        }

        static constexpr size_t MIN_NUM_BUCKETS = 4;
        static constexpr float MAX_LOAD = 0.8f;
        static constexpr KeyType NULL_KEY = KeyType(-1);
        static constexpr KeyType TOMBSTONE_KEY = KeyType(-2);

        Entry* m_beg = nullptr;        // Pointer to the beginning of memory block
        Entry* m_end = nullptr;        // Pointer to the end of memory block
        size_t m_numEntries = 0;
        size_t m_numNonTombstoneEntries = 0;
#if defined(__clang__)
        Allocator m_allocator;
#elif defined(_MSC_VER)
        [[msvc::no_unique_address]] Allocator m_allocator;
#endif
    };
}