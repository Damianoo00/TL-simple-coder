#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

struct OverlayBitmap {
    int width = 0, height = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> pixels;
    // Per-row nontransparent bounds avoid scanning the empty canvas on every video frame.
    std::vector<std::pair<int, int>> spans;
    void findSpans();
};
void blendOverlay(uint8_t* video, int stride, const OverlayBitmap& overlay);

class BrowserRenderer {
public:
    ~BrowserRenderer();
    bool start(const std::string& executable, const std::string& script,
               const std::string& url, int width, int height, int fps, const std::string& mode);
    void stop();
    std::shared_ptr<const OverlayBitmap> latest() const;
    bool failed() const { return m_failed.load(); }
private:
    void receive(int width, int height);
    bool readBytes(void* data, std::size_t size);
    int m_listener = -1, m_socket = -1;
    pid_t m_pid = -1;
    std::string m_directory;
    std::thread m_reader;
    std::atomic<bool> m_stopping{false}, m_failed{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::shared_ptr<const OverlayBitmap> m_latest;
};
