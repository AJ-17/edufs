#include "doctest.h"
#include "edufs/fs.hpp"
#include "test_util.hpp"
using namespace edufs;

TEST_CASE("filesystem supports create, write, read, listing, stat and persistence") {
    TempImage img;
    REQUIRE(Fs::format(img.path()) == Err::Ok);
    {
        Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
        CHECK(fs.check() == Err::Ok);
        CHECK(fs.mkdir("/docs") == Err::Ok);
        CHECK(fs.write_file("/docs/hello.txt", "hello EduFS") == Err::Ok);
        auto data = fs.read_file("/docs/hello.txt"); REQUIRE(data); CHECK(data.value == "hello EduFS");
        auto items = fs.list("/docs"); REQUIRE(items); REQUIRE(items.value.size() == 1); CHECK(items.value[0].name == "hello.txt");
        auto st = fs.stat("/docs/hello.txt"); REQUIRE(st); CHECK(st.value.size == 11); CHECK((st.value.mode & kModeMask) == kModeRegular);
        CHECK(fs.check() == Err::Ok);
        CHECK(fs.unmount() == Err::Ok);
    }
    {
        Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
        auto data = fs.read_file("/docs/hello.txt"); REQUIRE(data); CHECK(data.value == "hello EduFS");
        CHECK(fs.check() == Err::Ok);
    }
}

TEST_CASE("filesystem rejects invalid deletes and removes empty directories") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    CHECK(fs.remove_file("/missing") == Err::NotFound);
    CHECK(fs.mkdir("/d") == Err::Ok);
    CHECK(fs.remove_dir("/d") == Err::Ok);
    CHECK(fs.remove_dir("/d") == Err::NotFound);
    CHECK(fs.touch("/f") == Err::Ok);
    CHECK(fs.remove_dir("/f") == Err::NotDir);
    CHECK(fs.remove_file("/f") == Err::Ok);
    CHECK(fs.check() == Err::Ok);
}

TEST_CASE("filesystem supports indirect blocks for files spanning many blocks") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    std::string data(20 * fs.super().block_size, 'x');
    REQUIRE(fs.write_file("/large", data) == Err::Ok);
    auto got = fs.read_file("/large"); REQUIRE(got); CHECK(got.value == data);
    CHECK(fs.check() == Err::Ok);
    CHECK(fs.remove_file("/large") == Err::Ok);
    CHECK(fs.check() == Err::Ok);
}

TEST_CASE("multiple block groups format, mount, inspect and persist correctly") {
    TempImage img;
    FormatOptions opt; opt.size_bytes = 10ull << 20; opt.block_size = 1024;
    REQUIRE(Fs::format(img.path(), opt) == Err::Ok);
    {
        Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
        CHECK(fs.super().group_count == 2);
        CHECK(fs.check() == Err::Ok);
        CHECK(fs.write_file("/grouped.txt", "stored in a multi-group image") == Err::Ok);
        CHECK(fs.check() == Err::Ok);
    }
    {
        Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
        CHECK(fs.super().group_count == 2);
        auto text = fs.read_file("/grouped.txt"); REQUIRE(text);
        CHECK(text.value == "stored in a multi-group image");
        CHECK(fs.check() == Err::Ok);
    }
}

TEST_CASE("truncate releases blocks and zero-extends files") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    REQUIRE(fs.write_file("/f", std::string(3000, 'z')) == Err::Ok);
    REQUIRE(fs.truncate_file("/f", 5) == Err::Ok);
    auto small = fs.read_file("/f"); REQUIRE(small); CHECK(small.value == "zzzzz");
    REQUIRE(fs.truncate_file("/f", 8) == Err::Ok);
    auto extended = fs.read_file("/f"); REQUIRE(extended); CHECK(extended.value == std::string("zzzzz\0\0\0", 8));
    CHECK(fs.check() == Err::Ok);
}

TEST_CASE("allocation crosses into the next block group") {
    TempImage img;
    FormatOptions opt; opt.size_bytes = 10ull << 20; opt.block_size = 1024;
    REQUIRE(Fs::format(img.path(), opt) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    // Group 0 has fewer than 4,860 free data blocks after metadata and root.
    const std::string payload(4860ull * 1024ull, 'G');
    REQUIRE(fs.write_file("/cross-group.bin", payload) == Err::Ok);
    auto st = fs.stat("/cross-group.bin"); REQUIRE(st); CHECK(st.value.size == payload.size());
    CHECK(fs.check() == Err::Ok);
    auto read = fs.read_file("/cross-group.bin"); REQUIRE(read); CHECK(read.value == payload);
}

TEST_CASE("hard links share an inode and unlink preserves data until last link") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    REQUIRE(fs.write_file("/original", "shared contents") == Err::Ok);
    REQUIRE(fs.hard_link("/original", "/alias") == Err::Ok);
    auto a = fs.stat("/original"); auto b = fs.stat("/alias");
    REQUIRE(a); REQUIRE(b); CHECK(a.value.inode == b.value.inode); CHECK(a.value.links == 2);
    CHECK(fs.remove_file("/original") == Err::Ok);
    auto data = fs.read_file("/alias"); REQUIRE(data); CHECK(data.value == "shared contents");
    auto st = fs.stat("/alias"); REQUIRE(st); CHECK(st.value.links == 1);
    CHECK(fs.check() == Err::Ok);
    CHECK(fs.remove_file("/alias") == Err::Ok);
    CHECK(fs.check() == Err::Ok);
}

TEST_CASE("append, chmod and rename work and persist") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    REQUIRE(fs.write_file("/before.txt", "hello") == Err::Ok);
    CHECK(fs.append_file("/before.txt", " world") == Err::Ok);
    auto contents = fs.read_file("/before.txt"); REQUIRE(contents); CHECK(contents.value == "hello world");
    CHECK(fs.chmod("/before.txt", 0600) == Err::Ok);
    auto st = fs.stat("/before.txt"); REQUIRE(st); CHECK((st.value.mode & 07777) == 0600);
    CHECK(fs.rename_file("/before.txt", "/after.txt") == Err::Ok);
    CHECK(fs.read_file("/before.txt").err == Err::NotFound);
    auto moved = fs.read_file("/after.txt"); REQUIRE(moved); CHECK(moved.value == "hello world");
    CHECK(fs.check() == Err::Ok);
}

TEST_CASE("hard links reject directories and duplicate destinations") {
    TempImage img; REQUIRE(Fs::format(img.path()) == Err::Ok);
    Fs fs; REQUIRE(fs.mount(img.path()) == Err::Ok);
    REQUIRE(fs.mkdir("/dir") == Err::Ok);
    CHECK(fs.hard_link("/dir", "/dir-link") == Err::Unsupported);
    REQUIRE(fs.write_file("/f", "x") == Err::Ok);
    CHECK(fs.hard_link("/f", "/f") == Err::Exists);
    CHECK(fs.check() == Err::Ok);
}
