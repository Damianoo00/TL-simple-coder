#pragma once

#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern char** environ;

struct GraphProcess {
    std::uint64_t id;
    pid_t pid;
    std::string status = "running";
    std::optional<int> exitCode;
    std::optional<int> signal;
    std::filesystem::path configuration;
    std::string documentId;
    std::filesystem::path log;
};

struct GraphLogs {
    std::string text;
    std::uint64_t offset;
};

class Supervisor {
    std::mutex m_mutex;
    std::map<std::uint64_t, GraphProcess> m_processes;
    std::uint64_t m_nextId = 1;
    std::filesystem::path m_directory;

    void refresh(GraphProcess& process) {
        if (process.status != "running") return;
        int status = 0;
        pid_t result;
        do {
            result = waitpid(process.pid, &status, WNOHANG);
        } while (result == -1 && errno == EINTR);
        if (result == 0) return;
        if (result == -1) {
            throw std::system_error(errno, std::generic_category(), "waitpid failed");
        }
        if (WIFEXITED(status)) {
            process.exitCode = WEXITSTATUS(status);
            process.status = *process.exitCode == 0 ? "stopped" : "failed";
        } else if (WIFSIGNALED(status)) {
            process.signal = WTERMSIG(status);
            process.status = "failed";
        }
        std::error_code ignored;
        std::filesystem::remove(process.configuration, ignored);
    }

    void stop(GraphProcess& process) {
        refresh(process);
        if (process.status != "running") return;
        if (kill(process.pid, SIGTERM) == -1 && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(), "kill failed");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (process.status == "running" && std::chrono::steady_clock::now() < deadline) {
            refresh(process);
            if (process.status == "running") std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (process.status == "running") {
            if (kill(process.pid, SIGKILL) == -1 && errno != ESRCH) {
                throw std::system_error(errno, std::generic_category(), "kill failed");
            }
            int status;
            pid_t result;
            do { result = waitpid(process.pid, &status, 0); } while (result == -1 && errno == EINTR);
            if (result == -1) throw std::system_error(errno, std::generic_category(), "waitpid failed");
            if (WIFSIGNALED(status)) process.signal = WTERMSIG(status);
            else if (WIFEXITED(status)) process.exitCode = WEXITSTATUS(status);
            std::error_code ignored;
            std::filesystem::remove(process.configuration, ignored);
        }
        process.status = "stopped";
    }

public:
    Supervisor() {
        auto pattern = (std::filesystem::temp_directory_path() / "async-nodes-XXXXXX").string();
        if (!mkdtemp(pattern.data())) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp failed");
        }
        m_directory = pattern;
    }
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    ~Supervisor() {
        try { stopAll(); } catch (...) {}
        std::error_code ignored;
        std::filesystem::remove_all(m_directory, ignored);
    }

    GraphProcess startGraph(const std::string& executable, const std::string& configuration,
                            const std::string& documentId = {}) {
        std::lock_guard lock(m_mutex);
        if (!documentId.empty()) {
            for (auto& [existingId, existing] : m_processes) {
                (void)existingId;
                if (existing.documentId == documentId) {
                    refresh(existing);
                    if (existing.status == "running") throw std::logic_error("Graph is already running");
                }
            }
        }
        const auto id = m_nextId++;
        const auto path = m_directory / (std::to_string(id) + ".json");
        const auto log = m_directory / (std::to_string(id) + ".log");
        GraphProcess process{id, -1, "running", {}, {}, path, documentId, log};
        // Allocate the registry entry before spawning so allocation failures cannot orphan a worker.
        auto [entry, inserted] = m_processes.emplace(id, process);
        (void)inserted;
        try {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(path);
            file << configuration;
            file.close();
            auto argument = path.string();
            char* const argv[] = {const_cast<char*>(executable.c_str()), argument.data(), nullptr};
            posix_spawn_file_actions_t actions;
            int result = posix_spawn_file_actions_init(&actions);
            if (result != 0) throw std::system_error(result, std::generic_category(), "spawn file actions init failed");
            result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (result == 0) result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            if (result == 0) result = posix_spawn(&entry->second.pid, executable.c_str(), &actions, nullptr, argv, environ);
            posix_spawn_file_actions_destroy(&actions);
            if (result != 0) throw std::system_error(result, std::generic_category(), "posix_spawn failed");
        } catch (...) {
            m_processes.erase(entry);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(log, ignored);
            throw;
        }
        return entry->second;
    }

    std::optional<GraphProcess> getGraph(std::uint64_t id) {
        std::lock_guard lock(m_mutex);
        auto entry = m_processes.find(id);
        if (entry == m_processes.end()) return {};
        refresh(entry->second);
        return entry->second;
    }
    std::optional<GraphLogs> getLogs(std::uint64_t id, std::uint64_t since = 0) {
        std::lock_guard lock(m_mutex);
        auto entry = m_processes.find(id);
        if (entry == m_processes.end()) return {};
        std::ifstream file(entry->second.log, std::ios::binary | std::ios::ate);
        if (!file) return GraphLogs{{}, 0};
        constexpr std::uint64_t limit = 64 * 1024;
        const auto end = static_cast<std::uint64_t>(file.tellg());
        const auto start = std::max(std::min(since, end), end > limit ? end - limit : std::uint64_t{0});
        file.seekg(static_cast<std::streamoff>(start));
        std::string text(static_cast<std::size_t>(end - start), '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        return GraphLogs{std::move(text), end};
    }
    std::vector<GraphProcess> listGraphs() {
        std::lock_guard lock(m_mutex);
        std::vector<GraphProcess> result;
        for (auto& [id, process] : m_processes) {
            refresh(process);
            result.push_back(process);
        }
        return result;
    }
    void reap() {
        std::lock_guard lock(m_mutex);
        for (auto& [id, process] : m_processes) refresh(process);
    }
    std::optional<GraphProcess> stopGraph(std::uint64_t id) {
        std::lock_guard lock(m_mutex);
        auto entry = m_processes.find(id);
        if (entry == m_processes.end()) return {};
        stop(entry->second);
        return entry->second;
    }
    void stopAll() {
        std::lock_guard lock(m_mutex);
        for (auto& [id, process] : m_processes) stop(process);
    }
};
