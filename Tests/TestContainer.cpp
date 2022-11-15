#include <Utility/SmallVector.h>
#include <Win32/App.h>
#include <doctest-2.4.9/doctest.h>

using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Math;
using namespace ZetaRay::Support;
using namespace ZetaRay::App;

TEST_SUITE("SmallVector")
{
	//TEST_CASE("Init")
	//{
	//	App::InitSimple();
	//}

	TEST_CASE("Basic")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);

		SmallVector<int, 3, ArenaAllocator> vec1(aa);
		CHECK(vec1.has_inline_storage() == true);

		for (int i = 0; i < 3; i++)
		{
			vec1.push_back(i);
			CHECK(vec1.has_inline_storage() == true);
			CHECK(vec1[i] == i);
		}

		vec1.push_back(4);
		CHECK(vec1.has_inline_storage() == false);
	}

	TEST_CASE("Move-constructor-HeapHeap")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);

		SmallVector<int, 0, ArenaAllocator> vec1(aa);
		SmallVector<int, 0, ArenaAllocator> vec2(aa);

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

	TEST_CASE("Move-constructor-HeapInline")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);

		SmallVector<int, 0, ArenaAllocator> vec1(aa);
		SmallVector<int, 10, ArenaAllocator> vec2(aa);

		for (int i = 0; i < 4; i++)
			vec1.push_back(i);

		vec2 = ZetaMove(vec1);
		CHECK(vec2.has_inline_storage() == true);

		CHECK(vec1.size() == 0);
		CHECK(vec2.size() == 4);

		for (int i = 0; i < 4; i++)
			CHECK(vec2[i] == i);
	}

	TEST_CASE("Move-constructor-InlineInline")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);
	
		SmallVector<int, 5, ArenaAllocator> vec1(aa);
		SmallVector<int, 10, ArenaAllocator> vec2(aa);

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

	TEST_CASE("Copy-constructor")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);

		SmallVector<int, 0, ArenaAllocator> vec1(aa);

		for (int i = 0; i < 4; i++)
			vec1.push_back(i);

		SmallVector<int, 0, ArenaAllocator> vec2(aa);
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

	TEST_CASE("Swap")
	{
		MemoryArena ma(128);
		ArenaAllocator aa(ma);

		SmallVector<int, 0, ArenaAllocator> vec1(aa);
		SmallVector<int, 10, ArenaAllocator> vec2(aa);

		for (int i = 0; i < 10; i++)
			vec2.push_back(i);

		vec1.swap(vec2);
		CHECK(vec1.size() == 10);
		CHECK(vec2.size() == 0);

		for (int i = 0; i < 10; i++)
			CHECK(vec1[i] == i);
	}
};