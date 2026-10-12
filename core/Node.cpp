#include "Node.hpp"

#include <cstdio>

NodeBase::~NodeBase() {
    stop();
}

void NodeBase::configure(std::unordered_map<std::string, std::string>) {}

void NodeBase::connect(NodeBase& next, std::size_t output_port,
                       std::size_t input_port) {
    const auto expected = m_output_types.at(output_port);
    const auto& queue = next.m_input.at(input_port);
    if (expected != std::type_index(typeid(*queue)))
        throw std::invalid_argument("Incompatible node port types");
    m_output.at(output_port) = queue;
}

void NodeBase::closeOutputs() {
    for (auto& output : m_output) {
        if (output) output->close();
    }
}

void NodeBase::start() {
    if (m_worker.joinable()) return;
    m_worker = std::thread([this]() {
        try {
            while (process() >= 0) {}
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Node: %s\n", error.what());
        }
        closeOutputs();
    });
}

void NodeBase::stop() {
    m_stop_requested = true;
    for (auto& input : m_input) input->close();
    if (m_worker.joinable()) m_worker.join();
}
