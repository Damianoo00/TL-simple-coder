#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Node.hpp"

class Graph {
public:
    enum class State { Running, Completed, Failed, Cancelled };
    struct Status {
        State state;
        std::optional<std::size_t> failedNode;
        int error = NodeBase::Success;
    };

private:
    std::vector<std::unique_ptr<NodeBase>> m_nodes;
    Status m_status{State::Running, std::nullopt, NodeBase::Success};
    bool m_started = false;

    void requestStopAll() {
        for (auto& node : m_nodes) node->requestStop();
    }

    void joinAll() {
        for (auto& node : m_nodes) node->join();
    }

    void recordFailure() {
        if (m_status.state == State::Failed || m_status.state == State::Completed) return;
        for (std::size_t index = 0; index < m_nodes.size(); ++index) {
            const auto& node = m_nodes[index];
            if (node->finished() && node->result() < NodeBase::Finished) {
                m_status = {State::Failed, index, node->result()};
                return;
            }
        }
    }

public:
    ~Graph() { stop(); }

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
        if (m_started) return;
        if (m_status.state != State::Running)
            throw std::logic_error("Stopped graph cannot be started");
        m_started = true;
        std::size_t index = 0;
        try {
            for (; index < m_nodes.size(); ++index) m_nodes[index]->start();
        } catch (...) {
            m_status = {State::Failed, index, NodeBase::MediaError};
            requestStopAll();
            joinAll();
            throw;
        }
    }

    Status poll() {
        if (!m_started || m_status.state != State::Running) return m_status;
        recordFailure();
        if (m_status.state == State::Failed) {
            requestStopAll();
            return m_status;
        }
        bool allFinished = true;
        for (const auto& node : m_nodes) allFinished &= node->finished();
        if (allFinished) {
            // A node may publish failure between the first scan and this one.
            recordFailure();
            if (m_status.state == State::Failed) requestStopAll();
            else m_status.state = State::Completed;
        }
        return m_status;
    }

    Status wait() {
        if (!m_started) throw std::logic_error("Graph has not been started");
        while (poll().state == State::Running)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        joinAll();
        return m_status;
    }

    void stop() {
        if (!m_started) {
            m_status.state = State::Cancelled;
            return;
        }
        poll();
        if (m_status.state == State::Running) {
            m_status.state = State::Cancelled;
            requestStopAll();
        }
        joinAll();
        recordFailure();
    }
};
