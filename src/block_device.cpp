#include "edufs/block_device.hpp"

#include <filesystem>

namespace edufs {

Err BlockDevice::create(const std::string& path, uint64_t size_bytes) {
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return Err::Io;
    }
    std::error_code ec;
    std::filesystem::resize_file(path, size_bytes, ec);  // extends with zero bytes
    return ec ? Err::Io : Err::Ok;
}

Err BlockDevice::open(const std::string& path, uint32_t block_size) {
    close();
    if (block_size == 0) return Err::Invalid;
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return Err::NotFound;
    file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) return Err::Io;
    block_size_ = block_size;
    size_bytes_ = sz;
    fail_after_ = -1;
    return Err::Ok;
}

void BlockDevice::close() {
    if (file_.is_open()) file_.close();
    file_.clear();
}

Err BlockDevice::set_block_size(uint32_t bs) {
    if (bs == 0) return Err::Invalid;
    block_size_ = bs;
    return Err::Ok;
}

Err BlockDevice::read_bytes(uint64_t offset, void* buf, size_t len) {
    if (!is_open()) return Err::Io;
    if (offset + len > size_bytes_) return Err::Io;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(static_cast<char*>(buf), static_cast<std::streamsize>(len));
    if (!file_) { file_.clear(); return Err::Io; }
    reads_++;
    return Err::Ok;
}

bool BlockDevice::allow_write() {
    if (fail_after_ < 0) return true;
    if (fail_after_ == 0) return false;
    fail_after_--;
    return true;
}

Err BlockDevice::write_bytes(uint64_t offset, const void* buf, size_t len) {
    if (!is_open()) return Err::Io;
    if (offset + len > size_bytes_) return Err::Io;
    if (!allow_write()) return Err::Io;
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(static_cast<const char*>(buf), static_cast<std::streamsize>(len));
    file_.flush();
    if (!file_) { file_.clear(); return Err::Io; }
    writes_++;
    return Err::Ok;
}

Err BlockDevice::read_block(uint32_t n, uint8_t* out) {
    if (n >= block_count()) return Err::Io;
    return read_bytes(static_cast<uint64_t>(n) * block_size_, out, block_size_);
}

Err BlockDevice::write_block(uint32_t n, const uint8_t* in) {
    if (n >= block_count()) return Err::Io;
    return write_bytes(static_cast<uint64_t>(n) * block_size_, in, block_size_);
}

Err BlockDevice::read_block(uint32_t n, std::vector<uint8_t>& out) {
    out.resize(block_size_);
    return read_block(n, out.data());
}

Err BlockDevice::write_block(uint32_t n, const std::vector<uint8_t>& in) {
    if (in.size() != block_size_) return Err::Invalid;
    return write_block(n, in.data());
}

Err BlockDevice::flush() {
    if (!is_open()) return Err::Io;
    file_.flush();
    return file_ ? Err::Ok : Err::Io;
}

}  // namespace edufs