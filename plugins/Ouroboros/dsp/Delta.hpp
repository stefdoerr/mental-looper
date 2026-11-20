#pragma once
#include "BlockPool.hpp"

namespace ouro {

struct DeltaCursor { int32_t block = -1; uint32_t index = 0; };   // index = block number within the delta
// begin()/release() do not invalidate cursors: reset each one reading a reused delta to DeltaCursor{}, never share one.

// Samples one overdub added, at loop positions start..start+length-1 (mod L),
// stored as a linked list of pool blocks (block k = offsets [k*kBlock, (k+1)*kBlock)).
class Delta {
public:
    void begin(uint32_t startPos) { start_ = startPos; length_ = 0; head_ = tail_ = -1; }

    bool append(BlockPool& p, float v) {
        const uint32_t i = length_ % BlockPool::kBlock;
        if (i == 0) {
            int32_t b = p.alloc();
            if (b < 0) return false;
            if (tail_ >= 0) p.next(tail_) = b; else head_ = b;
            tail_ = b;
        }
        p.data(tail_)[i] = v;
        ++length_;
        return true;
    }

    void accumulate(BlockPool& p, DeltaCursor& c, uint32_t offset, float v) {
        seek(p, c, offset);
        p.data(c.block)[offset % BlockPool::kBlock] += v;
    }

    float read(const BlockPool& p, DeltaCursor& c, uint32_t offset) const {
        seek(p, c, offset);
        return p.data(c.block)[offset % BlockPool::kBlock];
    }

    uint32_t start() const { return start_; }
    uint32_t length() const { return length_; }
    uint32_t blocks() const { return (length_ + BlockPool::kBlock - 1) / BlockPool::kBlock; }

    void release(BlockPool& p) {
        for (int32_t b = head_; b >= 0;) { int32_t n = p.next(b); p.free(b); b = n; }
        head_ = tail_ = -1; length_ = 0;
    }

private:
    // Position c on the block holding offset; forward walks are O(blocks skipped).
    void seek(const BlockPool& p, DeltaCursor& c, uint32_t offset) const {
        const uint32_t k = offset / BlockPool::kBlock;
        if (c.block < 0 || k < c.index) { c.block = head_; c.index = 0; }
        while (c.index < k) { c.block = p.next(c.block); ++c.index; }
    }
    uint32_t start_ = 0, length_ = 0;
    int32_t head_ = -1, tail_ = -1;
};

} // namespace ouro
