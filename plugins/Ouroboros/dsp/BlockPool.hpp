#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ouro {

// Fixed-size block allocator. reserve() is the only allocating call (non-RT);
// everything else is RT-safe and single-threaded (audio thread).
class BlockPool {
public:
    static constexpr uint32_t kBlock = 4096;   // samples per block (16 KiB)

    BlockPool() = default;
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;
    ~BlockPool() { release(); }

    bool reserve(size_t maxBytes) {
        release();
        size_t n = maxBytes / (kBlock * sizeof(float));
        if (n > size_t(INT32_MAX)) n = INT32_MAX;
        if (n == 0) return false;
        data_ = static_cast<float*>(std::calloc(n, kBlock * sizeof(float)));
        next_ = static_cast<int32_t*>(std::calloc(n, sizeof(int32_t)));
        freeList_ = static_cast<int32_t*>(std::calloc(n, sizeof(int32_t)));
        if (!data_ || !next_ || !freeList_) { release(); return false; }
        reserved_ = uint32_t(n);
        budget_ = reserved_;
        return true;
    }

    void setBudgetBytes(size_t bytes) {
        size_t n = bytes / (kBlock * sizeof(float));
        budget_ = n < reserved_ ? uint32_t(n) : reserved_;
    }

    int32_t alloc() {
        if (used_ >= budget_) return -1;
        int32_t b;
        if (freeCount_ > 0) b = freeList_[--freeCount_];
        else if (highWater_ < reserved_) b = int32_t(highWater_++);
        else return -1;
        std::memset(data(b), 0, kBlock * sizeof(float));
        next_[b] = -1;
        ++used_;
        return b;
    }

    void free(int32_t b) { freeList_[freeCount_++] = b; --used_; }

    float* data(int32_t b) { return data_ + size_t(b) * kBlock; }
    const float* data(int32_t b) const { return data_ + size_t(b) * kBlock; }
    int32_t& next(int32_t b) { return next_[b]; }
    int32_t next(int32_t b) const { return next_[b]; }
    uint32_t usedBlocks() const { return used_; }
    uint32_t budgetBlocks() const { return budget_; }

private:
    void release() {
        std::free(data_); std::free(next_); std::free(freeList_);
        data_ = nullptr; next_ = nullptr; freeList_ = nullptr;
        reserved_ = budget_ = used_ = highWater_ = freeCount_ = 0;
    }
    float*   data_ = nullptr;
    int32_t* next_ = nullptr;
    int32_t* freeList_ = nullptr;
    uint32_t reserved_ = 0, budget_ = 0, used_ = 0, highWater_ = 0, freeCount_ = 0;
};

} // namespace ouro
