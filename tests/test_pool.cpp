#include "test_util.hpp"
#include "dsp/BlockPool.hpp"
#include "dsp/Delta.hpp"
#include <cstdint>

static void budgetLimitsAllocs() {
    ouro::BlockPool p; CHECK(p.reserve(1 << 20));
    p.setBudgetBytes(16 * ouro::BlockPool::kBlock * 4);
    for (int i = 0; i < 16; ++i) CHECK(p.alloc() >= 0);
    CHECK(p.alloc() == -1);  p.free(3);  CHECK(p.alloc() >= 0);  CHECK(p.usedBlocks() == 16);
}
static void reusedBlocksAreZeroed() {
    ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(1 << 20);
    int32_t b = p.alloc(); p.data(b)[100] = 1.0f; p.free(b);
    int32_t c = p.alloc(); CHECK(c == b && p.data(c)[100] == 0.0f);
}
static void deltaAppendReadAccumulate() {
    ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(1 << 20);
    ouro::Delta d; d.begin(777);
    for (uint32_t i = 0; i < 10000; ++i) CHECK(d.append(p, float(i)));
    CHECK(d.length() == 10000 && d.blocks() == 3 && d.start() == 777);
    ouro::DeltaCursor c;
    CHECK(d.read(p, c, 9999) == 9999.0f && d.read(p, c, 0) == 0.0f && d.read(p, c, 5000) == 5000.0f);
    d.accumulate(p, c, 5000, 0.5f);  CHECK(d.read(p, c, 5000) == 5000.5f);
    d.release(p);  CHECK(p.usedBlocks() == 0 && d.length() == 0);
}
static void appendStopsWhenPoolFull() {
    ouro::BlockPool p; p.reserve(1 << 20); p.setBudgetBytes(2 * ouro::BlockPool::kBlock * 4);
    ouro::Delta d; d.begin(0); uint32_t n = 0;
    while (d.append(p, 1.0f)) ++n;
    CHECK(n == 2 * ouro::BlockPool::kBlock && d.length() == n);
}
static void reserveFailureIsSafe() {
    ouro::BlockPool p; CHECK(!p.reserve(SIZE_MAX / 2)); CHECK(p.alloc() == -1);
}

int main() {
    budgetLimitsAllocs(); reusedBlocksAreZeroed(); deltaAppendReadAccumulate();
    appendStopsWhenPoolFull(); reserveFailureIsSafe();
    REPORT();
}
