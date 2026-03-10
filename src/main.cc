#include<iostream>

#include "../Third-Party/Crow/include/crow.h"
#include "../src/manager/limiter_manager.h"
#include "../src/cluster/hashing.h"

using namespace std;
using namespace manager;
using namespace cluster;

int main(){
    crow::SimpleApp app;
    HashRing ring;
    limit_manager manager(3,1);

    ring.add_node("node1");
    ring.add_node("node2");
    ring.add_node("node3");
    
    CROW_ROUTE(app, "/request")
    ([&](const crow::request& req){

    const char* client = req.url_params.get("client_id");

    if(client == nullptr)
        return crow::response(400, "Missing Client id");

    std::string client_id(client);
    
  std::string node = ring.get_node(client_id);

    if(node == "node1") {
        if(manager.allow(client_id))
            return crow::response(200,"Allowed");
        return crow::response(429,"rate limited");
    }
    else {
        return crow::response(200,"Forward to node: " + node);
    }
    
    return crow::response(429, "rate limited");
    });
    CROW_ROUTE(app, "/metrics")
    ([&](){
    return crow::response(manager.get_metrics());
    });
    app.port(8000).multithreaded().run();

    return 0;
}