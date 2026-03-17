#include "cluster/cluster_manager.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cluster {

namespace {

constexpr int kMaxForwardAttempts = 3;
constexpr auto kForwardTimeout = std::chrono::milliseconds(500);
constexpr auto kHealthTimeout = std::chrono::milliseconds(250);
constexpr auto kRetryBackoff = std::chrono::milliseconds(100);

bool IsRetryableStatus(const grpc::Status& status) {
    return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
           status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
}

}  // namespace

ClusterManager::ClusterManager(std::string self_node_id,
                               std::string self_address,
                               std::vector<std::string> peer_nodes)
    : self_node_id_(std::move(self_node_id)),
      self_address_(std::move(self_address)),
      peer_nodes_(std::move(peer_nodes)),
      configured_ring_(100),
      active_ring_(100) {
    if (peer_nodes_.empty()) {
        throw std::invalid_argument("peer_nodes must not be empty");
    }

    for (std::size_t i = 0; i < peer_nodes_.size(); ++i) {
        const std::string& address = peer_nodes_[i];
        address_to_node_id_.emplace(address, "node" + std::to_string(i + 1));
        configured_ring_.add_node(address);
        active_ring_.add_node(address);
        active_nodes_.push_back(address);
        node_health_[address] = true;
    }
}

bool ClusterManager::should_handle_locally(const std::string& client_id) const {
    return node_address_for(client_id) == self_address_;
}

std::size_t ClusterManager::node_index_for(const std::string& client_id) const {
    return std::hash<std::string>{}(client_id) % peer_nodes_.size();
}

std::string ClusterManager::node_address_for(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (active_nodes_.empty()) {
        return configured_ring_.get_node(client_id);
    }

    return active_ring_.get_node(client_id);
}

std::string ClusterManager::node_id_for(const std::string& client_id) const {
    return NodeIdForAddress(node_address_for(client_id));
}

std::string ClusterManager::configured_node_address_for(const std::string& client_id) const {
    return configured_ring_.get_node(client_id);
}

std::string ClusterManager::configured_node_id_for(const std::string& client_id) const {
    return NodeIdForAddress(configured_node_address_for(client_id));
}

const std::string& ClusterManager::self_node_id() const {
    return self_node_id_;
}

const std::string& ClusterManager::self_address() const {
    return self_address_;
}

std::vector<std::string> ClusterManager::peer_nodes() const {
    return peer_nodes_;
}

std::vector<std::string> ClusterManager::active_peer_nodes() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_nodes_;
}

bool ClusterManager::MarkNodeHealth(const std::string& peer_address, bool healthy) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = node_health_.find(peer_address);
    if (it == node_health_.end() || it->second == healthy) {
        return false;
    }

    it->second = healthy;
    RebuildActiveRingLocked();
    return true;
}

bool ClusterManager::IsNodeHealthy(const std::string& peer_address) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = node_health_.find(peer_address);
    return it != node_health_.end() && it->second;
}

grpc::Status ClusterManager::CheckNodeHealth(
    const std::string& peer_address,
    ratelimit::HealthCheckResponse* response) {
    NodeClient* client = GetOrCreateClient(peer_address);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kHealthTimeout);
    ratelimit::HealthCheckRequest request;
    return client->stub->HealthCheck(&context, request, response);
}

grpc::Status ClusterManager::ForwardCheckLimit(
    const ratelimit::LimitRequest& request,
    ratelimit::LimitResponse* response) {
    const std::string peer_address = node_address_for(request.client_id());
    return ForwardCheckLimitTo(peer_address, request, response);
}

grpc::Status ClusterManager::ForwardCheckLimitTo(
    const std::string& peer_address,
    const ratelimit::LimitRequest& request,
    ratelimit::LimitResponse* response) {
    NodeClient* client = GetOrCreateClient(peer_address);
    grpc::Status last_status;

    for (int attempt = 1; attempt <= kMaxForwardAttempts; ++attempt) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + kForwardTimeout);

        last_status = client->stub->CheckLimit(&context, request, response);
        if (last_status.ok()) {
            return last_status;
        }

        std::cerr << self_node_id_ << " forward attempt " << attempt
                  << " to " << peer_address
                  << " failed with code=" << last_status.error_code()
                  << " message=\"" << last_status.error_message() << "\""
                  << std::endl;

        if (!IsRetryableStatus(last_status) || attempt == kMaxForwardAttempts) {
            return last_status;
        }

        std::this_thread::sleep_for(kRetryBackoff);
    }

    return last_status;
}

ClusterManager::NodeClient* ClusterManager::GetOrCreateClient(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(peer_address);
    if (it == clients_.end()) {
        NodeClient client;
        client.channel = grpc::CreateChannel(peer_address, grpc::InsecureChannelCredentials());
        client.stub = ratelimit::RateLimiterService::NewStub(client.channel);
        it = clients_.emplace(peer_address, std::move(client)).first;
    }

    return &it->second;
}

void ClusterManager::RebuildActiveRingLocked() {
    active_ring_ = HashRing(100);
    active_nodes_.clear();

    for (const std::string& address : peer_nodes_) {
        if (node_health_[address]) {
            active_ring_.add_node(address);
            active_nodes_.push_back(address);
        }
    }
}

std::string ClusterManager::NodeIdForAddress(const std::string& address) const {
    auto it = address_to_node_id_.find(address);
    if (it == address_to_node_id_.end()) {
        return "unknown";
    }

    return it->second;
}

}  // namespace cluster
