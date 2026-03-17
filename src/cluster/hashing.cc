#include "hashing.h"
#include <functional>

using namespace cluster;

HashRing::HashRing(int vnodes) : virtual_nodes(vnodes) {}

size_t HashRing::hash(const std::string& key) const {
    return std::hash<std::string>{}(key);
}

void HashRing::add_node(const std::string& node) {

    for(int i = 0; i < virtual_nodes; i++) {

        std::string vnode = node + "#" + std::to_string(i);

        ring[hash(vnode)] = node;
    }
}

void HashRing::remove_node(const std::string& node) {

    for(int i = 0; i < virtual_nodes; i++) {

        std::string vnode = node + "#" + std::to_string(i);

        ring.erase(hash(vnode));
    }
}

std::string HashRing::get_node(const std::string& key) const {

    if(ring.empty())
        return "";

    size_t h = hash(key);

    auto it = ring.lower_bound(h);

    if(it == ring.end())
        it = ring.begin();

    return it->second;
}
