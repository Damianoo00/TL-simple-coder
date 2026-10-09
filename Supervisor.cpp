#include "Supervisor.hpp"
#include "JsonGraphParser.hpp"

#include <crow.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace {
using nlohmann::json;

json describe(const GraphProcess& process) {
    json result = {{"graph_id", process.id}, {"pid", process.pid}, {"status", process.status}};
    if (process.exitCode) result["exit_code"] = *process.exitCode;
    if (process.signal) result["signal"] = *process.signal;
    return result;
}
crow::response response(int status, const json& document) {
    crow::response result(status, document.dump());
    result.set_header("Content-Type", "application/json");
    return result;
}
}

int main(int argc, char* argv[]) try {
    if (argc > 3) throw std::runtime_error("Usage: supervisor [graph_worker_path] [port]");
    const std::string executable = argc >= 2 ? argv[1] : "./graph_worker";
    unsigned long port = 8080;
    if (argc == 3) {
        std::size_t consumed;
        port = std::stoul(argv[2], &consumed);
        if (consumed != std::string(argv[2]).size() || port == 0 || port > 65535) {
            throw std::runtime_error("port must be between 1 and 65535");
        }
    }
    Supervisor supervisor;
    crow::SimpleApp app;
    app.exception_handler([](crow::response& result) {
        try { throw; }
        catch (const std::exception& error) {
            std::cerr << "API: " << error.what() << '\n';
            result = response(500, {{"error", "Internal server error"}});
        } catch (...) { result = response(500, {{"error", "Internal server error"}}); }
    });

    CROW_ROUTE(app, "/graphs").methods(crow::HTTPMethod::POST)
    ([&](const crow::request& request) {
        auto type = request.get_header_value("Content-Type");
        type = type.substr(0, type.find(';'));
        const auto first = type.find_first_not_of(" \t");
        const auto last = type.find_last_not_of(" \t");
        type = first == std::string::npos ? "" : type.substr(first, last - first + 1);
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (type != "application/json") return response(415, {{"error", "Expected Content-Type: application/json"}});
        if (request.body.size() > 1024 * 1024) return response(413, {{"error", "Configuration exceeds 1 MiB"}});
        try { JsonGraphParser::validateJson(request.body); }
        catch (const std::exception& error) { return response(400, {{"error", error.what()}}); }
        auto process = supervisor.startGraph(executable, request.body);
        auto document = describe(process);
        document["status"] = "starting";
        auto result = response(202, document);
        result.set_header("Location", "/graphs/" + std::to_string(process.id));
        return result;
    });

    CROW_ROUTE(app, "/graphs").methods(crow::HTTPMethod::GET)
    ([&] {
        auto document = json::array();
        for (const auto& process : supervisor.listGraphs()) document.push_back(describe(process));
        return response(200, document);
    });
    CROW_ROUTE(app, "/graphs/<uint>").methods(crow::HTTPMethod::GET)
    ([&](std::uint64_t id) {
        auto process = supervisor.getGraph(id);
        return process ? response(200, describe(*process)) : response(404, {{"error", "Graph not found"}});
    });
    CROW_ROUTE(app, "/graphs/<uint>").methods(crow::HTTPMethod::DELETE)
    ([&](std::uint64_t id) {
        auto process = supervisor.stopGraph(id);
        return process ? response(200, describe(*process)) : response(404, {{"error", "Graph not found"}});
    });

    std::jthread reaper([&](std::stop_token token) {
        while (!token.stop_requested()) {
            try { supervisor.reap(); }
            catch (const std::exception& error) { std::cerr << "Reaper: " << error.what() << '\n'; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    app.bindaddr("127.0.0.1").port(static_cast<std::uint16_t>(port)).concurrency(4).run();
    reaper.request_stop();
    reaper.join();
    supervisor.stopAll();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Supervisor: " << error.what() << '\n';
    return 1;
}
