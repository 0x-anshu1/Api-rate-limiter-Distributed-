#pragma once

#include<iostream>
#include<vector>
#include<chrono>
#include<algorithm>

namespace limit{
    class rate_limiter{
        private:
                int m_capacity;
                double m_token,m_refill_rate;
                std::chrono::steady_clock::time_point m_last_refill;
        public:
                rate_limiter(int cap,double rate):m_capacity(cap),m_token(cap),m_refill_rate(rate),m_last_refill(std::chrono::steady_clock::now()){}
                bool allow_request();
                void refill();
                double getter_token();
    };
}