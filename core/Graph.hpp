#pragma once

#include <vector>
#include <memory>
#include <utility>
#include "Node.hpp"

class Graph{
    private:
        std::vector<std::unique_ptr<NodeBase>> m_nodes;

    public:
        ~Graph() {
            stop();
        }

        template<typename T, typename... Args>
        T& addNode(Args&&... args) {
            auto node = std::make_unique<T>(std::forward<Args>(args)...);
            T& ref = *node;
            m_nodes.push_back(std::move(node));
            return ref;
        }

        void connect(NodeBase& from, NodeBase& to, std::size_t output_port = 0,
                     std::size_t input_port = 0) {
            from.connect(to, output_port, input_port);
        }

        void start() {
            for (auto& node : m_nodes) {
                node->start();
            }
        }

        void stop() {
            for (auto& node : m_nodes) node->requestStop();
            for (auto& node : m_nodes) {
                node->stop();
            }
        }
};
