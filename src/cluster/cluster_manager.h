#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "cluster/hashing.h"
#include "node.grpc.pb.h"

namespace cluster {

class ClusterManager {
public:
    ClusterManager(std::string self_node_id,
                   std::string self_address,
                   std::vector<std::string> peer_nodes);

    bool should_handle_locally(const std::string& client_id) const;
    std::size_t node_index_for(const std::string& client_id) const;
    std::string node_address_for(const std::string& client_id) const;
    std::string node_id_for(const std::string& client_id) const;

    std::string configured_node_address_for(const std::string& client_id) const;
    std::string configured_node_id_for(const std::string& client_id) const;

    const std::string& self_node_id() const;
    const std::string& self_address() const;
    std::vector<std::string> peer_nodes() const;
    std::vector<std::string> active_peer_nodes() const;

    bool MarkNodeHealth(const std::string& peer_address, bool healthy);
    bool IsNodeHealthy(const std::string& peer_address) const;
    grpc::Status CheckNodeHealth(const std::string& peer_address,
                                 ratelimit::HealthCheckResponse* response);

    grpc::Status ForwardCheckLimit(const ratelimit::LimitRequest& request,
                                   ratelimit::LimitResponse* response);
    grpc::Status ForwardCheckLimitTo(const std::string& peer_address,
                                     const ratelimit::LimitRequest& request,
                                     ratelimit::LimitResponse* response);

private:
    struct NodeClient {
        std::shared_ptr<grpc::Channel> channel;
        std::unique_ptr<ratelimit::RateLimiterService::Stub> stub;
    };

    NodeClient* GetOrCreateClient(const std::string& peer_address);
    void RebuildActiveRingLocked();
    std::string NodeIdForAddress(const std::string& address) const;

    std::string self_node_id_;
    std::string self_address_;
    std::vector<std::string> peer_nodes_;
    std::unordered_map<std::string, std::string> address_to_node_id_;

    HashRing configured_ring_;
    HashRing active_ring_;
    std::vector<std::string> active_nodes_;
    std::unordered_map<std::string, bool> node_health_;

    mutable std::mutex state_mutex_;
    std::mutex clients_mutex_;
    std::unordered_map<std::string, NodeClient> clients_;
};

}  // namespace cluster
