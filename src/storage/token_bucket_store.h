#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "limiter/token_bucket.h"

namespace storage {

struct BucketDecision {
    bool allowed = false;
    std::int64_t remaining_tokens = 0;
    std::int64_t retry_after_ms = 0;
    bool used_redis = false;
    bool redis_failure = false;
};

class TokenBucketStore {
public:
    TokenBucketStore(int capacity, double refill_rate);
    ~TokenBucketStore();

    BucketDecision Consume(const std::string& client_id, int tokens_requested);

private:
    BucketDecision ConsumeInMemory(const std::string& client_id, int tokens_requested);

#ifdef HAVE_REDIS
    struct RedisContextDeleter {
        void operator()(void* context) const;
    };

    BucketDecision ConsumeWithRedis(const std::string& client_id, int tokens_requested);
    bool EnsureRedisConnection();
#endif

    const int capacity_;
    const double refill_rate_;

    std::mutex fallback_mutex_;
    std::unordered_map<std::string, limit::TokenBucket> fallback_buckets_;

#ifdef HAVE_REDIS
    std::mutex redis_mutex_;
    std::string redis_host_;
    int redis_port_;
    std::unique_ptr<void, RedisContextDeleter> redis_context_;
#endif
};

}  // namespace storage
