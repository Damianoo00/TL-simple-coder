#include "Node.hpp"

#include <cstdio>
#include <chrono>

NodeBase::~NodeBase() {
    stop();
}

int NodeBase::configure(std::unordered_map<std::string, std::string>) {
    m_status = Success;
    return Success;
}

int NodeBase::fail(Result code, const char* detail, std::size_t port, int stream_id) {
    m_status = code;
    std::fprintf(stderr, "%s: code=%d port=%zu stream_id=%d: %s\n",
                 nodeType(), code, port, stream_id, detail);
    return code;
}

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
        const auto started = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[info] %s: processing started\n", nodeType());
        std::size_t steps = 0;
        int result = Finished;
        try {
            while ((result = process()) >= 0) ++steps;
        } catch (const std::exception& error) {
            result = MediaError;
            m_status = result;
            std::fprintf(stderr, "Node: %s\n", error.what());
        } catch (...) {
            result = MediaError;
            m_status = result;
            std::fprintf(stderr, "Node: unknown exception\n");
        }
        if (result < Finished) m_status = result;
        // An ended consumer must release producers waiting on its bounded input.
        for (auto& input : m_input) input->close();
        closeOutputs();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "[info] %s: processing stopped result=%d steps=%zu elapsed=%.3fs stop_requested=%s\n",
                     nodeType(), result, steps, elapsed, m_stop_requested.load() ? "yes" : "no");
        m_result.store(result, std::memory_order_relaxed);
        m_finished.store(true, std::memory_order_release);
    });
}

void NodeBase::requestStop() {
    m_stop_requested = true;
    for (auto& input : m_input) input->close();
    closeOutputs();
}

void NodeBase::stop() {
    requestStop();
    join();
}

void NodeBase::join() {
    if (m_worker.joinable()) m_worker.join();
}
