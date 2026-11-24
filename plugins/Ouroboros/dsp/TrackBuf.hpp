#pragma once
#include "BlockPool.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ouro {

// A track's audio, cap samples long, in pool blocks (kind Audio) taken on the first write: unwritten samples read 0.
// A copy (shareFrom) shares the blocks; a write copies a shared block first. init() is the only allocating call
// (non-RT); everything else is RT-safe. The pool owns the blocks: clear() hands them back before the pool goes.
class TrackBuf {
public:
    TrackBuf() = default;
    TrackBuf(const TrackBuf&) = delete;
    TrackBuf& operator=(const TrackBuf&) = delete;
    ~TrackBuf() { std::free(tab_); }

    bool init(uint32_t cap) {                   // a block table for cap samples, no audio
        std::free(tab_);
        cap_ = cap;
        n_ = cap / kB + (cap % kB != 0);
        tab_ = static_cast<int32_t*>(std::malloc(size_t(n_) * sizeof(int32_t)));
        if (!tab_) n_ = 0;
        std::fill(tab_, tab_ + n_, -1);
        return tab_ != nullptr;
    }

    float get(const BlockPool& p, uint32_t i) const {
        const int32_t b = tab_[i / kB];
        return b < 0 ? 0.0f : p.data(b)[i % kB];
    }

    // Sample i, writable: its block is taken (zeroed) or unshared first. nullptr if the pool refuses.
    float* at(BlockPool& p, uint32_t i) {
        int32_t& b = tab_[i / kB];
        if (b < 0 || p.shared(b)) {
            const int32_t c = p.alloc(BlockPool::Kind::Audio);
            if (c < 0) return nullptr;
            if (b >= 0) { std::memcpy(p.data(c), p.data(b), kB * sizeof(float)); p.free(b); }
            b = c;
        }
        return p.data(b) + i % kB;
    }

    // [from, to) reads 0: whole blocks go back to the pool (the last one ends at cap), parts of one are written.
    // ponytail: a shared block's part needs a copy to write; a full pool leaves it unzeroed (parts only, unreached
    // before copies exist); drop its block instead if that is ever hit
    void zero(BlockPool& p, uint32_t from, uint32_t to) {
        while (from < to) {
            const uint32_t n = std::min(to - from, kB - from % kB);
            int32_t& b = tab_[from / kB];
            if (b >= 0 && from % kB == 0 && (n == kB || from + n == cap_)) { p.free(b); b = -1; }
            else if (b >= 0) if (float* s = at(p, from)) std::fill(s, s + n, 0.0f);
            from += n;
        }
    }

    void clear(BlockPool& p) {
        for (uint32_t k = 0; k < n_; ++k) if (tab_[k] >= 0) { p.free(tab_[k]); tab_[k] = -1; }
    }

    // This buffer becomes a copy of src (same cap) by reference: no allocation.
    void shareFrom(BlockPool& p, const TrackBuf& src) {
        clear(p);
        for (uint32_t k = 0; k < n_; ++k) if ((tab_[k] = src.tab_[k]) >= 0) p.addRef(tab_[k]);
    }

private:
    static constexpr uint32_t kB = BlockPool::kBlock;
    int32_t* tab_ = nullptr;                    // block per kB samples, -1 = none (reads 0)
    uint32_t n_ = 0, cap_ = 0;
};

} // namespace ouro
