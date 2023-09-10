#include <Support/OffsetAllocator.h>
#include <doctest/doctest.h>

using namespace ZetaRay::Support;

// Ref: https://github.com/sebbbi/OffsetAllocator/blob/main/offsetAllocatorTests.cpp
TEST_SUITE("OffsetAllocator")
{
	TEST_CASE("MaxNumAllocs")
	{
		OffsetAllocator allocator(128, 2);
		
		auto a = allocator.Allocate(31);
		CHECK(!a.IsEmpty());

		auto b = allocator.Allocate(23);
		CHECK(!b.IsEmpty());

		auto c = allocator.Allocate(19);
		CHECK(c.IsEmpty());
	}

	TEST_CASE("Alignment")
	{
		OffsetAllocator allocator(1024, 16);

		auto a = allocator.Allocate(7);
		CHECK(a.Offset == 0);

		auto b = allocator.Allocate(10);
		CHECK(b.Offset == 7);
		auto c = allocator.Allocate(12);
		CHECK(c.Offset == 17);

		allocator.Free(b);

		auto d = allocator.Allocate(1, 256);
		// shouldn't reuse "B" even though the size fits
		CHECK(d.Offset != 7);
		CHECK((d.Offset & 255) == 0);

		// should reuse "B"
		auto e = allocator.Allocate(1);
		CHECK(e.Offset == 7);
	}

	TEST_CASE("FreeStorage")
	{
		OffsetAllocator allocator(1024, 16);

		auto a = allocator.Allocate(88);
		auto b = allocator.Allocate(91);

		allocator.Free(a);
		CHECK(allocator.FreeStorage() == 1024 - 91);

		auto c = allocator.Allocate(85);
		CHECK(allocator.FreeStorage() == 1024 - 91 - 85);
	}

	TEST_CASE("Free")
	{
		OffsetAllocator allocator(1024, 16);

		// Free merges neighbor empty nodes. Next allocation should also have offset = 0
		auto a = allocator.Allocate(137);
		CHECK(a.Offset == 0);
		allocator.Free(a);

		auto b = allocator.Allocate(137);
		CHECK(b.Offset == 0);
		allocator.Free(b);

		// End: Validate that allocator has no fragmentation left. Should be 100% clean.
		auto validateAll = allocator.Allocate(1024);
		CHECK(validateAll.Offset == 0);
		allocator.Free(validateAll);
	}

	TEST_CASE("Merge")
	{
		OffsetAllocator allocator(1024, 16);

		// Free merges neighbor empty nodes. Next allocation should also have offset = 0
		auto a = allocator.Allocate(1);
		REQUIRE(a.Offset == 0);

		auto b = allocator.Allocate(123);
		REQUIRE(b.Offset == 1);

		auto c = allocator.Allocate(12);
		REQUIRE(c.Offset == 124);

		auto d = allocator.Allocate(29);
		REQUIRE(d.Offset == 136);

		allocator.Free(a);
		allocator.Free(c);
		allocator.Free(b);
		allocator.Free(d);

		// End: Validate that allocator has no fragmentation left. Should be 100% clean.
		auto validateAll = allocator.Allocate(1024);
		CHECK(validateAll.Offset == 0);
		allocator.Free(validateAll);
	}

	TEST_CASE("Reuse (trivial)")
	{
		OffsetAllocator allocator(1024, 16);

		// Allocator should reuse node freed by A since the allocation C fits in the same bin
		auto a = allocator.Allocate(128);
		CHECK(a.Offset == 0);

		auto b = allocator.Allocate(345);
		CHECK(b.Offset == 128);

		allocator.Free(a);

		auto c = allocator.Allocate(128);
		CHECK(c.Offset == 0);

		allocator.Free(c);
		allocator.Free(b);

		// End: Validate that allocator has no fragmentation left. Should be 100% clean.
		auto validateAll = allocator.Allocate(1024);
		CHECK(validateAll.Offset == 0);
		allocator.Free(validateAll);
	}

	TEST_CASE("Reuse (complex)")
	{
		OffsetAllocator allocator(1024, 16);

		// Allocator should not reuse node freed by A since the allocation C doesn't fit in the same bin
		// However node D and E fit there and should reuse node from A
		auto a = allocator.Allocate(128);
		CHECK(a.Offset == 0);

		auto b = allocator.Allocate(345);
		CHECK(b.Offset == 128);

		allocator.Free(a);

		auto c = allocator.Allocate(234);
		CHECK(c.Offset == 128 + 345);

		// should reuse "A" (smallest free node such that node.size >= request), which is
		// then broken up to 45 and (128 - 45) blocks
		auto d = allocator.Allocate(45);
		CHECK(d.Offset == 0);

		auto e = allocator.Allocate(51);
		CHECK(e.Offset == 45);

		auto report = allocator.GetStorageReport();
		CHECK(report.TotalFreeSpace == 1024 - 345 - 234 - 45 - 51);
		CHECK(report.LargestFreeRegion != report.TotalFreeSpace);

		allocator.Free(c);
		allocator.Free(d);
		allocator.Free(b);
		allocator.Free(e);

		// End: Validate that allocator has no fragmentation left. Should be 100% clean.
		auto validateAll = allocator.Allocate(1024);
		CHECK(validateAll.Offset == 0);
		allocator.Free(validateAll);
	}

	TEST_CASE("Fragmentation")
	{
		OffsetAllocator allocator(256 * 1024, 1024);

		// Allocate 256 x 1kb. Should fit. Then free four random slots and reallocate four slots.
		// Plus free four contiguous slots an allocate 4x larger slot. All must be zero fragmentation!
		OffsetAllocator::Allocation allocations[256];

		for (int i = 0; i < 256; i++)
		{
			allocations[i] = allocator.Allocate(1024);
			CHECK(allocations[i].Offset == i * 1024);
		}

		auto report = allocator.GetStorageReport();
		CHECK(report.TotalFreeSpace == 0);
		CHECK(report.LargestFreeRegion == 0);

		// Free four random slots
		allocator.Free(allocations[243]);
		allocator.Free(allocations[5]);
		allocator.Free(allocations[123]);
		allocator.Free(allocations[95]);

		// Free four contiguous slots (allocator must merge)
		allocator.Free(allocations[151]);
		allocator.Free(allocations[152]);
		allocator.Free(allocations[153]);
		allocator.Free(allocations[154]);

		allocations[243] = allocator.Allocate(1024);
		allocations[5] = allocator.Allocate(1024);
		allocations[123] = allocator.Allocate(1024);
		allocations[95] = allocator.Allocate(1024);
		allocations[151] = allocator.Allocate(1024 * 4); // 4x larger
		CHECK(!allocations[243].IsEmpty());
		CHECK(!allocations[5].IsEmpty());
		CHECK(!allocations[123].IsEmpty());
		CHECK(!allocations[95].IsEmpty());
		CHECK(!allocations[151].IsEmpty());

		for (int i = 0; i < 256; i++)
		{
			if (i < 152 || i > 154)
				allocator.Free(allocations[i]);
		}

		auto report2 = allocator.GetStorageReport();
		CHECK(report2.TotalFreeSpace == 1024 * 256);
		CHECK(report2.LargestFreeRegion == 1024 * 256);

		// End: Validate that allocator has no fragmentation left. Should be 100% clean.
		auto validateAll = allocator.Allocate(256 * 1024);
		CHECK(validateAll.Offset == 0);
		allocator.Free(validateAll);
	}
};