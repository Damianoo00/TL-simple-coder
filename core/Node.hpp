#pragma once

#include "ThreadSafeQueue.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

class NodeBase {
public:
    virtual ~NodeBase();
    virtual void configure(std::unordered_map<std::string, std::string> config);
    virtual int process() = 0;

    void start();
    void stop();
    void connect(NodeBase& next, std::size_t output_port = 0,
                 std::size_t input_port = 0);

protected:
    template<typename T>
    void addInput() {
        m_input.push_back(std::make_shared<ThreadSafeQueue<T>>());
    }

    template<typename T>
    void addOutput() {
        m_output.push_back(nullptr);
        m_output_types.emplace_back(typeid(ThreadSafeQueue<T>));
    }

    template<typename T>
    std::shared_ptr<ThreadSafeQueue<T>> input(std::size_t port = 0) const {
        auto queue = std::dynamic_pointer_cast<ThreadSafeQueue<T>>(m_input.at(port));
        if (!queue) throw std::logic_error("Incorrect input queue type");
        return queue;
    }

    template<typename T>
    std::shared_ptr<ThreadSafeQueue<T>> output(std::size_t port = 0) const {
        const auto& queue = m_output.at(port);
        if (!queue) return nullptr;
        auto typed = std::dynamic_pointer_cast<ThreadSafeQueue<T>>(queue);
        if (!typed) throw std::logic_error("Incorrect output queue type");
        return typed;
    }

    std::atomic<bool> m_stop_requested{false};

private:
    void closeOutputs();
    std::vector<std::shared_ptr<QueueBase>> m_input;
    std::vector<std::shared_ptr<QueueBase>> m_output;
    std::vector<std::type_index> m_output_types;
    std::thread m_worker;
};
