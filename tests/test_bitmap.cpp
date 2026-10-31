#include "doctest.h"
#include "edufs/bitmap.hpp"

using edufs::Bitmap;

TEST_CASE("new bitmap is entirely free") {
    Bitmap b(10);
    CHECK(b.count_free() == 10);
    CHECK(b.find_first_zero() == 0u);
}

TEST_CASE("set bits land where ext2 would put them") {
    Bitmap b(10);
    b.set(0);
    b.set(9);
    CHECK(b.raw()[0] == 0x01);
    CHECK(b.raw()[1] == 0x02);
}

TEST_CASE("alloc hands out lowest free bit, then runs out") {
    Bitmap b(3);
    CHECK(b.alloc() == 0u);
    CHECK(b.alloc() == 1u);
    CHECK(b.alloc() == 2u);
    CHECK_FALSE(b.alloc().has_value());
}

TEST_CASE("free and double free") {
    Bitmap b(10);
    for (int i = 0; i < 10; i++) b.alloc();
    SUBCASE("a freed hole is reused") {
        CHECK(b.free(4));
        CHECK(b.alloc() == 4u);
    }
    SUBCASE("double free is rejected") {
        CHECK(b.free(4));
        CHECK_FALSE(b.free(4));
    }
    SUBCASE("out of range is rejected") { CHECK_FALSE(b.free(10)); }
}

TEST_CASE("byte-boundary sizes allocate exactly size bits") {
    for (size_t size : {0u, 1u, 7u, 8u, 9u, 16u}) {
        INFO("size = " << size);
        Bitmap b(size);
        CHECK(b.raw().size() == (size + 7) / 8);
        size_t got = 0;
        while (b.alloc()) got++;
        CHECK(got == size);
    }
}

TEST_CASE("find_first_zero honours a start position") {
    Bitmap b(100);
    for (size_t i = 0; i < 64; i++) b.set(i);
    CHECK(b.find_first_zero() == 64u);
    CHECK(b.find_first_zero(70) == 70u);
}