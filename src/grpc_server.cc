#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "cluster/cluster_manager.h"
#include "node.grpc.pb.h"
#include "storage/token_bucket_store.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

using ratelimit::HealthCheckRequest;
using ratelimit::HealthCheckResponse;
using ratelimit::LimitRequest;
using ratelimit::LimitResponse;
using ratelimit::RateLimiterService;
using ratelimit::SyncState;

namespace {

struct ServerConfig {
    int port = 50051;
    std::string node_id = "node1";
    int capacity = 10;
    double refill_rate = 5.0;
    std::vector<std::string> peer_nodes = {
        "localhost:50051",
        "localhost:50052",
        "localhost:50053",
    };
};

int ParseIntArg(const std::string& value, const char* name) {
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid value for ") + name);
    }
}

double ParseDoubleArg(const std::string& value, const char* name) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid value for ") + name);
    }
}

ServerConfig ParseArgs(int argc, char** argv) {
    ServerConfig config;
    std::vector<std::string> positional_args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto read_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--port") {
            config.port = ParseIntArg(read_value("--port"), "--port");
        } else if (arg == "--node_id") {
            config.node_id = read_value("--node_id");
        } else if (arg == "--capacity") {
            config.capacity = ParseIntArg(read_value("--capacity"), "--capacity");
        } else if (arg == "--refill_rate") {
            config.refill_rate = ParseDoubleArg(read_value("--refill_rate"), "--refill_rate");
        } else if (!arg.empty() && arg[0] != '-') {
            positional_args.push_back(arg);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (!positional_args.empty()) {
        if (positional_args.size() != 2) {
            throw std::invalid_argument(
                "positional usage requires: grpc_server <node_id> <port>");
        }

        config.node_id = positional_args[0];
        config.port = ParseIntArg(positional_args[1], "<port>");
    }

    if (config.capacity <= 0) {
        throw std::invalid_argument("--capacity must be > 0");
    }

    if (config.refill_rate < 0.0) {
        throw std::invalid_argument("--refill_rate must be >= 0");
    }

    return config;
}

std::string AddressForPort(int port) {
    return "localhost:" + std::to_string(port);
}

std::int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void LogDecision(const std::string& node_id,
                 const std::string& request_id,
                 const std::string& client_id,
                 const std::string& target_node,
                 std::int64_t tokens_remaining,
                 std::int64_t latency_ms,
                 bool allowed,
                 bool used_redis,
                 bool redis_failure) {
    std::cout << "{"
              << "\"event\":\"rate_limit_decision\","
              << "\"node_id\":\"" << node_id << "\","
              << "\"request_id\":\"" << request_id << "\","
              << "\"client_id\":\"" << client_id << "\","
              << "\"target_node\":\"" << target_node << "\","
              << "\"tokens_remaining\":" << tokens_remaining << ","
              << "\"latency_ms\":" << latency_ms << ","
              << "\"allowed\":" << (allowed ? "true" : "false") << ","
              << "\"used_redis\":" << (used_redis ? "true" : "false") << ","
              << "\"redis_failure\":" << (redis_failure ? "true" : "false")
              << "}"
              << std::endl;
}

class RateLimiterServiceImpl final : public RateLimiterService::Service {
public:
    RateLimiterServiceImpl(int bucket_capacity,
                           double refill_rate,
                           std::string node_id,
                           std::string self_address,
                           std::vector<std::string> peer_nodes)
        : cluster_manager_(
              std::move(node_id),
              std::move(self_address),
              std::move(peer_nodes)),
          store_(bucket_capacity, refill_rate) {}

    Status CheckLimit(ServerContext* context,
                      const LimitRequest* request,
                      LimitResponse* response) override {
        (void)context;
        const auto start = CurrentTimeMs();

        const std::string& client_id = request->client_id();
        if (client_id.empty()) {
            return Status(StatusCode::INVALID_ARGUMENT, "client_id is required");
        }

        const int tokens_requested = request->tokens_requested() > 0
            ? static_cast<int>(request->tokens_requested())
            : 1;
        const std::string request_id =
            request->request_id().empty() ? "unknown" : request->request_id();
        const std::string owner_node_id = cluster_manager_.configured_node_id_for(client_id);
        const std::string owner_address = cluster_manager_.configured_node_address_for(client_id);

        if (!cluster_manager_.should_handle_locally(client_id)) {
            const Status forward_status =
                cluster_manager_.ForwardCheckLimit(*request, response);
            if (forward_status.ok()) {
                return Status::OK;
            }

            std::cerr << cluster_manager_.self_node_id()
                      << " fallback to local processing"
                      << " request_id=" << request_id
                      << " client=" << client_id
                      << " owner_node=" << owner_node_id
                      << " owner_address=" << owner_address
                      << " code=" << forward_status.error_code()
                      << " message=\"" << forward_status.error_message() << "\""
                      << std::endl;
        }

        const storage::BucketDecision decision = store_.Consume(client_id, tokens_requested);
        response->set_allowed(decision.allowed);
        response->set_remaining_tokens(decision.remaining_tokens);
        response->set_retry_after_ms(decision.retry_after_ms);
        response->set_node_id(cluster_manager_.self_node_id());
        response->set_used_redis(decision.used_redis);
        response->set_redis_failure(decision.redis_failure);
        response->set_target_node(owner_node_id);
        response->set_request_id(request_id);

        const auto latency_ms = CurrentTimeMs() - start;
        LogDecision(
            cluster_manager_.self_node_id(),
            request_id,
            client_id,
            owner_node_id,
            decision.remaining_tokens,
            latency_ms,
            decision.allowed,
            decision.used_redis,
            decision.redis_failure);
        return Status::OK;
    }

    Status SyncTokenState(ServerContext* context,
                          const SyncState* request,
                          LimitResponse* response) override {
        (void)context;
        response->set_allowed(true);
        response->set_remaining_tokens(request->tokens());
        response->set_retry_after_ms(0);
        response->set_node_id(cluster_manager_.self_node_id());
        response->set_used_redis(false);
        response->set_redis_failure(false);
        response->set_target_node(cluster_manager_.self_node_id());
        response->set_request_id("");
        return Status::OK;
    }

    Status HealthCheck(ServerContext* context,
                       const HealthCheckRequest* request,
                       HealthCheckResponse* response) override {
        (void)context;
        (void)request;
        response->set_healthy(true);
        response->set_node_id(cluster_manager_.self_node_id());
        return Status::OK;
    }

private:
    cluster::ClusterManager cluster_manager_;
    storage::TokenBucketStore store_;
};

void RunServer(const ServerConfig& config) {
    const std::string server_address = "0.0.0.0:" + std::to_string(config.port);
    const std::string self_address = AddressForPort(config.port);

    RateLimiterServiceImpl service(
        config.capacity,
        config.refill_rate,
        config.node_id,
        self_address,
        config.peer_nodes);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (!server) {
        throw std::runtime_error("failed to start gRPC server on " + server_address);
    }

    std::cout << "gRPC server started"
              << " node_id=" << config.node_id
              << " port=" << config.port
              << " capacity=" << config.capacity
              << " refill_rate=" << config.refill_rate
              << std::endl;

    server->Wait();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const ServerConfig config = ParseArgs(argc, argv);
        RunServer(config);
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "grpc_server error: " << ex.what() << std::endl;
        std::cerr
            << "Usage: grpc_server --port <port> --node_id <node_id> "
            << "--capacity <capacity> --refill_rate <refill_rate>"
            << std::endl;
        return EXIT_FAILURE;
    }
}
