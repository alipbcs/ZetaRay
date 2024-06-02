#include <Utility/SmallVector.h>
#include <Utility/HashTable.h>
#include <App/App.h>
#include <Support/MemoryArena.h>
#include <doctest/doctest.h>

using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::App;

TEST_SUITE("SmallVector")
{
    TEST_CASE("Basic")
    {
        MemoryArena ma(32);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator, 3> vec1(aa);
        CHECK(vec1.has_inline_storage() == true);

        for (int i = 0; i < 3; i++)
        {
            vec1.push_back(i);
            CHECK(vec1.has_inline_storage() == true);
            CHECK(vec1[i] == i);
        }

        vec1.push_back(4);
        CHECK(vec1.has_inline_storage() == false);

        SmallVector<int, SystemAllocator, 5> vec2(12);
        CHECK(vec2.size() == 5);
        for(int i = 0; i < 5; i++)
            CHECK(vec2[i] == 12);

        SmallVector<int, SystemAllocator, 15> vec3;
        CHECK(vec3.has_inline_storage());

        vec3.reserve(3);
        CHECK(vec3.has_inline_storage());

        vec3.resize(10);
        CHECK(vec3.capacity() == 15);
        vec3.resize(20);
        CHECK(!vec3.has_inline_storage());
        CHECK(vec3.capacity() == 20);
        vec3.resize(3);
        CHECK(vec3.capacity() == 20);
    }

    TEST_CASE("Move constructor-HeapHeap")
    {
        MemoryArena ma(8);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator> vec1(aa);
        SmallVector<int, ArenaAllocator> vec2(aa);

        for (int i = 0; i < 4; i++)
            vec1.push_back(i);

        vec2 = ZetaMove(vec1);
        CHECK(vec2.has_inline_storage() == false);
        CHECK(vec1.begin() == nullptr);
        CHECK(vec1.end() == nullptr);
        CHECK(vec1.capacity() == 0);

        CHECK(vec1.size() == 0);
        CHECK(vec2.size() == 4);

        for (int i = 0; i < 4; i++)
            CHECK(vec2[i] == i);
    }

    TEST_CASE("Move constructor-HeapInline")
    {
        MemoryArena ma(16);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator> vec1(aa);
        SmallVector<int, ArenaAllocator, 10> vec2(aa);
        vec2.push_back(10);
        vec2.push_back(11);

        for (int i = 0; i < 4; i++)
            vec1.push_back(i);

        vec2 = ZetaMove(vec1);
        CHECK(vec2.has_inline_storage() == true);

        CHECK(vec1.size() == 0);
        CHECK(vec2.size() == 4);

        for (int i = 0; i < 4; i++)
            CHECK(vec2[i] == i);
    }

    TEST_CASE("Move constructor-InlineInline")
    {
        MemoryArena ma(8);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator, 5> vec1(aa);
        SmallVector<int, ArenaAllocator, 10> vec2(aa);

        for (int i = 0; i < 5; i++)
            vec1.push_back(i);

        CHECK(vec1.has_inline_storage() == true);
        vec2 = ZetaMove(vec1);
        CHECK(vec2.has_inline_storage() == true);

        CHECK(vec1.size() == 0);
        CHECK(vec2.size() == 5);

        for (int i = 0; i < 5; i++)
        {
            CHECK(vec2[i] == i);
        }
    }

    TEST_CASE("Copy assignment")
    {
        MemoryArena ma(128);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator> vec1(aa);
        SmallVector<int, ArenaAllocator> vec2(aa);

        for (int i = 0; i < 4; i++)
            vec1.push_back(i);

        vec2 = vec1;
        CHECK(vec2.has_inline_storage() == false);

        CHECK(vec1.size() == 4);
        CHECK(vec2.size() == 4);

        for (int i = 0; i < 4; i++)
        {
            CHECK(vec1[i] == i);
            CHECK(vec2[i] == i);
        }
    }

    TEST_CASE("Default constructor")
    {
        struct A
        {
            A()
                : m_a(3)
            {}
            int m_a;
        };

        SmallVector<A, SystemAllocator, 5> vec;
        vec.resize(3);

        for (auto& a : vec)
            CHECK(a.m_a == 3);
    }

    TEST_CASE("Swap")
    {
        MemoryArena ma(128);
        ArenaAllocator aa(ma);

        SmallVector<int, ArenaAllocator> vec1(aa);
        SmallVector<int, ArenaAllocator, 10> vec2(aa);

        for (int i = 0; i < 10; i++)
            vec2.push_back(i);

        vec1.swap(vec2);
        CHECK(vec1.size() == 10);
        CHECK(vec2.size() == 0);

        for (int i = 0; i < 10; i++)
            CHECK(vec1[i] == i);
    }
};

TEST_SUITE("HashTable")
{
    TEST_CASE("Basic")
    {
        HashTable<int> table(6);

        CHECK(table.empty());
        CHECK(table.size() == 0);
        CHECK(table.load_factor() == 0.0f);
        CHECK(table.find(1) == nullptr);
        CHECK(table.bucket_count() == 8);

        CHECK(table.try_emplace(0, 100));
        CHECK(!table.empty());
        CHECK(table.try_emplace(1, 101));
        CHECK(table.try_emplace(2, 102));
        CHECK(table.try_emplace(3, 103));

        const auto oldSize = table.size();
        const auto oldLoad = table.load_factor();
        CHECK(!table.try_emplace(3, 103));
        const auto newSize = table.size();
        const auto newLoad = table.load_factor();
        CHECK(oldSize == newSize);
        CHECK(oldLoad == newLoad);

        auto* entry = table.find(2);
        CHECK(entry);
        CHECK(*entry == 102);

        table.insert_or_assign(0, 200);
        CHECK(newSize == table.size());
        entry = table.find(0);
        CHECK(*entry == 200);
    }

    TEST_CASE("Relocation")
    {
        HashTable<int> table(6);
        CHECK(table.bucket_count() == 8);

        table.try_emplace(0, 100);
        table[1] = 101;
        table[2] = 102;
        table[3] = 103;
        table[4] = 104;
        table[5] = 105;
        table[6] = 106;

        const auto oldLoad = table.load_factor();
        CHECK(table.size() == 7);

        // Should trigger relocation
        table[7] = 107;
        const auto newLoad = table.load_factor();
        CHECK(newLoad < oldLoad);
        CHECK(table.size() == 8);

        for (int i = 0; i < 8; i++)
        {
            auto* entry = table.find(i);
            CHECK(*entry == 100 + i);
        }
    }

    TEST_CASE("Erase")
    {
        HashTable<int> table(6);
        CHECK(table.bucket_count() == 8);

        table[3] = 103;
        table[3 + 8] = 104;
        table[3 + 8 * 2] = 105;
        CHECK(table.size() == 3);

        auto numErased = table.erase(3 + 8);
        CHECK(numErased == 1);
        auto* entry = table.find(3 + 8);
        CHECK(!entry);
        numErased = table.erase(10);
        CHECK(numErased == 0);

        // Probing with tombstones in between
        entry = table.find(3 + 8 * 2);
        CHECK(entry);
        CHECK(*entry == 105);

        // Erase shouldn't change the size
        CHECK(table.size() == 3);

        // Should reuse the removed entry
        table[3 + 8 * 3] = 106;
        CHECK(table.size() == 3);

        // Delete all the entries
        numErased = table.erase(3);
        CHECK(numErased == 1);
        numErased = table.erase(3 + 8 * 2);
        CHECK(numErased == 1);
        numErased = table.erase(3 + 8 * 3);
        CHECK(numErased == 1);
        CHECK(table.size() == 3);

        table[0] = 100;
        table[1] = 101;
        table[2] = 102;
        table[3] = 103;
        // We've had 4 inserts, one of which should resue: 3 + 4 - 1 = 6
        CHECK(table.size() == 6);

        table.erase(0);
        table.erase(1);
        table.erase(2);
        CHECK(table.size() == 6);

        // Insert new entris to force resize
        table[6] = 106;
        auto oldLoad = table.load_factor();
        CHECK(table.size() == 7);
        table[7] = 107;
        auto newLoad = table.load_factor();
        CHECK(newLoad < oldLoad);
        // Tombstones shouldn't be carried over to new table
        CHECK(table.size() == 3);
    }
};