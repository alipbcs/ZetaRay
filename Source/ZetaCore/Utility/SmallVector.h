#pragma once

#include "../Math/Common.h"
#include "../Support/Memory.h"
#include <utility>    // std::swap

namespace ZetaRay::Util
{
    //--------------------------------------------------------------------------------------
    // Vector
    // 
    // Cannot be directly constructed -- base class for SmallVector.
    //--------------------------------------------------------------------------------------

    template<typename T, Support::AllocatorType Allocator = Support::SystemAllocator>
    class Vector
    {
        static constexpr size_t MIN_CAPACITY = Math::Max(64 / sizeof(T), 4llu);

    public:
        bool has_inline_storage() const
        {
            constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
            return reinterpret_cast<uintptr_t>(m_beg) == reinterpret_cast<uintptr_t>(this) + inlineStorageOffset;
        }

        void swap(Vector& other)
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

            if (!other.empty())
                reserve(oldOtherSize);         // Doesn't allocate if inline storage happens to be large enough

            if (!empty())
                other.reserve(oldSize);        // Doesn't allocate if inline storage happens to be large enough

            T* largerBeg = otherIsLarger ? other.m_beg : m_beg;
            T* smallerBeg = otherIsLarger ? m_beg : other.m_beg;

            for (size_t i = 0; i < minSize; i++)
                std::swap(*(m_beg + i), *(other.m_beg + i));

            // Copy over the remaining elements to the smaller Vector
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

        ZetaInline T* begin()
        {
            return m_beg;
        }

        ZetaInline T* end()
        {
            return m_end;
        }

        ZetaInline const T* begin() const
        {
            return m_beg;
        }

        ZetaInline const T* end() const
        {
            return m_end;
        }

        ZetaInline T* data()
        {
            return m_beg;
        }

        ZetaInline const T* data() const
        {
            return m_beg;
        }

        ZetaInline T& back()
        {
            Assert(size() > 0, "Vector is empty.");
            return *(m_beg + size() - 1);
        }

        ZetaInline const T& back() const
        {
            Assert(size() > 0, "Vector is empty.");
            return *(m_beg + size() - 1);
        }

        ZetaInline T& operator[](size_t pos)
        {
            Assert(pos < (uintptr_t)(m_end - m_beg), "Out-of-bound access.");
            return *(m_beg + pos);
        }

        ZetaInline const T& operator[](size_t pos) const
        {
            Assert(pos < (uintptr_t)(m_end - m_beg), "Out-of-bound access.");
            return *(m_beg + pos);
        }

        ZetaInline size_t capacity() const
        {
            return m_last - m_beg;
        }

        ZetaInline size_t size() const
        {
            return m_end - m_beg;
        }

        ZetaInline bool empty() const
        {
            return m_end - m_beg == 0;
        }

        void reserve(size_t n)
        {
            const size_t currCapacity = capacity();
            const size_t oldSize = size();

            if (n <= currCapacity)
                return;

            void* mem = relocate(n);

            // Adjust pointers
            m_beg = reinterpret_cast<T*>(mem);
            m_end = m_beg + oldSize;
            m_last = m_beg + n;
        }

        void resize(size_t n)
        {
            static_assert(std::is_default_constructible_v<T>, "T is not default-constructible.");

            const size_t currCapacity = capacity();
            const size_t oldSize = size();

            // Check if current capacity is enough, otherwise just adjust the "end" pointer
            if (currCapacity < n)
            {
                void* mem = relocate(n);
                m_beg = reinterpret_cast<T*>(mem);
                m_last = m_beg + n;
            }

            // Adjust the end pointer
            m_end = m_beg + n;

            // Default construct the newly added elements
            if constexpr (!std::is_trivially_default_constructible_v<T>)
            {
                if (oldSize < n)
                {
                    T* curr = m_beg + oldSize;
                    while (curr != m_end)
                    {
                        new (curr) T;
                        curr++;
                    }
                }
            }

            // Default destruct leftovers if size decreased
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                if (n < oldSize)
                {
                    T* curr = m_beg + n;
                    const T* oldEnd = m_beg + oldSize;
                    while (curr != oldEnd)
                        curr++->~T();
                }
            }
        }

        // Note: "Val" is used to initialize the newly added elements (if any) and the 
        // existing elements aren't replaced by it. May have to change in the future.
        void resize(size_t n, const T& val)
        {
            static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, 
                "T is not copy-or-move-constructible.");

            const size_t oldSize = size();
            const size_t currCapacity = capacity();

            if (currCapacity < n)
            {
                void* mem = relocate(n);
                m_beg = reinterpret_cast<T*>(mem);
                m_last = m_beg + n;
            }

            // Adjust the pointers
            m_end = m_beg + n;

            // Copy/move construct the new elements
            if (oldSize < n)
            {
                T* curr = m_beg + oldSize;
                while (curr != m_end)
                {
                    new (curr) T(ZetaForward(val));
                    curr++;
                }
            }

            // Default-destruct leftovers if size decreased
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                if (n < oldSize)
                {
                    T* curr = m_beg + n;
                    const T* oldEnd = m_beg + oldSize;
                    while (curr != oldEnd)
                        curr++->~T();
                }
            }
        }

        void pop_back(size_t num = 1)
        {
            Assert(size() >= num, "Number of elements to pop exceeds Vector's size.");

            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (size_t i = 0; i < num; i++)
                    (m_end - 1 - i)->~T();
            }

            m_end -= num;
        }

        void push_back(const T& val)
        {
            static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, 
                "T is not move-or-copy-constructible.");
            emplace_back(val);
        }

        void push_back(T&& val)
        {
            static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, 
                "T is not move-or-copy-constructible.");
            emplace_back(ZetaMove(val));
        }

        template<typename... Args>
        void emplace_back(Args&&... args)
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

        void append_range(const T* beg, const T* end, bool exact = false)
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
                Assert(false, "Calling reserve() for a non-copyable and non-movable T when Vector is non-empty is invalid.");

            m_end += num;
        }

        // Erases an element by swapping it with the last element. Returns a pointer to the next element.
        T* erase_at_index(size_t pos)
        {
            static_assert(std::is_swappable_v<T>, "T is not swappable.");
            const size_t n = size();
            Assert(pos < n, "Out-of-bound access.");
            Assert(!empty(), "Attempting to erase from an empty Vector.");

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
        T* erase(const T& item)
        {
            const size_t pos = &item - m_beg;
            return erase_at_index(pos);
        }

        void push_front(const T& val)
        {
            static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, 
                "T is not move-or-copy-constructible.");
            static_assert(std::is_swappable_v<T>, "T is not swappable.");

            emplace_back(val);

            if (const size_t n = size(); n > 1)
                std::swap(*m_beg, *(m_beg + n - 1));
        }

        void push_front(T&& val)
        {
            static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>, 
                "T is not move-or-copy-constructible.");
            static_assert(std::is_swappable_v<T>, "T is not swappable.");

            emplace_back(ZetaMove(val));

            if (const size_t n = size(); n > 1)
                std::swap(*m_beg, *(m_beg + n - 1));
        }

        void clear()
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (T* curr = m_beg; curr != m_end; curr++)
                    curr->~T();
            }

            m_end = m_beg;
        }

    protected:
        // Constructor
        Vector(const Allocator& a)
            : m_allocator(a)
        {}
        Vector(Allocator&& a)
            : m_allocator(ZetaMove(a))
        {}
        // Copy constructor
        Vector(const Vector& other)
            : m_allocator(other.m_allocator)
        {}
        // Move constructor
        Vector(Vector&& other)
            : m_allocator(ZetaMove(other.m_allocator))
        {}
        // Move assignment
        Vector& operator=(Vector<T, Allocator>&& other)
        {
            m_allocator = ZetaMove(other.m_allocator);
            return *this;
        }

        void* relocate(size_t n)
        {
            // Allocate memory to accommodate the new size
            void* mem = m_allocator.AllocateAligned(n * sizeof(T), alignof(T));
            const size_t oldSize = size();

            // Copy over the old elements
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
                    Assert(false, "Calling reserve() for a non-copyable and non-movable type T when Vector is non-empty is invalid.");

                // Destruct old elements
                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    for (T* curr = m_beg; curr < m_end; curr++)
                        curr->~T();
                }
            }

            // Free the previously allocated memory
            const size_t currCapacity = capacity();

            if (currCapacity && !has_inline_storage())
                m_allocator.FreeAligned(m_beg, currCapacity * sizeof(T), alignof(T));

            return mem;
        }

        void move_from(size_t N, Vector<T, Allocator>&& other)
        {
            static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                "T is not copy-or-move-assignable.");
            static_assert(std::is_move_assignable_v<Allocator> || std::is_copy_assignable_v<Allocator>,
                "Allocator is not copy-or-move-assignable.");

            // Just switch pointers when MovedFrom is using the heap and MovedTo's 
            // inline storage isn't large enough
            if ((N < other.size()) && !other.has_inline_storage())
            {
                m_beg = other.m_beg;
                m_end = other.m_end;
                m_last = other.m_last;

                other.m_beg = nullptr;
                other.m_end = nullptr;
                other.m_last = nullptr;
            }
            // Existing inline storage isn't large enough or MoveFrom isn't using the heap
            else if (!other.empty())
            {
                // Doesn't allocate if inline storage happens to be large enough
                reserve(other.size());

                if constexpr (std::is_trivially_copyable_v<T>)
                {
                    memcpy(m_beg, other.m_beg, sizeof(T) * other.size());
                }
                else if constexpr (std::is_move_constructible_v<T>)
                {
                    T* source = other.m_beg;
                    T* target = m_beg;
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
                    T* target = m_beg;
                    const T* end = target + other.size();

                    while (target != end)
                    {
                        new (target) T(*source);

                        source++;
                        target++;
                    }
                }

                // Adjust the pointers (m_last was set by reserve())
                m_end = m_beg + other.size();
                Assert(size() == other.size(), "These must be equal.");

                other.clear();
            }
        }

        void copy_from(const Vector<T, Allocator>& other)
        {
            static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                "T is not copy-or-move-assignable.");
            static_assert(std::is_move_assignable_v<Allocator> || std::is_copy_assignable_v<Allocator>,
                "Allocator is not copy-or-move-assignable.");

            const size_t currCapacity = capacity();
            const size_t newSize = other.size();

            if (newSize == 0)
                return;

            if (currCapacity < newSize)
            {
                // Free the previously allocated memory
                if (currCapacity > 0 && !has_inline_storage())
                    m_allocator.FreeAligned(m_beg, currCapacity * sizeof(T), alignof(T));

                // Allocate memory to accommodate new size
                void* mem = m_allocator.AllocateAligned(newSize * sizeof(T), alignof(T));

                // Adjust the pointers
                m_beg = reinterpret_cast<T*>(mem);
                m_last = m_beg + newSize;
            }

            // Regardless of whether new memory was allocated
            m_end = m_beg + newSize;

            // Just copy the memory
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                memcpy(m_beg, other.m_beg, sizeof(T) * newSize);
            }
            // Call the copy constructor
            else
            {
                T* source = m_beg;
                T* target = other.m_beg;

                for (int64_t i = 0; i < newSize; i++)
                {
                    new (source) T(*target);

                    source++;
                    target++;
                }
            }
        }

        T* m_beg = nullptr;        // Pointer to the beginning of memory block
        T* m_end = nullptr;        // Pointer to element to insert at next (one past the last inserted element)
        T* m_last = nullptr;       // Pointer to the end of memory block

#if defined(ZETA_HAS_NO_UNIQUE_ADDRESS)
        [[msvc::no_unique_address]] Allocator m_allocator;
#else
        Allocator m_allocator;
#endif
    };

    //--------------------------------------------------------------------------------------
    // InlineStorage
    //--------------------------------------------------------------------------------------

    template<typename T, uint32_t N>
    struct InlineStorage
    {
        alignas(T) uint8_t Buffer[sizeof(T) * N];
    };

    // Reminder: empty struct occupies one byte (unless c++20 [[no_unique_address]] is used)
    template<typename T>
    struct InlineStorage<T, 0>
    {};

    //--------------------------------------------------------------------------------------
    // SmallVector 
    // 
    // Dynamic array with inline storage that holds a static number of elements within the 
    // object. Inspired by the following talk:
    // Chandler Carruth, "High Performance Code 201: Hybrid Data Structures", CppCon 2016.
    //--------------------------------------------------------------------------------------

    constexpr uint32_t GetExcessSize(uint32_t sizeofT, uint32_t alignofT)
    {
        return Math::Max(0u, (uint32_t)Math::Min(
            (32 - Math::AlignUp((uint32_t)sizeof(void*) * 3, alignofT)) / sizeofT,
            (64 - Math::AlignUp((uint32_t)sizeof(void*) * 3, alignofT)) / sizeofT));
    }

    template<typename T, Support::AllocatorType Allocator = Support::SystemAllocator, uint32_t N = GetExcessSize(sizeof(T), alignof(T))>
    class SmallVector : public Vector<T, Allocator>
    {
    public:
        // Constructor
        SmallVector()
            : Vector<T, Allocator>(Allocator())
        {
            static_assert(std::is_default_constructible_v<T>, 
                "T is not default-constructible.");
            static_assert(std::is_default_constructible_v<Allocator>, 
                "Allocator is not default-constructible.");
            init_storage();
        }
        ~SmallVector()
        {
            free_memory();

            if constexpr (!std::is_trivially_destructible_v<Allocator>)
                this->m_allocator.~Allocator();
        }
        explicit SmallVector(const Allocator& a)
            : Vector<T, Allocator>(a)
        {
            static_assert(std::is_default_constructible_v<T>, "T is not default-constructible.");
            init_storage();
        }
        explicit SmallVector(const T& t)
            : Vector<T, Allocator>(Allocator())
        {
            static_assert(std::is_copy_constructible_v<T>, "T is not copy-constructible.");
            static_assert(std::is_default_constructible_v<Allocator>, "Allocator is not default-constructible.");
            init_storage(t);
        }
        SmallVector(const T& t, const Allocator& a)
            : Vector<T, Allocator>(a)
        {
            static_assert(std::is_copy_constructible_v<T>, "T is not copy-constructible.");
            init_storage(t);
        }
        // Copy constructor/assignment
        SmallVector(const SmallVector& other)
            : Vector<T, Allocator>(other)
        {
            init_storage();
            this->copy_from(other);
        }
        SmallVector(const Vector<T, Allocator>& other)
            : Vector<T, Allocator>(other)
        {
            init_storage();
            this->copy_from(other);
        }
        // Note: Currently, copy assignment doesn't change the allocator. Furthermore, 
        // the existing memory is retained if it can accommodate the new size. May need
        // to change in the future.
        SmallVector& operator=(const SmallVector& other)
        {
            if (this == &other)
                return *this;

            // Destruct existing elements
            this->clear();
            reset_storage();
            this->copy_from(other);

            return *this;
        }
        SmallVector& operator=(const Vector<T, Allocator>& other)
        {
            if (this == &other)
                return *this;

            // Destruct existing elements
            this->clear();
            reset_storage();
            this->copy_from(other);

            return *this;
        }
        // Move constructor/assignment
        SmallVector(SmallVector&& other)
            : Vector<T, Allocator>(ZetaMove(other))
        {
            init_storage();
            this->move_from(N, ZetaMove(other));
        }
        SmallVector(Vector<T, Allocator>&& other)
            : Vector<T, Allocator>(ZetaMove(other))
        {
            init_storage();
            this->move_from(N, ZetaMove(other));
        }
        SmallVector& operator=(SmallVector&& other)
        {
            if (this == &other)
                return *this;

            // Previously allocated memory is not needed anymore and can be released. Also
            // storage is reverted to inline storage.
            free_memory();

            if constexpr (!std::is_trivially_destructible_v<Allocator>)
                this->m_allocator.~Allocator();

            this->m_allocator = ZetaMove(other.m_allocator);
            this->move_from(N, ZetaMove(other));

            return *this;
        }
        SmallVector& operator=(Vector<T, Allocator>&& other)
        {
            if (this == &other)
                return *this;

            // Previously allocated memory is not needed anymore and can be released. Also
            // storage is reverted to inline storage.
            free_memory();

            if constexpr (!std::is_trivially_destructible_v<Allocator>)
                this->m_allocator.~Allocator();

            Vector<T, Allocator>::operator=(ZetaMove(other));
            this->move_from(N, ZetaMove(other));

            return *this;
        }

        void free_memory()
        {
            // Destruct existing items (if any)
            this->clear();

            const size_t currCapacity = this->capacity();

            // Free the previously allocated memory
            if (currCapacity && !this->has_inline_storage())
                this->m_allocator.FreeAligned(this->m_beg, currCapacity * sizeof(T), alignof(T));

            // Revert back to inline storage
            reset_storage();
        }

    private:
        void init_storage()
        {
            constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
            this->m_beg = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + inlineStorageOffset);
            this->m_end = this->m_beg;
            this->m_last = this->m_beg + N;

            if constexpr (!std::is_trivially_default_constructible_v<T>)
            {
                for (T* curr = this->m_beg; curr != this->m_last; curr++)
                    new (curr) T;
            }

            Assert(this->has_inline_storage(), "Must've reverted to inline storage.");
            Assert(this->capacity() == N, "Capacity must be N.");
        }
        void init_storage(const T& t)
        {
            constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
            this->m_beg = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + inlineStorageOffset);
            this->m_end = this->m_beg + N;
            this->m_last = this->m_beg + N;

            for (T* curr = this->m_beg; curr != this->m_last; curr++)
                new (curr) T(t);

            Assert(this->has_inline_storage(), "Must've reverted to inline storage.");
            Assert(this->capacity() == N, "Capacity must be N.");
        }
        void reset_storage()
        {
            constexpr size_t inlineStorageOffset = Math::AlignUp(sizeof(Vector<T, Allocator>), alignof(T));
            this->m_beg = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + inlineStorageOffset);
            this->m_end = this->m_beg;
            this->m_last = this->m_beg + N;
            Assert(this->has_inline_storage(), "Must've reverted to inline storage.");
            Assert(this->capacity() == N, "Capacity must be N.");
        }

        InlineStorage<T, N> m_inlineStorage;
    };
}