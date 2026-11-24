#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ouro {

// Fixed-size block allocator. reserve() is the only allocating call (non-RT);
// everything else is RT-safe and single-threaded (audio thread).
// Undo blocks (deltas) count toward the budget, Audio blocks (track audio) only toward the reserve. A block can have
// several owners (addRef): free() drops one, and the last one gives the block back.
class BlockPool {
public:
    static constexpr uint32_t kBlock = 4096;   // samples per block (16 KiB)
    enum class Kind : uint8_t { Undo, Audio };

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
        meta_ = static_cast<Meta*>(std::calloc(n, sizeof(Meta)));
        freeList_ = static_cast<int32_t*>(std::calloc(n, sizeof(int32_t)));
        if (!data_ || !meta_ || !freeList_) { release(); return false; }
        reserved_ = uint32_t(n);
        budget_ = reserved_;
        return true;
    }

    void setBudgetBytes(size_t bytes) {
        size_t n = bytes / (kBlock * sizeof(float));
        budget_ = n < reserved_ ? uint32_t(n) : reserved_;
    }

    // -1 when refused: an Undo block over the budget, or no block left (an Audio one then sets full()).
    int32_t alloc(Kind k = Kind::Undo) {
        const bool undo = k == Kind::Undo;
        if (undo && undoUsed_ >= budget_) return -1;
        int32_t b;
        if (freeCount_ > 0) b = freeList_[--freeCount_];
        else if (highWater_ < reserved_) b = int32_t(highWater_++);
        else { full_ = full_ || !undo; latch_ = latch_ || !undo; return -1; }
        std::memset(data(b), 0, kBlock * sizeof(float));
        meta_[b] = Meta{-1, 1, k};
        ++used_;
        undoUsed_ += undo;
        return b;
    }

    void addRef(int32_t b) { ++meta_[b].refs; }
    bool shared(int32_t b) const { return meta_[b].refs > 1; }
    void free(int32_t b) {
        if (--meta_[b].refs) return;
        freeList_[freeCount_++] = b;
        --used_;
        undoUsed_ -= meta_[b].kind == Kind::Undo;
        full_ = false;
        if (latch_ && freeCount_ + (reserved_ - highWater_) >= margin_) latch_ = false;
    }
    // The engine reports a write it had to refuse (a delta that could not grow) when nothing could be evicted.
    void markFull() { latch_ = true; }
    // Blocks that must be free before the latch lets go: about 1 s of audio at this rate.
    void setMarginBlocks(uint32_t n) { margin_ = n; }

    float* data(int32_t b) { return data_ + size_t(b) * kBlock; }
    const float* data(int32_t b) const { return data_ + size_t(b) * kBlock; }
    int32_t& next(int32_t b) { return meta_[b].next; }
    int32_t next(int32_t b) const { return meta_[b].next; }
    uint32_t used() const { return used_; }                // every block
    uint32_t usedBlocks() const { return used_; }          // the same (test_pool)
    uint32_t undoUsed() const { return undoUsed_; }
    uint32_t budgetBlocks() const { return budget_; }
    bool full() const { return full_; }                    // an Audio block was refused, and none came back since
    // Like full(), but held until margin free blocks are back (a lead-in or a dropped undo record is not enough).
    bool latched() const { return latch_; }

private:
    struct Meta { int32_t next; uint16_t refs; Kind kind; };
    void release() {
        std::free(data_); std::free(meta_); std::free(freeList_);
        data_ = nullptr; meta_ = nullptr; freeList_ = nullptr;
        reserved_ = budget_ = used_ = undoUsed_ = highWater_ = freeCount_ = 0;
        full_ = latch_ = false;
    }
    float*   data_ = nullptr;
    Meta*    meta_ = nullptr;
    int32_t* freeList_ = nullptr;
    uint32_t reserved_ = 0, budget_ = 0, used_ = 0, undoUsed_ = 0, highWater_ = 0, freeCount_ = 0;
    uint32_t margin_ = 1;
    bool full_ = false, latch_ = false;
};

} // namespace ouro
