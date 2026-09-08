#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hft {

struct LatencyHistogram {
    void record(uint64_t ns) {
        samples_.push_back(ns);
        sum_ += ns;
        if (ns < min_) min_ = ns;
        if (ns > max_) max_ = ns;
    }
    uint64_t count() const { return samples_.size(); }
    uint64_t min() const { return samples_.empty() ? 0 : min_; }
    uint64_t max() const { return samples_.empty() ? 0 : max_; }
    double mean() const {
        return samples_.empty() ? 0.0 : double(sum_) / samples_.size();
    }
    uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        auto s = samples_;
        std::sort(s.begin(), s.end());
        return s[size_t(p * (s.size() - 1))];
    }
private:
    std::vector<uint64_t> samples_;
    uint64_t sum_{0}, min_{UINT64_MAX}, max_{0};
};

} // namespace hft
