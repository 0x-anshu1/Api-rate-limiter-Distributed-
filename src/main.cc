#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "crow.h"
#include <grpcpp/grpcpp.h>

#include "cluster/cluster_manager.h"
#include "node.grpc.pb.h"

namespace {

struct GatewayStats {
    std::atomic<std::uint64_t> total_requests{0};
    std::atomic<std::uint64_t> forwarded_to_owner{0};
    std::atomic<std::uint64_t> fallback_requests{0};
    std::atomic<std::uint64_t> failed_requests{0};
    std::atomic<std::uint64_t> allowed_requests{0};
    std::atomic<std::uint64_t> rejected_requests{0};
    std::atomic<std::uint64_t> redis_requests{0};
    std::atomic<std::uint64_t> redis_failures{0};
    std::atomic<std::uint64_t> node_failures{0};
    std::atomic<std::uint64_t> hash_reroutes{0};
};

std::int64_t CurrentTimeSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string MakeRequestId() {
    static std::atomic<std::uint64_t> request_seq{0};
    return "req-" + std::to_string(CurrentTimeMs()) + "-" +
           std::to_string(request_seq.fetch_add(1, std::memory_order_relaxed));
}

class HttpGateway {
public:
    HttpGateway()
        : cluster_manager_(
              "gateway",
              "localhost:8000",
              {"localhost:50051", "localhost:50052", "localhost:50053"}) {
        health_thread_ = std::thread([this]() {
            MonitorNodes();
        });
    }

    ~HttpGateway() {
        stop_health_checks_.store(true, std::memory_order_relaxed);
        if (health_thread_.joinable()) {
            health_thread_.join();
        }
    }

    crow::response HandleRequest(const crow::request& req) {
        const auto start_ms = CurrentTimeMs();
        const char* client = req.url_params.get("client_id");
        if (client == nullptr) {
            return crow::response(400, "Missing client_id");
        }

        const std::string client_id(client);
        const std::string request_id = MakeRequestId();
        const std::string configured_owner = cluster_manager_.configured_node_id_for(client_id);
        const std::string owner_node_id = cluster_manager_.node_id_for(client_id);
        const std::string owner_address = cluster_manager_.node_address_for(client_id);

        ratelimit::LimitRequest grpc_request;
        grpc_request.set_client_id(client_id);
        grpc_request.set_tokens_requested(1);
        grpc_request.set_timestamp(CurrentTimeSeconds());
        grpc_request.set_request_id(request_id);

        ratelimit::LimitResponse grpc_response;
        stats_.total_requests.fetch_add(1, std::memory_order_relaxed);

        grpc::Status status =
            cluster_manager_.ForwardCheckLimitTo(owner_address, grpc_request, &grpc_response);

        if (status.ok()) {
            stats_.forwarded_to_owner.fetch_add(1, std::memory_order_relaxed);
            UpdateResponseMetrics(grpc_response, configured_owner);
            LogRequest(
                request_id,
                client_id,
                owner_node_id,
                grpc_response.remaining_tokens(),
                CurrentTimeMs() - start_ms,
                grpc_response.allowed());
            return BuildResponse(grpc_response);
        }

        std::cerr << "gateway primary owner unavailable"
                  << " request_id=" << request_id
                  << " client=" << client_id
                  << " owner_node=" << owner_node_id
                  << " owner_address=" << owner_address
                  << " code=" << status.error_code()
                  << " message=\"" << status.error_message() << "\""
                  << std::endl;

        for (const std::string& peer_address : cluster_manager_.active_peer_nodes()) {
            if (peer_address == owner_address) {
                continue;
            }

            ratelimit::LimitResponse fallback_response;
            grpc::Status fallback_status = cluster_manager_.ForwardCheckLimitTo(
                peer_address,
                grpc_request,
                &fallback_response);

            if (!fallback_status.ok()) {
                continue;
            }

            stats_.fallback_requests.fetch_add(1, std::memory_order_relaxed);
            UpdateResponseMetrics(fallback_response, configured_owner);
            LogRequest(
                request_id,
                client_id,
                fallback_response.node_id(),
                fallback_response.remaining_tokens(),
                CurrentTimeMs() - start_ms,
                fallback_response.allowed());
            return BuildResponse(fallback_response);
        }

        stats_.failed_requests.fetch_add(1, std::memory_order_relaxed);
        return crow::response(503, "All rate limiter nodes are unavailable");
    }

    crow::response Metrics() const {
        std::ostringstream out;
        out << "{\n";
        out << "  \"gateway\": \"localhost:8000\",\n";
        out << "  \"peer_nodes\": [";

        const std::vector<std::string> peers = cluster_manager_.peer_nodes();
        for (std::size_t i = 0; i < peers.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << "\"" << peers[i] << "\"";
        }
        out << "],\n";

        const std::vector<std::string> active = cluster_manager_.active_peer_nodes();
        out << "  \"active_nodes\": [";
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << "\"" << active[i] << "\"";
        }
        out << "],\n";
        out << "  \"total_requests\": " << stats_.total_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"forwarded_to_owner\": " << stats_.forwarded_to_owner.load(std::memory_order_relaxed) << ",\n";
        out << "  \"fallback_requests\": " << stats_.fallback_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"failed_requests\": " << stats_.failed_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"allowed_requests\": " << stats_.allowed_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"rejected_requests\": " << stats_.rejected_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"redis_requests\": " << stats_.redis_requests.load(std::memory_order_relaxed) << ",\n";
        out << "  \"redis_failures\": " << stats_.redis_failures.load(std::memory_order_relaxed) << ",\n";
        out << "  \"node_failures\": " << stats_.node_failures.load(std::memory_order_relaxed) << ",\n";
        out << "  \"hash_reroutes\": " << stats_.hash_reroutes.load(std::memory_order_relaxed) << "\n";
        out << "}\n";
        crow::response response(out.str());
        response.set_header("Content-Type", "application/json");
        return response;
    }

private:
    static crow::response BuildResponse(const ratelimit::LimitResponse& response) {
        std::ostringstream body;
        body << "{\n";
        body << "  \"allowed\": " << (response.allowed() ? "true" : "false") << ",\n";
        body << "  \"remaining_tokens\": " << response.remaining_tokens() << ",\n";
        body << "  \"retry_after_ms\": " << response.retry_after_ms() << ",\n";
        body << "  \"node_id\": \"" << response.node_id() << "\"\n";
        body << "}\n";

        const int status_code = response.allowed() ? 200 : 429;
        crow::response http_response(status_code, body.str());
        http_response.set_header("Content-Type", "application/json");
        return http_response;
    }

    void UpdateResponseMetrics(const ratelimit::LimitResponse& response,
                               const std::string& configured_owner) {
        if (response.allowed()) {
            stats_.allowed_requests.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.rejected_requests.fetch_add(1, std::memory_order_relaxed);
        }

        if (response.used_redis()) {
            stats_.redis_requests.fetch_add(1, std::memory_order_relaxed);
        }

        if (response.redis_failure()) {
            stats_.redis_failures.fetch_add(1, std::memory_order_relaxed);
        }

        if (!response.node_id().empty() && response.node_id() != configured_owner) {
            stats_.hash_reroutes.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void LogRequest(const std::string& request_id,
                    const std::string& client_id,
                    const std::string& target_node,
                    std::int64_t tokens_remaining,
                    std::int64_t latency_ms,
                    bool allowed) const {
        std::cout << "{"
                  << "\"event\":\"gateway_request\","
                  << "\"request_id\":\"" << request_id << "\","
                  << "\"client_id\":\"" << client_id << "\","
                  << "\"target_node\":\"" << target_node << "\","
                  << "\"tokens_remaining\":" << tokens_remaining << ","
                  << "\"latency_ms\":" << latency_ms << ","
                  << "\"allowed\":" << (allowed ? "true" : "false")
                  << "}"
                  << std::endl;
    }

    void MonitorNodes() {
        while (!stop_health_checks_.load(std::memory_order_relaxed)) {
            for (const std::string& peer_address : cluster_manager_.peer_nodes()) {
                ratelimit::HealthCheckResponse response;
                const grpc::Status status = cluster_manager_.CheckNodeHealth(peer_address, &response);
                const bool healthy = status.ok() && response.healthy();
                const bool changed = cluster_manager_.MarkNodeHealth(peer_address, healthy);
                if (!changed) {
                    continue;
                }

                if (healthy) {
                    std::cout << "Node rejoined cluster"
                              << " address=" << peer_address
                              << " node_id=" << response.node_id()
                              << std::endl;
                } else {
                    stats_.node_failures.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "Node removed from cluster"
                              << " address=" << peer_address
                              << std::endl;
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    cluster::ClusterManager cluster_manager_;
    GatewayStats stats_;
    std::thread health_thread_;
    std::atomic<bool> stop_health_checks_{false};
};

}  // namespace

int main() {
    crow::SimpleApp app;
    HttpGateway gateway;

    CROW_ROUTE(app, "/request")
    ([&gateway](const crow::request& req) {
        return gateway.HandleRequest(req);
    });

    CROW_ROUTE(app, "/metrics")
    ([&gateway]() {
        return gateway.Metrics();
    });

    app.port(8000).multithreaded().run();
    return 0;
}
