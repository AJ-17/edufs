#pragma once
// Bitmap: one bit per block/inode. 0 = free, 1 = used.
// Bit n lives in byte n/8 at position n%8 (least significant bit first),
// the same convention ext2 uses.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace edufs {

class Bitmap {
public:
    Bitmap() = default;
    explicit Bitmap(size_t bits) : bits_(bits), bytes_((bits + 7) / 8, 0) {}
    Bitmap(size_t bits, std::vector<uint8_t> raw) : bits_(bits), bytes_(std::move(raw)) {
        bytes_.resize((bits + 7) / 8, 0);
        mask_padding();
    }

    size_t size() const { return bits_; }

    bool test(size_t n) const {
        if (n >= bits_) return false;
        return (bytes_[n / 8] >> (n % 8)) & 1u;
    }
    bool set(size_t n) {
        if (n >= bits_) return false;
        bytes_[n / 8] |= static_cast<uint8_t>(1u << (n % 8));
        return true;
    }
    bool clear(size_t n) {
        if (n >= bits_) return false;
        bytes_[n / 8] &= static_cast<uint8_t>(~(1u << (n % 8)));
        return true;
    }

    // First zero bit at or after `start`. Padding bits past size() are never returned.
    std::optional<size_t> find_first_zero(size_t start = 0) const {
        size_t n = start;
        while (n < bits_) {
            if (n % 8 == 0 && bytes_[n / 8] == 0xFF) { n += 8; continue; }  // skip full bytes
            if (!test(n)) return n;
            n++;
        }
        return std::nullopt;
    }

    std::optional<size_t> alloc(size_t start = 0) {
        auto n = find_first_zero(start);
        if (n) set(*n);
        return n;
    }

    // Returns false on out-of-range or double free.
    bool free(size_t n) {
        if (n >= bits_ || !test(n)) return false;
        clear(n);
        return true;
    }

    size_t count_used() const {
        size_t c = 0;
        for (uint8_t b : bytes_) c += static_cast<size_t>(std::popcount(b));
        return c;
    }
    size_t count_free() const { return bits_ - count_used(); }

    const std::vector<uint8_t>& raw() const { return bytes_; }

private:
    void mask_padding() {  // padding bits past size() must read as 0
        if (bits_ % 8 && !bytes_.empty())
            bytes_.back() &= static_cast<uint8_t>((1u << (bits_ % 8)) - 1u);
    }
    size_t bits_ = 0;
    std::vector<uint8_t> bytes_;
};

}  // namespace edufs