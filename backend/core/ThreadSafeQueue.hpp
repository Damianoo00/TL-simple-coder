#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

class QueueBase {
public:
    static constexpr std::size_t DefaultCapacity = 256;
    virtual ~QueueBase() = default;
    virtual void close() = 0;
    virtual void setCapacity(std::size_t capacity) = 0;
};

template<typename T>
class ThreadSafeQueue : public QueueBase {
private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_closed = false;
    std::size_t m_capacity = QueueBase::DefaultCapacity;

public:
    bool push(T value) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_closed || !m_capacity || m_queue.size() < m_capacity; });

            if (m_closed)
                return false;

            m_queue.push(std::move(value));
        }

        m_cv.notify_all();
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
        m_cv.notify_all();
        return value;
    }

    // Configure before graph start. Zero preserves the original unbounded behavior.
    void setCapacity(std::size_t capacity) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_queue.empty() || m_closed) throw std::logic_error("Queue capacity must be set before use");
        m_capacity = capacity;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }

        m_cv.notify_all();
    }
};
