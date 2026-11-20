#pragma once
#include <cstdint>

namespace ouro {
constexpr int      kTracks = 4;
constexpr float    kFadeInMs = 3.0f, kPunchOutFadeMs = 5.0f, kRampMs = 15.0f;
constexpr float    kMinLoopMs = 100.0f, kMaxLoopSeconds = 300.0f, kHistoryMs = 100.0f;
constexpr float    kSilenceFloor = 5.0e-4f, kSoftClipKnee = 0.7079f, kMaxBudgetMb = 400.0f;
constexpr uint32_t kMaxPendingOps = 32;
inline uint32_t msToSamples(float ms, double sr) { return uint32_t(ms * sr / 1000.0 + 0.5); }
}
