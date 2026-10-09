#pragma once

#include "ThreadSafeQueue.hpp"

#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>

class Node {
public:
    Node();
    virtual ~Node();

    void init();
    virtual void configure(std::unordered_map<std::string, std::string> config);
    virtual int process();
    void connect(const Node& next, std::size_t output_port = 0, std::size_t input_port = 0);

    void start();
    void stop();

protected:
    std::atomic<bool> m_stop_requested{false};
    std::vector<std::shared_ptr<ThreadSafeQueue<int>>> m_output;
    std::vector<std::shared_ptr<ThreadSafeQueue<int>>> m_input;
    std::thread m_worker;
};
