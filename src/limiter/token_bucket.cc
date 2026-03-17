#include "token_bucket.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace limit {

TokenBucket::TokenBucket(int capacity, double refill_rate)
    : capacity_(capacity > 0 ? capacity : 1),
      refill_rate_(refill_rate > 0.0 ? refill_rate : 0.0),
      tokens_(static_cast<double>(capacity_)),
      last_refill_timestamp_(current_time_ms()) {}

bool TokenBucket::allow_request(int tokens_requested) {
    if (tokens_requested <= 0) {
        return true;
    }

    refill();

    if (tokens_ < static_cast<double>(tokens_requested)) {
        return false;
    }

    tokens_ -= static_cast<double>(tokens_requested);
    return true;
}

void TokenBucket::refill() {
    refill_to(current_time_ms());
}

int TokenBucket::capacity() const {
    return capacity_;
}

double TokenBucket::refill_rate() const {
    return refill_rate_;
}

double TokenBucket::tokens() const {
    return tokens_;
}

std::int64_t TokenBucket::last_refill_timestamp() const {
    return last_refill_timestamp_;
}

std::int64_t TokenBucket::retry_after_ms(int tokens_requested) const {
    if (tokens_requested <= 0 || tokens_ >= static_cast<double>(tokens_requested)) {
        return 0;
    }

    if (refill_rate_ <= 0.0) {
        return -1;
    }

    const double missing_tokens = static_cast<double>(tokens_requested) - tokens_;
    const double wait_ms = (missing_tokens / refill_rate_) * 1000.0;
    return static_cast<std::int64_t>(std::ceil(wait_ms));
}

std::int64_t TokenBucket::current_time_ms() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

void TokenBucket::refill_to(std::int64_t now_ms) {
    if (now_ms <= last_refill_timestamp_) {
        return;
    }

    const double elapsed_seconds =
        static_cast<double>(now_ms - last_refill_timestamp_) / 1000.0;
    tokens_ = std::min(
        static_cast<double>(capacity_),
        tokens_ + (elapsed_seconds * refill_rate_));
    last_refill_timestamp_ = now_ms;
}

}  // namespace limit
