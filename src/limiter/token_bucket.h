#pragma once

#include <cstdint>

namespace limit {

class TokenBucket {
public:
    TokenBucket(int capacity, double refill_rate);

    bool allow_request(int tokens_requested = 1);
    void refill();
    static std::int64_t current_time_ms();

    int capacity() const;
    double refill_rate() const;
    double tokens() const;
    std::int64_t last_refill_timestamp() const;
    std::int64_t retry_after_ms(int tokens_requested = 1) const;

private:
    void refill_to(std::int64_t now_ms);

    int capacity_;
    double refill_rate_;
    double tokens_;
    std::int64_t last_refill_timestamp_;
};

}  // namespace limit
