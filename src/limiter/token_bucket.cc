#include "token_bucket.h"

using namespace limit;
using namespace std;

bool rate_limiter::allow_request(){
    refill();
    if(m_token>=1){
        m_token-=1;
        return true;
    }
    return false;
}

void rate_limiter::refill(){
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_last_refill).count();
    m_token = std::min((double)m_capacity, m_token + elapsed * m_refill_rate);
    m_last_refill = now;

    std::cout<<getter_token()<<std::endl;

}

double rate_limiter::getter_token(){
    return m_token;
}