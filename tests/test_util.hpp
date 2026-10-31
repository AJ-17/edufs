#pragma once
// Test helpers shared by all test files.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

// A throw-away disk image path. The file (if any) is deleted when the object dies,
// even when a test fails, because destructors always run (RAII).
class TempImage {
public:
    explicit TempImage(const std::string& tag = "img") {
        static std::atomic<int> counter{0};
        path_ = (std::filesystem::temp_directory_path() /
                 ("edufs_" + tag + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".img"))
                    .string();
    }
    ~TempImage() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempImage(const TempImage&) = delete;
    TempImage& operator=(const TempImage&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

#include <cstdint>
#include <vector>

// Raw access to an image file, bypassing EduFS. Used to inject corruption and to look at
// the bytes exactly as a hexdump would.
inline void poke_bytes(const std::string& path, uint64_t offset, const void* data, size_t n) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}

inline void poke_u32(const std::string& path, uint64_t offset, uint32_t v) {
    poke_bytes(path, offset, &v, 4);
}

inline std::vector<uint8_t> peek_bytes(const std::string& path, uint64_t offset, size_t n) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> v(n);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
    return v;
}