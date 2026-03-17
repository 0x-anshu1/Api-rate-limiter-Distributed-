#include "storage/token_bucket_store.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef HAVE_REDIS
#include <hiredis/hiredis.h>
#endif

namespace storage {

namespace {

constexpr const char* kRedisConsumeScript = R"lua(
local capacity = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])
local tokens_requested = tonumber(ARGV[3])
local now_ms = tonumber(ARGV[4])

local current_tokens = tonumber(redis.call('HGET', KEYS[1], 'tokens'))
local last_refill = tonumber(redis.call('HGET', KEYS[1], 'last_refill_timestamp'))

if current_tokens == nil then
  current_tokens = capacity
  last_refill = now_ms
end

if now_ms > last_refill then
  local elapsed_seconds = (now_ms - last_refill) / 1000.0
  current_tokens = math.min(capacity, current_tokens + (elapsed_seconds * refill_rate))
  last_refill = now_ms
end

local allowed = 0
local retry_after_ms = 0

if tokens_requested <= 0 then
  allowed = 1
elseif current_tokens >= tokens_requested then
  current_tokens = current_tokens - tokens_requested
  allowed = 1
elseif refill_rate <= 0 then
  retry_after_ms = -1
else
  retry_after_ms = math.ceil(((tokens_requested - current_tokens) / refill_rate) * 1000.0)
end

redis.call('HSET', KEYS[1], 'tokens', current_tokens, 'last_refill_timestamp', last_refill)

return {allowed, math.floor(current_tokens), retry_after_ms}
)lua";

std::int64_t CurrentTimeMs() {
    return limit::TokenBucket::current_time_ms();
}

}  // namespace

TokenBucketStore::TokenBucketStore(int capacity, double refill_rate)
    : capacity_(capacity),
      refill_rate_(refill_rate)
#ifdef HAVE_REDIS
      ,
      redis_host_(std::getenv("REDIS_HOST") != nullptr ? std::getenv("REDIS_HOST")
                                                       : "127.0.0.1"),
      redis_port_(std::getenv("REDIS_PORT") != nullptr ? std::atoi(std::getenv("REDIS_PORT"))
                                                       : 6379),
      redis_context_(nullptr)
#endif
{
}

TokenBucketStore::~TokenBucketStore() = default;

BucketDecision TokenBucketStore::Consume(const std::string& client_id, int tokens_requested) {
#ifdef HAVE_REDIS
    BucketDecision redis_decision = ConsumeWithRedis(client_id, tokens_requested);
    if (redis_decision.used_redis && !redis_decision.redis_failure) {
        return redis_decision;
    }

    BucketDecision fallback_decision = ConsumeInMemory(client_id, tokens_requested);
    fallback_decision.redis_failure = redis_decision.redis_failure;
    return fallback_decision;
#else
    return ConsumeInMemory(client_id, tokens_requested);
#endif
}

BucketDecision TokenBucketStore::ConsumeInMemory(
    const std::string& client_id,
    int tokens_requested) {
    std::lock_guard<std::mutex> lock(fallback_mutex_);

    auto [it, inserted] = fallback_buckets_.try_emplace(
        client_id,
        capacity_,
        refill_rate_);
    (void)inserted;

    limit::TokenBucket& bucket = it->second;
    const bool allowed = bucket.allow_request(tokens_requested);

    BucketDecision decision;
    decision.allowed = allowed;
    decision.remaining_tokens = static_cast<std::int64_t>(bucket.tokens());
    decision.retry_after_ms = allowed ? 0 : bucket.retry_after_ms(tokens_requested);
    decision.used_redis = false;
    decision.redis_failure = false;
    return decision;
}

#ifdef HAVE_REDIS
void TokenBucketStore::RedisContextDeleter::operator()(void* context) const {
    if (context != nullptr) {
        redisFree(static_cast<redisContext*>(context));
    }
}

bool TokenBucketStore::EnsureRedisConnection() {
    if (redis_context_ != nullptr && static_cast<redisContext*>(redis_context_.get())->err == 0) {
        return true;
    }

    redisContext* context = redisConnect(redis_host_.c_str(), redis_port_);
    if (context == nullptr) {
        redis_context_.reset(nullptr);
        return false;
    }

    if (context->err != 0) {
        redisFree(context);
        redis_context_.reset(nullptr);
        return false;
    }

    redis_context_.reset(context);
    return true;
}

BucketDecision TokenBucketStore::ConsumeWithRedis(
    const std::string& client_id,
    int tokens_requested) {
    BucketDecision decision;
    decision.used_redis = true;

    std::lock_guard<std::mutex> lock(redis_mutex_);
    if (!EnsureRedisConnection()) {
        decision.redis_failure = true;
        return decision;
    }

    redisReply* reply = static_cast<redisReply*>(redisCommand(
        static_cast<redisContext*>(redis_context_.get()),
        "EVAL %s 1 %s %d %f %d %lld",
        kRedisConsumeScript,
        ("rate_limit:" + client_id).c_str(),
        capacity_,
        refill_rate_,
        tokens_requested,
        static_cast<long long>(CurrentTimeMs())));

    if (reply == nullptr) {
        redis_context_.reset(nullptr);
        decision.redis_failure = true;
        return decision;
    }

    std::unique_ptr<redisReply, decltype(&freeReplyObject)> owned_reply(reply, &freeReplyObject);
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 3) {
        decision.redis_failure = true;
        return decision;
    }

    decision.allowed = reply->element[0]->integer != 0;
    decision.remaining_tokens = reply->element[1]->integer;
    decision.retry_after_ms = reply->element[2]->integer;
    return decision;
}
#endif

}  // namespace storage
