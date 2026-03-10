#include <sstream>
#include "limiter_manager.h"

using namespace manager;

limit_manager::limit_manager(int cap,double rate)
    : m_capacity(cap), m_refill_rate(rate) {}

bool limit_manager::allow(const std::string& cli_id){
    std::lock_guard<std::mutex> lock(m_mtx);

    auto it = m_Bucket.find(cli_id);

    if(it == m_Bucket.end()){
        it = m_Bucket.emplace(
            cli_id,
            rate_limiter(m_capacity, m_refill_rate)
        ).first;
    }

    bool allowed = it->second.allow_request();

    if(allowed)
        m_stats[cli_id].allowed++;
    else
        m_stats[cli_id].blocked++;

    return allowed;
}

std::string limit_manager::get_metrics(){

    std::lock_guard<std::mutex> lock(m_mtx);

    std::stringstream ss;

    ss << "{\n";
    ss << "  \"active_clients\": " << m_Bucket.size() << ",\n";
    ss << "  \"clients\": [\n";

    bool first = true;

    for(auto &p : m_Bucket){

        if(!first) ss << ",\n";
        first = false;

        const std::string& client = p.first;
        rate_limiter& limiter = p.second;

        ss << "    {\n";
        ss << "      \"client_id\": \"" << client << "\",\n";
        ss << "      \"tokens\": " << limiter.getter_token() << ",\n";
        ss << "      \"allowed\": " << m_stats[client].allowed << ",\n";
        ss << "      \"blocked\": " << m_stats[client].blocked << "\n";
        ss << "    }";
    }

    ss << "\n  ]\n";
    ss << "}\n";

    return ss.str();
}