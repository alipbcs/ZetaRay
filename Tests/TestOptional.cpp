#include <Utility/Optional.h>
#include <App/App.h>
#include <doctest/doctest.h>

#include <optional>

using namespace ZetaRay::Util;
using namespace ZetaRay::App;

struct NonTrivial
{
	NonTrivial()
		: val(0)
	{}
	NonTrivial(int v)
		: val(v)
	{}
	~NonTrivial()
	{
		val = -1;
	}
	NonTrivial(const NonTrivial& other)
		: val(other.val)
	{}
	NonTrivial& operator=(NonTrivial&& other)
	{
		val = other.val;
		other.val = -1;

		return *this;
	}

	int val;
};

TEST_SUITE("Optional")
{
	TEST_CASE("Basic")
	{
		Optional<NonTrivial> a;
		CHECK(!a);

		a = 5;
		CHECK(a);
		CHECK(a.value().val == 5);

		Optional<NonTrivial> b = NonTrivial();
		CHECK(b.value().val == 0);
	}

	TEST_CASE("Copy constructor/assignment")
	{
		Optional<NonTrivial> a(13);
		Optional<NonTrivial> b(27);

		a = b;
		CHECK(a.value().val == 27);
		CHECK(b.value().val == 27);

		Optional<NonTrivial> c(a);
		CHECK(c.value().val == 27);
	}

	TEST_CASE("Move constructor/assignment")
	{
		Optional<NonTrivial> a(13);
		Optional<NonTrivial> b(27);

		a = ZetaMove(b);
		CHECK(a.value().val == 27);
		CHECK(!b);

		Optional<NonTrivial> c(ZetaMove(a));
		CHECK(c.value().val == 27);
		CHECK(!a);

		Optional<NonTrivial> o2;
		
		{
			NonTrivial a1(54);
			o2 = ZetaMove(a1);
		}

		CHECK(o2.value().val == 54);
	}

	TEST_CASE("Reset")
	{
		Optional<NonTrivial> o1(NonTrivial(13));
		CHECK(o1.value().val == 13);

		o1.reset();
		CHECK(!o1);
	}

	TEST_CASE("Null pointer")
	{
		int a = 45;

		Optional<int*> o1(&a);
		CHECK(o1);
		
		o1 = nullptr;
		CHECK(!o1);

		Optional<int*> o2(nullptr);
		CHECK(!o2);

		Optional<int*> o3(&a);
		CHECK(o3);
		o3 = ZetaMove(o2);
		CHECK(!o3);
	}
};