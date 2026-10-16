#include "Supervisor.hpp"
#include "JsonGraphParser.hpp"

#include <crow.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <random>
#include <sstream>

#ifndef ASYNC_NODES_FRONTEND_DIR
#define ASYNC_NODES_FRONTEND_DIR "frontend/dist"
#endif

namespace {
using nlohmann::json;

json describe(const GraphProcess& process) {
    json result = {{"graph_id", process.id}, {"pid", process.pid}, {"status", process.status}};
    if (!process.documentId.empty()) result["document_id"] = process.documentId;
    if (process.exitCode) result["exit_code"] = *process.exitCode;
    if (process.signal) result["signal"] = *process.signal;
    return result;
}
crow::response response(int status, const json& document) {
    crow::response result(status, document.dump(-1, ' ', false, json::error_handler_t::replace));
    result.set_header("Content-Type", "application/json");
    return result;
}
crow::response staticFile(const std::filesystem::path& path, const std::string& type) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return crow::response(404, "Not found");
    std::string body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    crow::response result(200, std::move(body));
    result.set_header("Content-Type", type);
    return result;
}
bool validAssetName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '.' || c == '_' || c == '-';
           });
}
bool jsonContentType(const crow::request& request) {
    auto type = request.get_header_value("Content-Type");
    type = type.substr(0, type.find(';'));
    const auto first = type.find_first_not_of(" \t");
    const auto last = type.find_last_not_of(" \t");
    type = first == std::string::npos ? "" : type.substr(first, last - first + 1);
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return type == "application/json";
}
bool validId(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isxdigit(c) && !std::isupper(c);
    });
}
class GraphLibrary {
    std::mutex mutex;
    std::filesystem::path directory;
    std::mt19937_64 generator{std::random_device{}()};
    std::string newId() {
        static constexpr char hex[] = "0123456789abcdef";
        std::string id(32, '0');
        for (auto& c : id) c = hex[generator() & 15];
        return id;
    }
    std::filesystem::path path(const std::string& id) const { return directory / (id + ".json"); }
    std::optional<json> read(const std::string& id) const {
        if (!validId(id)) return {};
        const auto filePath = path(id);
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(filePath, error))) return {};
        std::ifstream file(filePath);
        if (!file) throw std::runtime_error("Cannot open graph document");
        return json::parse(file);
    }
    void write(const std::string& id, const json& document) {
        const auto temporary = directory / (id + ".tmp");
        try {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(temporary, std::ios::trunc);
            file << document.dump();
            file.close();
            std::filesystem::rename(temporary, path(id));
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }
public:
    GraphLibrary() {
        const char* configured = std::getenv("ASYNC_NODES_GRAPH_DIR");
        directory = configured && *configured ? configured : "graphs";
        std::filesystem::create_directories(directory);
    }
    static json validate(const std::string& body) {
        auto document = json::parse(body);
        if (!document.is_object() || !document.contains("name") || !document["name"].is_string() ||
            document["name"].get<std::string>().empty() || document["name"].get<std::string>().size() > 200 ||
            !document.contains("configuration") || !document["configuration"].is_object() ||
            !document.contains("editor") || !document["editor"].is_object()) {
            throw std::invalid_argument("Expected name, configuration, and editor");
        }
        JsonGraphParser::validateJson(document["configuration"].dump());
        return {{"name", document["name"]}, {"configuration", document["configuration"]},
                {"editor", document["editor"]}};
    }
    json list() {
        std::lock_guard lock(mutex);
        auto result = json::array();
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto id = entry.path().stem().string();
            if (entry.path().extension() != ".json" || !validId(id)) continue;
            if (auto document = read(id)) result.push_back(*document);
        }
        return result;
    }
    std::optional<json> get(const std::string& id) {
        std::lock_guard lock(mutex);
        return read(id);
    }
    json create(json document) {
        std::lock_guard lock(mutex);
        std::string id;
        do { id = newId(); } while (std::filesystem::exists(path(id)));
        document["id"] = id;
        write(id, document);
        return document;
    }
    std::optional<json> update(const std::string& id, json document) {
        std::lock_guard lock(mutex);
        if (!read(id)) return {};
        document["id"] = id;
        write(id, document);
        return document;
    }
    bool remove(const std::string& id) {
        std::lock_guard lock(mutex);
        if (!read(id)) return false;
        return std::filesystem::remove(path(id));
    }
};
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
    GraphLibrary library;
    crow::SimpleApp app;
    CROW_ROUTE(app, "/").methods(crow::HTTPMethod::GET)
    ([] { return staticFile(std::filesystem::path(ASYNC_NODES_FRONTEND_DIR) / "index.html", "text/html; charset=utf-8"); });
    CROW_ROUTE(app, "/assets/<string>").methods(crow::HTTPMethod::GET)
    ([](const std::string& name) {
        if (!validAssetName(name)) return crow::response(404, "Not found");
        const auto extension = std::filesystem::path(name).extension();
        std::string type;
        if (extension == ".js") type = "text/javascript; charset=utf-8";
        else if (extension == ".css") type = "text/css; charset=utf-8";
        else if (extension == ".svg") type = "image/svg+xml";
        else if (extension == ".png") type = "image/png";
        else if (extension == ".jpg" || extension == ".jpeg") type = "image/jpeg";
        else if (extension == ".woff2") type = "font/woff2";
        else return crow::response(404, "Not found");
        return staticFile(std::filesystem::path(ASYNC_NODES_FRONTEND_DIR) / "assets" / name, type);
    });
    app.exception_handler([](crow::response& result) {
        try { throw; }
        catch (const std::exception& error) {
            std::cerr << "API: " << error.what() << '\n';
            result = response(500, {{"error", "Internal server error"}});
        } catch (...) { result = response(500, {{"error", "Internal server error"}}); }
    });

    CROW_ROUTE(app, "/graphs").methods(crow::HTTPMethod::POST)
    ([&](const crow::request& request) {
        if (!jsonContentType(request)) return response(415, {{"error", "Expected Content-Type: application/json"}});
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

    CROW_ROUTE(app, "/library").methods(crow::HTTPMethod::GET)
    ([&] { return response(200, library.list()); });
    CROW_ROUTE(app, "/library").methods(crow::HTTPMethod::POST)
    ([&](const crow::request& request) {
        if (!jsonContentType(request)) return response(415, {{"error", "Expected Content-Type: application/json"}});
        if (request.body.size() > 1024 * 1024) return response(413, {{"error", "Document exceeds 1 MiB"}});
        json document;
        try { document = GraphLibrary::validate(request.body); }
        catch (const std::exception& error) { return response(400, {{"error", error.what()}}); }
        document = library.create(std::move(document));
        auto result = response(201, document);
        result.set_header("Location", "/library/" + document["id"].get<std::string>());
        return result;
    });
    CROW_ROUTE(app, "/library/<string>").methods(crow::HTTPMethod::GET)
    ([&](const std::string& id) {
        auto document = library.get(id);
        return document ? response(200, *document) : response(404, {{"error", "Document not found"}});
    });
    CROW_ROUTE(app, "/library/<string>").methods(crow::HTTPMethod::PUT)
    ([&](const crow::request& request, const std::string& id) {
        if (!validId(id)) return response(404, {{"error", "Document not found"}});
        if (!jsonContentType(request)) return response(415, {{"error", "Expected Content-Type: application/json"}});
        if (request.body.size() > 1024 * 1024) return response(413, {{"error", "Document exceeds 1 MiB"}});
        json document;
        try { document = GraphLibrary::validate(request.body); }
        catch (const std::exception& error) { return response(400, {{"error", error.what()}}); }
        auto saved = library.update(id, std::move(document));
        return saved ? response(200, *saved) : response(404, {{"error", "Document not found"}});
    });
    CROW_ROUTE(app, "/library/<string>").methods(crow::HTTPMethod::DELETE)
    ([&](const std::string& id) {
        return library.remove(id) ? response(200, {{"id", id}}) : response(404, {{"error", "Document not found"}});
    });
    CROW_ROUTE(app, "/library/<string>/start").methods(crow::HTTPMethod::POST)
    ([&](const std::string& id) {
        auto document = library.get(id);
        if (!document) return response(404, {{"error", "Document not found"}});
        try {
            auto process = supervisor.startGraph(executable, (*document)["configuration"].dump(), id);
            auto result = describe(process);
            result["status"] = "starting";
            auto accepted = response(202, result);
            accepted.set_header("Location", "/graphs/" + std::to_string(process.id));
            return accepted;
        } catch (const std::logic_error& error) { return response(409, {{"error", error.what()}}); }
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
    CROW_ROUTE(app, "/graphs/<uint>/logs").methods(crow::HTTPMethod::GET)
    ([&](const crow::request& request, std::uint64_t id) {
        std::uint64_t since = 0;
        if (const char* raw = request.url_params.get("since")) {
            const std::string_view value(raw);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), since);
            if (value.empty() || error != std::errc{} || end != value.data() + value.size())
                return response(400, {{"error", "since must be an unsigned integer"}});
        }
        auto logs = supervisor.getLogs(id, since);
        return logs ? response(200, {{"text", logs->text}, {"offset", logs->offset}})
                    : response(404, {{"error", "Graph not found"}});
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
