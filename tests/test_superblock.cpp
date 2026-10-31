#include <algorithm>
#include <cstddef>

#include "doctest.h"
#include "edufs/superblock.hpp"

using namespace edufs;

namespace {
FormatOptions opts(uint64_t size, uint32_t bs, uint32_t inodes = 0) {
    FormatOptions o;
    o.size_bytes = size;
    o.block_size = bs;
    o.inodes = inodes;
    return o;
}
}  // namespace

TEST_CASE("on-disk struct sizes are exactly what the format promises") {
    CHECK(sizeof(Superblock) == 256);
    CHECK(offsetof(Superblock, magic) == 0);
    CHECK(offsetof(Superblock, block_size) == 8);
    CHECK(offsetof(Superblock, data_start) == 56);
}

TEST_CASE("the magic number spells EDFS in the file") {
    const uint32_t m = kMagic;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&m);
    CHECK(p[0] == 'E');
    CHECK(p[1] == 'D');
    CHECK(p[2] == 'F');
    CHECK(p[3] == 'S');
}

TEST_CASE("layout of a 1 MB image with 1 KB blocks (worked by hand in the lesson)") {
    auto r = make_superblock(opts(1 << 20, 1024));
    REQUIRE(r.ok());
    const Superblock& s = r.value;
    CHECK(s.magic == kMagic);
    CHECK(s.total_blocks == 1024);
    CHECK(s.total_inodes == 256);
    CHECK(s.bb_start == 2);   // block 0 = boot, block 1 = superblock
    CHECK(s.bb_blocks == 1);  // 1024 bits fit in one 8192-bit bitmap block
    CHECK(s.ib_start == 3);
    CHECK(s.ib_blocks == 1);
    CHECK(s.it_start == 4);
    CHECK(s.it_blocks == 32);  // 256 inodes * 128 bytes / 1024
    CHECK(s.data_start == 36);
    CHECK(s.free_blocks == 1024 - 36);
    CHECK(s.free_inodes == 256);
    CHECK(s.root_inode == 2);
    CHECK(s.state == kStateClean);
}

TEST_CASE("larger blocks put the superblock inside block 0") {
    SUBCASE("2 KB") {
        auto r = make_superblock(opts(1 << 20, 2048));
        REQUIRE(r.ok());
        CHECK(r.value.total_blocks == 512);
        CHECK(r.value.bb_start == 1);
        CHECK(r.value.it_blocks == 8);  // 128 inodes / 16 per block
        CHECK(r.value.data_start == 11);
    }
    SUBCASE("4 KB") {
        auto r = make_superblock(opts(1 << 20, 4096));
        REQUIRE(r.ok());
        CHECK(r.value.total_blocks == 256);
        CHECK(r.value.bb_start == 1);
        CHECK(r.value.it_blocks == 2);  // 64 inodes / 32 per block
        CHECK(r.value.data_start == 5);
    }
}

TEST_CASE("inode count is rounded up to fill whole inode-table blocks") {
    auto r = make_superblock(opts(1 << 20, 1024, 100));
    REQUIRE(r.ok());
    CHECK(r.value.total_inodes == 104);  // 100 -> next multiple of 8
    CHECK(r.value.it_blocks == 13);
}

TEST_CASE("make_superblock rejects bad requests") {
    CHECK(make_superblock(opts(1 << 20, 512)).err == Err::Invalid);
    CHECK(make_superblock(opts(1 << 20, 1000)).err == Err::Invalid);
    CHECK(make_superblock(opts(1 << 20, 8192)).err == Err::Invalid);
    CHECK(make_superblock(opts(1 << 20, 0)).err == Err::Invalid);
    CHECK(make_superblock(opts(1000, 1024)).err == Err::Invalid);      // far too small
    CHECK(make_superblock(opts(64 * 1024 - 1, 1024)).err == Err::Invalid);
    // 64 KB image but 5000 inodes: the inode table alone would not fit.
    CHECK(make_superblock(opts(64 * 1024, 1024, 5000)).err == Err::NoSpace);
}

TEST_CASE("smallest allowed image works") {
    auto r = make_superblock(opts(64 * 1024, 1024));
    REQUIRE(r.ok());
    CHECK(r.value.total_blocks == 64);
    CHECK(r.value.total_inodes == 32);
    CHECK(r.value.data_start == 8);
}

TEST_CASE("layout invariants hold for many sizes, block sizes and inode counts") {
    for (uint64_t size : {64ull << 10, 100ull << 10, 1ull << 20, (3ull << 20) + 12345, 10ull << 20, 64ull << 20}) {
        for (uint32_t bs : {1024u, 2048u, 4096u}) {
            for (uint32_t inodes : {0u, 1u, 100u, 5000u}) {
                INFO("size=" << size << " bs=" << bs << " inodes=" << inodes);
                auto r = make_superblock(opts(size, bs, inodes));
                if (!r.ok()) {
                    CHECK((r.err == Err::NoSpace || r.err == Err::Invalid || r.err == Err::Unsupported));
                    continue;
                }
                const Superblock& s = r.value;
                // Group 0 keeps the primary metadata in the familiar order.
                CHECK(s.bb_start * uint64_t(bs) >= kSuperblockOffset + sizeof(Superblock));
                CHECK(s.ib_start == s.bb_start + 1);
                CHECK(s.it_start == s.ib_start + 1);
                CHECK(s.data_start == s.it_start + s.it_blocks);
                CHECK(s.data_start < s.total_blocks);
                CHECK(s.bb_blocks == 1);
                CHECK(s.ib_blocks == 1);
                CHECK(s.blocks_per_group <= bs * 8u);
                CHECK(s.group_count * s.inodes_per_group == s.total_inodes);
                CHECK(s.it_blocks * (bs / kInodeSize) == s.inodes_per_group);
                uint64_t expected_free = 0;
                for (uint32_t g = 0; g < s.group_count; ++g) {
                    uint32_t first = g * s.blocks_per_group;
                    uint32_t count = std::min<uint32_t>(s.blocks_per_group, s.total_blocks - first);
                    uint32_t data_first = g == 0 ? s.data_start : first + 2 + s.it_blocks;
                    expected_free += first + count - data_first;
                }
                CHECK(s.free_blocks == expected_free);
                CHECK(uint64_t(s.total_blocks) * bs <= size);
                CHECK(validate_superblock(s, size) == Err::Ok);
            }
        }
    }
}

TEST_CASE("validate_superblock spots every kind of damage") {
    const uint64_t size = 1 << 20;
    auto r = make_superblock(opts(size, 1024));
    REQUIRE(r.ok());
    Superblock s = r.value;
    REQUIRE(validate_superblock(s, size) == Err::Ok);

    SUBCASE("wrong magic means not our filesystem") {
        s.magic = 0x12345678;
        CHECK(validate_superblock(s, size) == Err::BadMagic);
    }
    SUBCASE("zeroed superblock is also bad magic") {
        s = Superblock{};
        CHECK(validate_superblock(s, size) == Err::BadMagic);
    }
    SUBCASE("image smaller than the filesystem claims") { CHECK(validate_superblock(s, size / 2) == Err::Corrupt); }
    SUBCASE("version") { s.version = 99; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("block size") { s.block_size = 3000; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("inode size") { s.inode_size = 64; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("zero blocks") { s.total_blocks = 0; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("too few inodes") { s.total_inodes = 1; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("root inode") { s.root_inode = 5; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("block bitmap start") { s.bb_start++; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("block bitmap size") { s.bb_blocks++; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("inode bitmap start") { s.ib_start++; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("inode table start") { s.it_start--; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("inode table size") { s.it_blocks--; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("data start") { s.data_start = s.total_blocks; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("free blocks exceed total") { s.free_blocks = s.total_blocks + 1; CHECK(validate_superblock(s, size) == Err::Corrupt); }
    SUBCASE("free inodes exceed total") { s.free_inodes = s.total_inodes + 1; CHECK(validate_superblock(s, size) == Err::Corrupt); }
}