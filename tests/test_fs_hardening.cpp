#include "doctest.h"
#include "edufs/fs.hpp"
#include "test_util.hpp"
#include <string>
using namespace edufs;

TEST_CASE("filesystem randomized create-write-read-delete sequence stays consistent") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    for (int i = 0; i < 40; ++i) {
        const std::string path = "/file" + std::to_string(i);
        const std::string content = "payload-" + std::to_string(i) + std::string(static_cast<size_t>(i * 17), char('a' + (i % 26)));
        REQUIRE(fs.write_file(path, content) == Err::Ok);
        auto read = fs.read_file(path); REQUIRE(read); CHECK(read.value == content);
        CHECK(fs.check() == Err::Ok);
    }
    for (int i = 0; i < 40; i += 2) CHECK(fs.remove_file("/file" + std::to_string(i)) == Err::Ok);
    CHECK(fs.check() == Err::Ok);
    for (int i = 1; i < 40; i += 2) CHECK(fs.read_file("/file" + std::to_string(i)).ok());
}

TEST_CASE("fsck detects an allocated but unreferenced data block") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    uint32_t bb_start = 0, bs = 0, data_start = 0;
    {
        Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
        auto before = fs.space(); REQUIRE(before.free_blocks > 0);
        bb_start = fs.super().bb_start; bs = fs.super().block_size; data_start = fs.super().data_start;
        REQUIRE(fs.unmount() == Err::Ok);
    }
    // Reserve a data block in the on-disk bitmap without giving it to an inode: simulate corruption.
    std::vector<uint8_t> byte(1);
    { std::ifstream f(img.path(), std::ios::binary); f.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(bb_start) * bs + (data_start + 1) / 8)); f.read(reinterpret_cast<char*>(byte.data()), 1); }
    byte[0] |= static_cast<uint8_t>(1u << ((data_start + 1) % 8));
    { std::fstream f(img.path(), std::ios::binary | std::ios::in | std::ios::out); f.seekp(static_cast<std::streamoff>(static_cast<uint64_t>(bb_start) * bs + (data_start + 1) / 8)); f.write(reinterpret_cast<const char*>(byte.data()), 1); }
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    CHECK(fs.check() == Err::Corrupt);
}

TEST_CASE("disk-full write fails without corrupting existing metadata") {
    TempImage img;
    FormatOptions opt; opt.size_bytes = 64ull << 10; opt.block_size = 1024; opt.inodes = 32;
    REQUIRE(Fs::format(img.path(), opt) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    CHECK(fs.write_file("/too-big", std::string(2ull << 20, 'X')) == Err::NoSpace);
    CHECK(fs.check() == Err::Ok);
    auto empty = fs.read_file("/too-big"); REQUIRE(empty); CHECK(empty.value.empty());
}

TEST_CASE("inode exhaustion returns NoInodes cleanly") {
    TempImage img;
    FormatOptions opt; opt.size_bytes = 64ull << 10; opt.block_size = 1024; opt.inodes = 32;
    REQUIRE(Fs::format(img.path(), opt) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    for (int i = 0; i < 30; ++i) REQUIRE(fs.touch("/f" + std::to_string(i)) == Err::Ok);
    CHECK(fs.touch("/out-of-inodes") == Err::NoInodes);
    CHECK(fs.check() == Err::Ok);
}
