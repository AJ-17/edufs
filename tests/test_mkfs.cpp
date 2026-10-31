#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "doctest.h"
#include "edufs/fs.hpp"
#include "test_util.hpp"

using namespace edufs;
namespace fsys = std::filesystem;

namespace {
FormatOptions opts(uint64_t size, uint32_t bs = 1024) {
    FormatOptions o;
    o.size_bytes = size;
    o.block_size = bs;
    return o;
}
constexpr uint64_t sb_off(size_t field) { return kSuperblockOffset + field; }
}  // namespace

TEST_CASE("format creates an image of the right size with the magic at byte 1024") {
    TempImage img;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);
    CHECK(fsys::file_size(img.path()) == (1u << 20));

    auto magic = peek_bytes(img.path(), 1024, 4);
    CHECK(magic[0] == 'E');
    CHECK(magic[1] == 'D');
    CHECK(magic[2] == 'F');
    CHECK(magic[3] == 'S');

    for (uint8_t b : peek_bytes(img.path(), 0, 1024)) REQUIRE(b == 0);  // boot area untouched
}

TEST_CASE("mount reads back exactly what format computed") {
    TempImage img;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);
    auto expected = make_superblock(opts(1 << 20));
    REQUIRE(expected.ok());

    Fs fs;
    REQUIRE(fs.mount(img.path()) == Err::Ok);
    const Superblock& s = fs.super();
    CHECK(s.total_blocks == expected.value.total_blocks);
    CHECK(s.total_inodes == expected.value.total_inodes);
    CHECK(s.data_start == expected.value.data_start);
    CHECK(s.free_blocks + 1 == expected.value.free_blocks); // root directory owns one data block at format time
    CHECK(s.free_inodes + 2 == expected.value.free_inodes); // inode 1 and root inode are reserved
    CHECK(s.it_start == expected.value.it_start);
    CHECK(fs.device().block_size() == 1024);
}

TEST_CASE("every supported block size formats and mounts") {
    for (uint32_t bs : {1024u, 2048u, 4096u}) {
        INFO("block size " << bs);
        TempImage img;
        REQUIRE(Fs::format(img.path(), opts(2 << 20, bs)) == Err::Ok);
        Fs fs;
        REQUIRE(fs.mount(img.path()) == Err::Ok);
        CHECK(fs.super().block_size == bs);
        CHECK(fs.device().block_size() == bs);
        CHECK(fs.device().block_count() == (2u << 20) / bs);
    }
}

TEST_CASE("format validates before touching the disk") {
    TempImage img;
    CHECK(Fs::format(img.path(), opts(1 << 20, 1000)) == Err::Invalid);
    CHECK_FALSE(fsys::exists(img.path()));  // nothing was created

    CHECK(Fs::format(img.path(), opts(100)) == Err::Invalid);
    CHECK_FALSE(fsys::exists(img.path()));
}

TEST_CASE("format overwrites whatever was in the file before") {
    TempImage img;
    {
        std::ofstream f(img.path(), std::ios::binary);
        std::string junk(200 * 1024, '\xFF');
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    REQUIRE(Fs::format(img.path(), opts(128 * 1024)) == Err::Ok);
    CHECK(fsys::file_size(img.path()) == 128 * 1024);
    Fs fs;
    CHECK(fs.mount(img.path()) == Err::Ok);
    for (uint8_t b : peek_bytes(img.path(), 0, 1024)) REQUIRE(b == 0);
}

TEST_CASE("mount and unmount track clean state and mount count") {
    TempImage img;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);

    for (uint32_t expected = 1; expected <= 3; expected++) {
        Fs fs;
        REQUIRE(fs.mount(img.path()) == Err::Ok);
        CHECK(fs.was_clean());
        CHECK(fs.super().mount_count == expected);
        CHECK(fs.super().state == kStateDirty);  // in use while mounted
        CHECK(fs.mounted());
        // on disk it is also marked dirty right now
        CHECK(peek_bytes(img.path(), sb_off(offsetof(Superblock, state)), 4)[0] == kStateDirty);
    }  // destructor unmounts

    Fs fs;
    REQUIRE(fs.mount(img.path()) == Err::Ok);
    CHECK(fs.super().mount_count == 4);
    CHECK(fs.unmount() == Err::Ok);
    CHECK_FALSE(fs.mounted());
    CHECK(peek_bytes(img.path(), sb_off(offsetof(Superblock, state)), 4)[0] == kStateClean);
    CHECK(fs.unmount() == Err::Ok);  // unmounting twice is harmless
}

TEST_CASE("an image copied while mounted looks like a crash") {
    TempImage img, crashed;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);
    {
        Fs fs;
        REQUIRE(fs.mount(img.path()) == Err::Ok);
        fsys::copy_file(img.path(), crashed.path());  // power cut: no unmount happened
    }
    Fs fs2;
    REQUIRE(fs2.mount(crashed.path()) == Err::Ok);
    CHECK_FALSE(fs2.was_clean());
}

TEST_CASE("mounting twice is refused") {
    TempImage img;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);
    Fs fs;
    REQUIRE(fs.mount(img.path()) == Err::Ok);
    CHECK(fs.mount(img.path()) == Err::Invalid);
    CHECK(fs.mounted());
}

TEST_CASE("the magic number protects against mounting the wrong thing") {
    TempImage img;
    Fs fs;

    SUBCASE("missing file") { CHECK(fs.mount(img.path()) == Err::NotFound); }
    SUBCASE("tiny file") {
        std::ofstream(img.path(), std::ios::binary) << "hello";
        CHECK(fs.mount(img.path()) == Err::BadMagic);
    }
    SUBCASE("text file") {
        std::ofstream f(img.path(), std::ios::binary);
        for (int i = 0; i < 1000; i++) f << "this is definitely not a filesystem\n";
        f.close();
        CHECK(fs.mount(img.path()) == Err::BadMagic);
    }
    SUBCASE("unformatted zero-filled image") {
        REQUIRE(BlockDevice::create(img.path(), 1 << 20) == Err::Ok);
        CHECK(fs.mount(img.path()) == Err::BadMagic);
    }
    CHECK_FALSE(fs.mounted());
}

TEST_CASE("a damaged superblock is detected as corrupt, and mount changes nothing") {
    TempImage img;
    REQUIRE(Fs::format(img.path(), opts(1 << 20)) == Err::Ok);
    Fs fs;

    SUBCASE("impossible block size") {
        poke_u32(img.path(), sb_off(offsetof(Superblock, block_size)), 3000);
    }
    SUBCASE("claims more blocks than the image holds") {
        poke_u32(img.path(), sb_off(offsetof(Superblock, total_blocks)), 5000000);
    }
    SUBCASE("unknown version") { poke_u32(img.path(), sb_off(offsetof(Superblock, version)), 7); }
    SUBCASE("layout does not add up") {
        poke_u32(img.path(), sb_off(offsetof(Superblock, data_start)), 5);
    }
    SUBCASE("image truncated after formatting") {
        fsys::resize_file(img.path(), 512 * 1024);
    }

    const auto before = peek_bytes(img.path(), 0, 4096);
    CHECK(fs.mount(img.path()) == Err::Corrupt);
    CHECK_FALSE(fs.mounted());
    CHECK(peek_bytes(img.path(), 0, 4096) == before);  // refused mount wrote nothing
}