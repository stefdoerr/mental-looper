#pragma once
#include <cstdint>

namespace ouro {
constexpr int      kTracks = 4;
constexpr float    kFadeInMs = 3.0f, kPunchOutFadeMs = 5.0f, kRampMs = 15.0f;
constexpr float    kMinLoopMs = 100.0f, kMaxLoopSeconds = 300.0f, kHistoryMs = 250.0f;
constexpr float    kPreRollMs = 10.0f;      // an auto-record start keeps this much before the crossing
constexpr float    kSyncGraceMs = 150.0f;   // Sync: a press this late after a bar line counts as on it
constexpr float    kSilenceFloor = 5.0e-4f, kSoftClipKnee = 0.7079f, kMaxBudgetMb = 400.0f;
constexpr uint32_t kMaxPendingOps = 32;
constexpr float    kRoundDownFraction = 0.25f;   // a recording onto the loop rounds down within this much of a loop
inline uint32_t msToSamples(float ms, double sr) { return uint32_t(ms * sr / 1000.0 + 0.5); }
}
