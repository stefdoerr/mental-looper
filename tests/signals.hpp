#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

inline std::vector<float> sine(double hz, float amp, uint32_t n, double sr = 48000.0) {
    std::vector<float> x(n);
    for (uint32_t i = 0; i < n; ++i)
        x[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * i / sr));
    return x;
}
inline std::vector<float> slice(const std::vector<float>& x, size_t from, size_t to) {
    return std::vector<float>(x.begin() + from, x.begin() + to);
}
// Appends img[0] so maxStep also sees the L-1 -> 0 wrap.
inline std::vector<float> wrap(std::vector<float> img) {
    if (!img.empty()) img.push_back(img[0]);
    return img;
}
inline float maxStep(const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}
inline float peak(const std::vector<float>& x) {
    float m = 0.0f;
    for (float v : x) m = std::max(m, std::fabs(v));
    return m;
}
inline float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}
inline float sineStep(double hz, float amp, double sr = 48000.0) {
    return static_cast<float>(2.0 * M_PI * hz / sr * amp);
}
