#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "node.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using ratelimit::RateLimiterService;
using ratelimit::LimitRequest;
using ratelimit::LimitResponse;

class RateLimiterClient {

    std::unique_ptr<RateLimiterService::Stub> stub_;

public:

    RateLimiterClient(std::shared_ptr<Channel> channel)
        : stub_(RateLimiterService::NewStub(channel)) {}

    void CheckLimit(std::string client_id) {

        LimitRequest request;

        request.set_client_id(client_id);
        request.set_tokens_requested(1);
        request.set_timestamp(time(NULL));
        request.set_request_id("grpc-client-demo");

        LimitResponse response;

        ClientContext context;

        Status status =
            stub_->CheckLimit(&context, request, &response);

        if (status.ok()) {

            std::cout << "Allowed: "
                      << response.allowed() << std::endl;

            std::cout << "Remaining tokens: "
                      << response.remaining_tokens() << std::endl;

            std::cout << "Served by node: "
                      << response.node_id() << std::endl;

            std::cout << "Used Redis: "
                      << response.used_redis() << std::endl;
        }
        else {
            std::cout << "RPC failed" << std::endl;
        }
    }
};

int main() {

    RateLimiterClient client(
        grpc::CreateChannel(
            "localhost:50051",
            grpc::InsecureChannelCredentials()));

    client.CheckLimit("user123");
}
