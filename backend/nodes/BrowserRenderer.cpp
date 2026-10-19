#include "BrowserRenderer.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char** environ;

void OverlayBitmap::findSpans() {
    spans.resize(height);
    for (int y = 0; y < height; ++y) {
        const auto row = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        int begin = 0, end = width;
        while (begin < end && row[begin * 4 + 3] == 0) ++begin;
        while (end > begin && row[(end - 1) * 4 + 3] == 0) --end;
        spans[y] = {begin, end};
    }
}
void blendOverlay(uint8_t* video, int stride, const OverlayBitmap& overlay) {
    for (int y = 0; y < overlay.height; ++y) {
        const auto [begin, end] = overlay.spans[y];
        auto dst = video + static_cast<std::ptrdiff_t>(y) * stride + begin * 4;
        auto src = overlay.pixels.data() + (static_cast<std::size_t>(y) * overlay.width + begin) * 4;
        for (int x = begin; x < end; ++x, dst += 4, src += 4) {
            const unsigned alpha = src[3];
            if (!alpha) continue;
            if (alpha == 255) { std::memcpy(dst, src, 4); continue; }
            for (int c = 0; c < 3; ++c)
                dst[c] = static_cast<uint8_t>(std::min(255u, src[c] + (dst[c] * (255 - alpha) + 127) / 255));
            dst[3] = 255; // The underlying video is opaque.
        }
    }
}
BrowserRenderer::~BrowserRenderer() { stop(); }
std::shared_ptr<const OverlayBitmap> BrowserRenderer::latest() const {
    std::lock_guard lock(m_mutex);
    return m_latest;
}
bool BrowserRenderer::start(const std::string& executable, const std::string& script,
                            const std::string& url, int width, int height, int fps, const std::string& mode) {
    stop();
    m_stopping = false; m_failed = false;
    { std::lock_guard lock(m_mutex); m_latest.reset(); }
    char directory[] = "/tmp/async-overlay-XXXXXX";
    if (!mkdtemp(directory)) return false;
    m_directory = directory;
    m_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = m_directory + "/frames.sock";
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (m_listener < 0 || bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(m_listener, 1) < 0) { stop(); return false; }
    std::vector<std::string> arguments{executable};
    // Pass startup switches before Electron initializes Chromium, rather than
    // appending them later from the application's JavaScript entry point.
    const char* noSandbox = std::getenv("ASYNC_NODES_OVERLAY_NO_SANDBOX");
    if (noSandbox && std::strcmp(noSandbox, "1") == 0) {
        arguments.emplace_back("--no-sandbox");
        std::fprintf(stderr, "BrowserRenderer: launching with --no-sandbox (explicit environment opt-in)\n");
    } else {
        std::fprintf(stderr, "BrowserRenderer: Chromium sandbox enabled; ASYNC_NODES_OVERLAY_NO_SANDBOX=%s\n",
            noSandbox ? noSandbox : "<unset>");
    }
    arguments.insert(arguments.end(), {script, "--overlay-socket=" + path,
        "--overlay-url=" + url, "--overlay-width=" + std::to_string(width),
        "--overlay-height=" + std::to_string(height), "--overlay-fps=" + std::to_string(fps),
        "--overlay-mode=" + mode});
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    const int result = posix_spawnp(&m_pid, executable.c_str(), nullptr, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    if (result) {
        m_pid = -1;
        std::fprintf(stderr, "BrowserRenderer: cannot start %s: %s\n", executable.c_str(), std::strerror(result));
        stop(); return false;
    }
    m_reader = std::thread([this, width, height] {
        try { receive(width, height); }
        catch (const std::exception& error) { std::fprintf(stderr, "BrowserRenderer: %s\n", error.what()); }
        { std::lock_guard lock(m_mutex); if (!m_stopping) m_failed = true; }
        m_ready.notify_all();
    });
    std::unique_lock lock(m_mutex);
    const bool ready = m_ready.wait_for(lock, std::chrono::seconds(30), [this] { return m_latest || m_failed; });
    const bool success = ready && m_latest && !m_failed;
    lock.unlock();
    if (!success) { std::fprintf(stderr, "BrowserRenderer: no ready overlay within startup timeout\n"); stop(); }
    return success;
}
bool BrowserRenderer::readBytes(void* data, std::size_t size) {
    auto bytes = static_cast<uint8_t*>(data);
    while (size && !m_stopping) {
        pollfd descriptor{m_socket, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (!ready) continue;
        const auto count = read(m_socket, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count; size -= count;
    }
    return !size;
}
void BrowserRenderer::receive(int width, int height) {
    while (!m_stopping) {
        pollfd descriptor{m_listener, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0 && errno != EINTR) return;
        if (ready > 0) { m_socket = accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC); break; }
        siginfo_t info{};
        if (waitid(P_PID, m_pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == m_pid) return;
    }
    if (m_socket < 0) return;
    const auto started = std::chrono::steady_clock::now();
    auto lastLog = started;
    std::size_t updates = 0;
    while (!m_stopping) {
        std::array<uint8_t, 24> header{};
        if (!readBytes(header.data(), header.size())) return;
        auto integer = [&header](int offset) {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= uint32_t(header[offset + i]) << (8 * i);
            return value;
        };
        const auto size = static_cast<std::size_t>(width) * height * 4;
        if (std::memcmp(header.data(), "OVL1", 4) || integer(4) != static_cast<unsigned>(width) ||
            integer(8) != static_cast<unsigned>(height) || integer(12) != size) {
            std::fprintf(stderr, "BrowserRenderer: malformed frame header\n"); return;
        }
        auto bitmap = std::make_shared<OverlayBitmap>();
        bitmap->width = width; bitmap->height = height;
        for (int i = 0; i < 8; ++i) bitmap->sequence |= uint64_t(header[16 + i]) << (8 * i);
        bitmap->pixels.resize(size);
        if (!readBytes(bitmap->pixels.data(), size)) return;
        bitmap->findSpans();
        { std::lock_guard lock(m_mutex); m_latest = std::move(bitmap); }
        m_ready.notify_all();
        ++updates;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "[overlay] received_fps=%.1f (static pages paint only on changes)\n",
                updates / std::chrono::duration<double>(now - lastLog).count());
            updates = 0; lastLog = now;
        }
    }
}
void BrowserRenderer::stop() {
    m_stopping = true;
    // Let Electron shut down its Chromium children in order. Kill the group only on timeout.
    if (m_pid > 0) kill(m_pid, SIGTERM);
    if (m_reader.joinable()) m_reader.join();
    if (m_socket >= 0) { close(m_socket); m_socket = -1; }
    if (m_listener >= 0) { close(m_listener); m_listener = -1; }
    if (m_pid > 0) {
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            const auto result = waitpid(m_pid, &status, WNOHANG);
            if (result == m_pid || (result < 0 && errno == ECHILD)) break;
            if (i == 49) { kill(-m_pid, SIGKILL); while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {} }
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        m_pid = -1;
    }
    if (!m_directory.empty()) {
        unlink((m_directory + "/frames.sock").c_str()); rmdir(m_directory.c_str()); m_directory.clear();
    }
}
