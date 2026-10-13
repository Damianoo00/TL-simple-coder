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
    enum Result : int {
        Success = 0, Finished = -1, InvalidPorts = -2,
        OutputNotConnected = -3, InvalidConfigFrame = -4,
        DuplicateStreamId = -5, StreamIdChanged = -6,
        InvalidEndFrame = -7, InvalidDataFrame = -8,
        InvalidConfiguration = -9, NotConfigured = -10,
        MediaError = -11, AllocationFailed = -12, InvalidStreamId = -13,
        LateConfigFrame = -14, FrameBeforeConfig = -15,
        FrameAfterEnd = -16, UnknownFrameType = -17, InsufficientPorts = -18
    };
    // Configuration is only allowed before start. Read status before start or after stop.
    int status() const { return m_status; }
    virtual int configure(std::unordered_map<std::string, std::string> config);
    virtual int process() = 0;

    void start();
    void stop();
    void requestStop();
    void setQueueCapacity(std::size_t capacity) {
        for (auto& queue : m_input) queue->setCapacity(capacity);
    }
    void connect(NodeBase& next, std::size_t output_port = 0,
                 std::size_t input_port = 0);

protected:
    virtual const char* nodeType() const { return "Node"; }
    int fail(Result code, const char* detail, std::size_t port = 0, int stream_id = -1);
    int m_status = Success;
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

    // Port registration and reset are only permitted before graph start.
    void resetInputs() { m_input.clear(); }
    void resetOutputs() { m_output.clear(); m_output_types.clear(); }

    std::atomic<bool> m_stop_requested{false};

private:
    void closeOutputs();
    std::vector<std::shared_ptr<QueueBase>> m_input;
    std::vector<std::shared_ptr<QueueBase>> m_output;
    std::vector<std::type_index> m_output_types;
    std::thread m_worker;
};
