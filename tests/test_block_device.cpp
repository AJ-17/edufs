#include <filesystem>
#include <fstream>
#include <vector>

#include "doctest.h"
#include "edufs/block_device.hpp"
#include "test_util.hpp"

using namespace edufs;

namespace {
// A block whose bytes all differ from their neighbours, so shifted or swapped data is detected.
std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(seed + i * 7);
    return v;
}
}  // namespace

TEST_CASE("create makes a zero-filled image of the exact size") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 4096) == Err::Ok);
    CHECK(std::filesystem::file_size(img.path()) == 4096);

    std::ifstream f(img.path(), std::ios::binary);
    std::vector<char> bytes(4096, 'x');
    f.read(bytes.data(), 4096);
    for (char c : bytes) REQUIRE(c == 0);
}

TEST_CASE("create overwrites an existing image") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 8192) == Err::Ok);
    REQUIRE(BlockDevice::create(img.path(), 2048) == Err::Ok);
    CHECK(std::filesystem::file_size(img.path()) == 2048);
}

TEST_CASE("open reports errors and computes block_count") {
    TempImage img;
    BlockDevice dev;

    SUBCASE("missing file") { CHECK(dev.open(img.path(), 1024) == Err::NotFound); }
    SUBCASE("zero block size") {
        REQUIRE(BlockDevice::create(img.path(), 4096) == Err::Ok);
        CHECK(dev.open(img.path(), 0) == Err::Invalid);
    }
    SUBCASE("exact multiple") {
        REQUIRE(BlockDevice::create(img.path(), 3 * 1024) == Err::Ok);
        REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
        CHECK(dev.block_count() == 3);
        CHECK(dev.block_size() == 1024);
    }
    SUBCASE("a partial trailing block does not count") {
        REQUIRE(BlockDevice::create(img.path(), 3000) == Err::Ok);
        REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
        CHECK(dev.block_count() == 2);
    }
    SUBCASE("block size changes the count") {
        REQUIRE(BlockDevice::create(img.path(), 1 << 20) == Err::Ok);
        REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
        CHECK(dev.block_count() == 1024);
        REQUIRE(dev.set_block_size(4096) == Err::Ok);
        CHECK(dev.block_count() == 256);
    }
}

TEST_CASE("blocks round trip") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 8 * 1024) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);

    for (uint32_t n : {0u, 3u, 7u}) {  // first, middle, last
        INFO("block " << n);
        auto w = pattern(1024, static_cast<uint8_t>(n + 1));
        REQUIRE(dev.write_block(n, w) == Err::Ok);
        std::vector<uint8_t> r;
        REQUIRE(dev.read_block(n, r) == Err::Ok);
        CHECK(r == w);
    }
}

TEST_CASE("writing a block leaves its neighbours untouched") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 5 * 1024) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);

    REQUIRE(dev.write_block(2, pattern(1024, 9)) == Err::Ok);
    std::vector<uint8_t> r, zeros(1024, 0);
    for (uint32_t n : {0u, 1u, 3u, 4u}) {
        REQUIRE(dev.read_block(n, r) == Err::Ok);
        CHECK(r == zeros);
    }
}

TEST_CASE("blocks land at block_number * block_size in the host file") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 8 * 1024) == Err::Ok);
    {
        BlockDevice dev;
        REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
        std::vector<uint8_t> blk(1024, 0);
        const char* hello = "HELLO";
        for (int i = 0; i < 5; i++) blk[i] = static_cast<uint8_t>(hello[i]);
        REQUIRE(dev.write_block(3, blk) == Err::Ok);
    }  // device closed here

    std::ifstream f(img.path(), std::ios::binary);
    f.seekg(3 * 1024);  // the offset formula from P3
    char got[6] = {0};
    f.read(got, 5);
    CHECK(std::string(got) == "HELLO");
}

TEST_CASE("bounds and size checks") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 4 * 1024) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
    std::vector<uint8_t> r, w(1024, 1);

    SUBCASE("last valid block works, one past the end fails") {
        CHECK(dev.read_block(3, r) == Err::Ok);
        CHECK(dev.read_block(4, r) == Err::Io);
        CHECK(dev.write_block(4, w) == Err::Io);
        CHECK(dev.read_block(0xFFFFFFFFu, r) == Err::Io);
    }
    SUBCASE("wrong-size buffers are rejected") {
        CHECK(dev.write_block(0, std::vector<uint8_t>(100, 1)) == Err::Invalid);
        CHECK(dev.write_block(0, std::vector<uint8_t>(2048, 1)) == Err::Invalid);
        CHECK(dev.write_block(0, std::vector<uint8_t>{}) == Err::Invalid);
    }
    SUBCASE("a failed write changes nothing") {
        CHECK(dev.write_block(4, w) == Err::Io);
        std::vector<uint8_t> zeros(1024, 0);
        for (uint32_t n = 0; n < 4; n++) {
            REQUIRE(dev.read_block(n, r) == Err::Ok);
            CHECK(r == zeros);
        }
    }
}

TEST_CASE("byte-level access is bounds-checked") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 2048) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);

    uint32_t v = 0xDEADBEEF, back = 0;
    REQUIRE(dev.write_bytes(1500, &v, 4) == Err::Ok);
    REQUIRE(dev.read_bytes(1500, &back, 4) == Err::Ok);
    CHECK(back == 0xDEADBEEF);

    CHECK(dev.write_bytes(2046, &v, 4) == Err::Io);  // crosses the end
    CHECK(dev.read_bytes(2048, &back, 1) == Err::Io);
    CHECK(dev.read_bytes(2047, &back, 1) == Err::Ok);  // last byte is fine
}

TEST_CASE("data survives close and reopen") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 4 * 1024) == Err::Ok);
    auto w = pattern(1024, 42);
    {
        BlockDevice dev;
        REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
        REQUIRE(dev.write_block(2, w) == Err::Ok);
    }
    BlockDevice dev2;
    REQUIRE(dev2.open(img.path(), 1024) == Err::Ok);
    std::vector<uint8_t> r;
    REQUIRE(dev2.read_block(2, r) == Err::Ok);
    CHECK(r == w);
}

TEST_CASE("a closed device refuses I/O") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 2048) == Err::Ok);
    BlockDevice dev;
    CHECK_FALSE(dev.is_open());
    std::vector<uint8_t> r;
    CHECK(dev.read_block(0, r) == Err::Io);

    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
    CHECK(dev.is_open());
    dev.close();
    CHECK_FALSE(dev.is_open());
    CHECK(dev.read_block(0, r) == Err::Io);
}

TEST_CASE("I/O counters count successful operations only") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 2048) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
    std::vector<uint8_t> r, w(1024, 5);
    dev.write_block(0, w);
    dev.write_block(1, w);
    dev.read_block(0, r);
    dev.read_block(99, r);  // out of range: not counted
    CHECK(dev.writes() == 2);
    CHECK(dev.reads() == 1);
}

TEST_CASE("fault injection stops writes after N successes (used for crash tests later)") {
    TempImage img;
    REQUIRE(BlockDevice::create(img.path(), 4 * 1024) == Err::Ok);
    BlockDevice dev;
    REQUIRE(dev.open(img.path(), 1024) == Err::Ok);
    std::vector<uint8_t> w(1024, 0xAA), r;

    dev.fail_writes_after(2);
    CHECK(dev.write_block(0, w) == Err::Ok);
    CHECK(dev.write_block(1, w) == Err::Ok);
    CHECK(dev.write_block(2, w) == Err::Io);  // the "power cut"
    CHECK(dev.write_block(3, w) == Err::Io);

    REQUIRE(dev.read_block(1, r) == Err::Ok);
    CHECK(r == w);
    REQUIRE(dev.read_block(2, r) == Err::Ok);
    CHECK(r == std::vector<uint8_t>(1024, 0));  // block 2 never reached the disk

    dev.fail_writes_after(-1);  // disabled again
    CHECK(dev.write_block(2, w) == Err::Ok);
}