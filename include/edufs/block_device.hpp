#pragma once
// BlockDevice: a disk image file viewed as an array of equal-size blocks.
//   byte_offset = block_number * block_size
// Everything above this layer is built from these reads and writes.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "edufs/errors.hpp"

namespace edufs {

class BlockDevice {
public:
    BlockDevice() = default;
    ~BlockDevice() { close(); }
    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;

    // Create (or overwrite) a zero-filled image of exactly size_bytes.
    static Err create(const std::string& path, uint64_t size_bytes);

    Err open(const std::string& path, uint32_t block_size);
    void close();
    bool is_open() const { return file_.is_open(); }

    uint32_t block_size() const { return block_size_; }
    uint64_t size_bytes() const { return size_bytes_; }
    uint64_t block_count() const { return size_bytes_ / block_size_; }
    Err set_block_size(uint32_t bs);

    Err read_block(uint32_t n, uint8_t* out);
    Err write_block(uint32_t n, const uint8_t* in);
    Err read_block(uint32_t n, std::vector<uint8_t>& out);
    Err write_block(uint32_t n, const std::vector<uint8_t>& in);  // size must equal block_size

    // Byte-granular access (bounds-checked). Used for the superblock and inode slots.
    Err read_bytes(uint64_t offset, void* buf, size_t len);
    Err write_bytes(uint64_t offset, const void* buf, size_t len);

    Err flush();

    // Statistics, handy for tests and for understanding I/O cost.
    uint64_t reads() const { return reads_; }
    uint64_t writes() const { return writes_; }

    // Fault injection for crash tests (used from Part C): after `n` more successful
    // write calls, every write fails with Err::Io. A negative value disables it.
    void fail_writes_after(int64_t n) { fail_after_ = n; }

private:
    bool allow_write();

    std::fstream file_;
    uint32_t block_size_ = 1024;
    uint64_t size_bytes_ = 0;
    uint64_t reads_ = 0, writes_ = 0;
    int64_t fail_after_ = -1;
};

}  // namespace edufs