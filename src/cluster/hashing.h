#pragma once

#include <map>
#include <vector>
#include <string>

namespace cluster {

class HashRing {

private:

    std::map<size_t, std::string> ring;
    int virtual_nodes;

    size_t hash(const std::string& key) const;

public:

    HashRing(int vnodes = 100);

    void add_node(const std::string& node);

    void remove_node(const std::string& node);

    std::string get_node(const std::string& key);

};

}