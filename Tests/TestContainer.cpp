#include <Utility/SmallVector.h>
#include <doctest-2.4.8/doctest.h>

using namespace ZetaRay;
using namespace ZetaRay::Math;

TEST_SUITE("SmallVector")
{
	TEST_CASE("Init")
	{
		App::InitSimple();
	}

	TEST_CASE("Basic")
	{
		SmallVector<int, 3> vec;
		CHECK(vec.has_inline_storage() == true);

		for (int i = 0; i < 3; i++)
		{
			vec.push_back(i);
			CHECK(vec.has_inline_storage() == true);
			CHECK(vec[i] == i);
		}

		vec.push_back(4);
		CHECK(vec.has_inline_storage() == false);
	}

	TEST_CASE("Move-constructor-HeapHeap")
	{
		SmallVector<int, 0> vec1;
		SmallVector<int, 0> vec2;

		for (int i = 0; i < 4; i++)
		{
			vec1.push_back(i);
		}

		vec2 = ZetaMove(vec1);
		CHECK(vec2.has_inline_storage() == false);
		CHECK(vec1.begin() == nullptr);
		CHECK(vec1.end() == nullptr);
		CHECK(vec1.capacity() == 0);

		CHECK(vec1.size() == 0);
		CHECK(vec2.size() == 4);

		for (int i = 0; i < 4; i++)
		{
			CHECK(vec2[i] == i);
		}
	}

	TEST_CASE("Move-constructor-HeapInline")
	{
		SmallVector<int, 0> vec1;
		SmallVector<int, 10> vec2;

		for (int i = 0; i < 4; i++)
		{
			vec1.push_back(i);
		}

		vec2 = ZetaMove(vec1);
		CHECK(vec2.has_inline_storage() == true);

		CHECK(vec1.size() == 0);
		CHECK(vec2.size() == 4);

		for (int i = 0; i < 4; i++)
		{
			CHECK(vec2[i] == i);
		}
	}

	TEST_CASE("Move-constructor-InlineInline")
	{
		SmallVector<int, 5> vec1;
		SmallVector<int, 10> vec2;

		for (int i = 0; i < 5; i++)
		{
			vec1.push_back(i);
		}

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
		SmallVector<int, 0> vec1;

		for (int i = 0; i < 4; i++)
		{
			vec1.push_back(i);
		}

		SmallVector<int, 0> vec2;
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
		SmallVector<int, 0> vec1;
		SmallVector<int, 10> vec2;

		for (int i = 0; i < 10; i++)
		{
			vec2.push_back(i);
		}

		vec1.swap(vec2);
		CHECK(vec1.size() == 10);
		CHECK(vec2.size() == 0);

		for (int i = 0; i < 10; i++)
		{
			CHECK(vec1[i] == i);
		}
	}
};