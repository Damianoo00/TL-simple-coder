#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_closed = false;

public:
    bool push(T value) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_closed)
                return false;

            m_queue.push(std::move(value));
        }

        m_cv.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cv.wait(lock, [this]() {
            return !m_queue.empty() || m_closed;
        });

        if (m_queue.empty())
            return std::nullopt;

        T value = std::move(m_queue.front());
        m_queue.pop();

        return value;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }

        m_cv.notify_all();
    }
};
