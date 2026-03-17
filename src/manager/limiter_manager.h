#pragma once

#include <string>
#include <mutex>
#include <unordered_map>

#include "../limiter/token_bucket.h"

namespace manager {

struct ClientStats {
    int allowed = 0;
    int blocked = 0;
};

class limit_manager {
private:
    std::unordered_map<std::string, limit::TokenBucket> m_Bucket;
    std::unordered_map<std::string, ClientStats> m_stats;

    std::mutex m_mtx;

    int m_capacity;
    double m_refill_rate;

public:
    limit_manager(int cap,double rate);

    bool allow(const std::string& cli_id);

    std::string get_metrics();
};

}
