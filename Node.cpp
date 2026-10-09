#include "Node.hpp"

#include <cstdio>

Node::Node() {
    init();
}

Node::~Node() {
    stop();
}

void Node::init() {
    m_input.push_back(std::make_shared<ThreadSafeQueue<int>>());
    m_output.push_back(nullptr);
}

void Node::configure(std::unordered_map<std::string, std::string> config) {
    return;
}

void Node::connect(const Node& next, std::size_t output_port, std::size_t input_port){
    m_output.at(output_port) = next.m_input.at(input_port);
}

void Node::start() {
    if (m_worker.joinable())
        return;

    m_worker = std::thread([this]() {
        int res{0};
        while (true) {
            if (res = process(); res < 0){
                break;
            }
        }
    });
}

void Node::stop() {
    m_stop_requested = true;
    for (auto input : m_input){
        input->close();
    }
    

    if (m_worker.joinable()) {
        m_worker.join();
    }
}

int Node::process() {
    auto value = m_input.at(0)->pop();
    if (!value)
        return -1;
    if (m_output.at(0)) {
        m_output.at(0)->push(value.value());
    } else {
        std::printf("End of graph: %d\n", value.value());
    }
    return 0;
}
