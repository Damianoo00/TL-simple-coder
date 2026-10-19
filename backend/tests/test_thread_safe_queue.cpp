#include "core/ThreadSafeQueue.hpp"

#include <chrono>
#include <algorithm>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Action>
void requireThrows(Action action) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("Expected queue configuration exception");
}

int main() {
    {
        ThreadSafeQueue<int> queue;
        for (int i = 0; i < 256; ++i)
            require(queue.push(i), "Default queue rejected data before capacity");
        auto producer = std::async(std::launch::async, [&] { return queue.push(256); });
        const bool blocked = producer.wait_for(20ms) == std::future_status::timeout;
        queue.close();
        const bool accepted = producer.get();
        require(blocked && !accepted, "Default queue must block at 256 elements");
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(0);
        for (int i = 0; i < 1000; ++i)
            require(queue.push(i), "Unbounded queue rejected data");
        queue.close();
        for (int i = 0; i < 1000; ++i)
            require(queue.pop() == i, "Unbounded queue reordered data");
        require(!queue.pop(), "Unbounded queue did not drain");
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(1);
        queue.push(0);
        std::vector<std::future<bool>> producers;
        for (int i = 0; i < 4; ++i)
            producers.push_back(std::async(std::launch::async, [&] { return queue.push(1); }));
        queue.close();
        for (auto& producer : producers)
            require(!producer.get(), "Close did not release every producer");
        require(queue.pop() == 0 && !queue.pop(), "Waiting producers changed closed queue");
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(7);
        std::vector<std::future<std::vector<int>>> consumers;
        for (int i = 0; i < 4; ++i) {
            consumers.push_back(std::async(std::launch::async, [&] {
                std::vector<int> values;
                while (auto value = queue.pop()) values.push_back(*value);
                return values;
            }));
        }
        std::vector<std::future<bool>> producers;
        for (int i = 0; i < 4; ++i) {
            producers.push_back(std::async(std::launch::async, [&, i] {
                for (int value = i * 1000; value < (i + 1) * 1000; ++value)
                    if (!queue.push(value)) return false;
                return true;
            }));
        }
        bool produced = true;
        for (auto& producer : producers) produced &= producer.get();
        queue.close();
        std::vector<int> values;
        for (auto& consumer : consumers) {
            auto batch = consumer.get();
            values.insert(values.end(), batch.begin(), batch.end());
        }
        std::sort(values.begin(), values.end());
        require(produced && values.size() == 4000, "Concurrent producers/consumers lost data");
        for (int i = 0; i < 4000; ++i)
            require(values[i] == i, "Concurrent producers/consumers duplicated or changed data");
    }
    {
        ThreadSafeQueue<std::unique_ptr<int>> queue;
        require(queue.push(std::make_unique<int>(7)), "Move-only push failed");
        require(queue.push(std::make_unique<int>(9)), "Second push failed");
        queue.close();
        queue.close();
        require(!queue.push(std::make_unique<int>(11)), "Closed queue accepted data");
        auto first = queue.pop();
        auto second = queue.pop();
        require(first && **first == 7 && second && **second == 9, "Close lost data or FIFO order");
        require(!queue.pop() && !queue.pop(), "Drained queue did not retain EOF");
        requireThrows<std::logic_error>([&] { queue.setCapacity(1); });
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(1);
        queue.push(1);
        requireThrows<std::logic_error>([&] { queue.setCapacity(2); });
        std::promise<void> entered;
        auto producer = std::async(std::launch::async, [&] {
            entered.set_value();
            return queue.push(2);
        });
        entered.get_future().wait();
        const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
        auto first = queue.pop();
        const bool accepted = producer.get(); // CTest bounds a missing wakeup; no scheduling deadline.
        queue.close();
        require(blocked && accepted, "Pop did not release bounded producer");
        require(first == 1 && queue.pop() == 2 && !queue.pop(), "Bounded FIFO lost data");
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(1);
        queue.push(1);
        std::promise<void> entered;
        auto producer = std::async(std::launch::async, [&] {
            entered.set_value();
            return queue.push(2);
        });
        entered.get_future().wait();
        const bool blocked = producer.wait_for(50ms) == std::future_status::timeout;
        queue.close();
        require(blocked && !producer.get(), "Close did not reject waiting producer");
        require(queue.pop() == 1 && !queue.pop(), "Close changed buffered data");
    }
    {
        ThreadSafeQueue<int> queue;
        std::vector<std::future<std::optional<int>>> consumers;
        for (int i = 0; i < 4; ++i)
            consumers.push_back(std::async(std::launch::async, [&] { return queue.pop(); }));
        const bool blocked = consumers.front().wait_for(50ms) == std::future_status::timeout;
        queue.close();
        require(blocked, "Empty queue did not block consumer");
        for (auto& consumer : consumers)
            require(!consumer.get(), "Close did not wake all consumers with EOF");
    }
    {
        ThreadSafeQueue<int> queue;
        queue.setCapacity(3);
        auto producer = std::async(std::launch::async, [&] {
            for (int i = 0; i < 10000; ++i)
                if (!queue.push(i)) return false;
            queue.close();
            return true;
        });
        int expected = 0;
        bool ordered = true;
        while (auto value = queue.pop()) ordered &= *value == expected++;
        require(producer.get() && ordered && expected == 10000, "Concurrent transfer lost or reordered values");
    }
}
