#pragma once

#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    GraphProcess startGraph(const std::string& executable, const std::string& configuration) {
        std::lock_guard lock(m_mutex);
        const auto id = m_nextId++;
        const auto path = m_directory / (std::to_string(id) + ".json");
        GraphProcess process{id, -1, "running", {}, {}, path};
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
            const int result = posix_spawn(&entry->second.pid, executable.c_str(), nullptr, nullptr, argv, environ);
            if (result != 0) throw std::system_error(result, std::generic_category(), "posix_spawn failed");
        } catch (...) {
            m_processes.erase(entry);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
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
