#pragma once

#include <vector>
#include <memory>
#include <utility>
#include "Node.hpp"

class Graph{
    private:
        std::vector<std::unique_ptr<Node>> m_nodes;

    public:
        ~Graph() {
            stop();
        }

        template<typename T = Node, typename... Args>
        T& addNode(Args&&... args) {
            auto node = std::make_unique<T>(std::forward<Args>(args)...);
            T& ref = *node;
            m_nodes.push_back(std::move(node));
            return ref;
        }

        void connect(Node& from, Node& to) {
            from.connect(to);
        }

        void start() {
            for (auto& node : m_nodes) {
                node->start();
            }
        }

        void stop() {
            for (auto& node : m_nodes) {
                node->stop();
            }
        }
};
