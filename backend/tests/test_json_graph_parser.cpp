#include "api/JsonGraphParser.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Json = nlohmann::json;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Json node(std::string type, std::string name, Json configuration = Json::array(),
          Json connections = Json::array()) {
    return {{"type", std::move(type)}, {"name", std::move(name)},
            {"configuration", std::move(configuration)}, {"connections", std::move(connections)}};
}

Json packetGraph() {
    return {{"graph", Json::array({
        node("InputNode", "source", Json::array({{{"filename", "absent-input.mp4"}}}), Json::array({"sink"})),
        node("OutputNode", "sink", Json::array({{{"filename", "absent-output.mp4"}}}))
    })}};
}

Json portGraph() {
    return {{"graph", Json::array({
        node("InputNode", "source", Json::array({{{"filename", "absent-input.mp4"}}}), Json::array({"demux"})),
        node("DemuxNode", "demux", Json::array({{{"ports", "2"}}}),
             Json::array({Json{{"node", "mux"}, {"output_port", 0}, {"input_port", 1}},
                          Json{{"node", "mux"}, {"output_port", 1}, {"input_port", 0}}})),
        node("MuxerNode", "mux", Json::array({{{"ports", "2"}}}), Json::array({"sink"})),
        node("OutputNode", "sink", Json::array({{{"filename", "absent-output.mp4"}}}))
    })}};
}

void accepts(const std::string& label, const Json& document) {
    try { JsonGraphParser::validateJson(document.dump()); }
    catch (const std::exception& error) {
        throw std::runtime_error(label + " unexpectedly rejected: " + error.what());
    }
}

void rejects(const std::string& label, const Json& document, std::string_view expected) {
    try { JsonGraphParser::validateJson(document.dump()); }
    catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                label + " gave wrong error: " + error.what());
        return;
    }
    throw std::runtime_error(label + " unexpectedly accepted");
}

void schemaAndConfiguration() {
    accepts("basic packet graph", packetGraph());
    auto rtmp = packetGraph();
    rtmp["graph"][1]["type"] = "RTMPOutputNode";
    rtmp["graph"][1]["configuration"] = Json::array({{{"url", "rtmp://127.0.0.1:1935/live/test"}}});
    accepts("RTMP packet graph", rtmp);
    rtmp["graph"][1]["configuration"] = Json::array({{{"url", "https://example.org"}}});
    rejects("invalid RTMP URL", rtmp, "rtmp:// url");
    for (const auto& [label, document, expected] : std::vector<std::tuple<std::string, Json, std::string>>{
        {"root array", Json::array(), "cannot use at() with array"},
        {"missing graph", Json::object(), "graph"},
        {"empty graph", Json{{"graph", Json::array()}}, "graph must be a non-empty array"},
        {"graph object", Json{{"graph", Json::object()}}, "graph must be a non-empty array"},
        {"node scalar", Json{{"graph", Json::array({7})}}, "cannot use at() with number"},
    }) rejects(label, document, expected);

    struct Change { const char* name; Json value; const char* error; };
    for (const auto& change : std::vector<Change>{
        {"missing type", nullptr, "type"}, {"type number", 7, "type must be a non-empty string"},
        {"empty type", "", "type must be a non-empty string"}, {"unknown type", "Other", "Unknown node type"},
        {"missing name", nullptr, "name"}, {"name number", 7, "name must be a non-empty string"},
        {"empty name", "", "name must be a non-empty string"},
    }) {
        auto document = packetGraph();
        if (change.value.is_null()) document["graph"][0].erase(change.name == std::string("missing type") ? "type" : "name");
        else document["graph"][0][change.name == std::string("missing type") ||
                                        change.name == std::string("type number") ||
                                        change.name == std::string("empty type") ||
                                        change.name == std::string("unknown type") ? "type" : "name"] = change.value;
        rejects(change.name, document, change.error);
    }
    auto document = packetGraph(); document["graph"][1]["name"] = "source";
    rejects("duplicate name", document, "Duplicate node name: source");

    struct Mutation { const char* label; Json value; const char* error; };
    for (const auto& change : std::vector<Mutation>{
        {"configuration object", Json::object(), "configuration must be an array"},
        {"configuration entry scalar", Json::array({1}), "configuration entries must be objects"},
        {"configuration value number", Json::array({{{"filename", 1}}}), "Configuration values must be strings"},
        {"missing filename", Json::array(), "Missing non-empty filename"},
        {"empty filename", Json::array({{{"filename", ""}}}), "Missing non-empty filename"},
        {"duplicate key", Json::array({{{"filename", "a"}}, {{"filename", "b"}}}), "Duplicate configuration key"},
    }) {
        document = packetGraph(); document["graph"][1]["configuration"] = change.value;
        rejects(change.label, document, change.error);
    }
    document = packetGraph(); document["graph"][1].erase("configuration");
    rejects("missing configuration", document, "configuration");
    document = packetGraph(); document["graph"][1].erase("connections");
    rejects("missing connections", document, "connections");
    document = packetGraph(); document["graph"][1]["connections"] = Json::object();
    rejects("connections object", document, "connections must be an array");
}

void capacityAndPorts() {
    for (const auto& value : {Json(1), Json(65536)}) {
        auto document = packetGraph(); document["queue_capacity"] = value;
        accepts("valid capacity " + value.dump(), document);
    }
    for (const auto& value : {Json(0), Json(-1), Json(65537), Json(1.5), Json("4"), Json(true), Json(nullptr)}) {
        auto document = packetGraph(); document["queue_capacity"] = value;
        rejects("invalid capacity " + value.dump(), document, "queue_capacity must be an integer from 1 to 65536");
    }
    accepts("explicit crossed ports", portGraph());
    auto document = portGraph(); document["graph"][1]["configuration"] = Json::array();
    document["graph"][2]["configuration"] = Json::array();
    accepts("default two ports", document);
    document = portGraph(); document["graph"][1]["configuration"] = Json::array({{{"ports", "1"}}});
    rejects("demux output bound", document, "Invalid output port for node demux");
    document = portGraph(); document["graph"][2]["configuration"] = Json::array({{{"ports", "1"}}});
    rejects("mux input bound", document, "Invalid input port for node mux");
    for (const auto& value : {"0", "-1", "abc", "1x", ""}) {
        for (int index : {1, 2}) {
            document = portGraph(); document["graph"][index]["configuration"] = Json::array({{{"ports", value}}});
            rejects(std::string("invalid ports ") + value, document, "ports must be a positive integer");
        }
    }
    document = {{"graph", Json::array({node("DemuxNode", "demux", Json::array({{{"ports", "2"}}}))})}};
    accepts("unused demux outputs allowed", document);
}

void topologyAndTypes() {
    auto document = packetGraph(); document["graph"][0]["connections"] = Json::array({"unknown"});
    rejects("unknown target", document, "Unknown connection target unknown");
    document = packetGraph(); document["graph"][0]["connections"] = Json::array({""});
    rejects("empty target", document, "Empty connection target");
    document = packetGraph(); document["graph"][0]["connections"] = Json::array({7});
    rejects("target scalar", document, "Connection must be a node name or a port mapping");
    document = packetGraph(); document["graph"][0]["connections"] = Json::array({Json::object()});
    rejects("missing mapping target", document, "node");
    for (const auto& value : {Json(""), Json(1)}) {
        document = packetGraph(); document["graph"][0]["connections"] = Json::array({Json{{"node", value}}});
        rejects("invalid mapping target", document, "node must be a non-empty string");
    }
    for (const auto& key : {"output_port", "input_port"}) {
        for (const auto& value : {Json(-1), Json("0"), Json(0.5), Json(true)}) {
            document = packetGraph(); document["graph"][0]["connections"] = Json::array({Json{{"node", "sink"}, {key, value}}});
            rejects(std::string(key) + "=" + value.dump(), document, std::string(key) + " must be a non-negative integer");
        }
        document = packetGraph(); document["graph"][0]["connections"] = Json::array({Json{{"node", "sink"}, {key, 1}}});
        rejects(std::string(key) + " out of range", document, key == std::string("output_port") ? "Invalid output port" : "Invalid input port");
    }
    document = packetGraph(); document["graph"][1]["connections"] = Json::array({"source"});
    rejects("output node cannot send", document, "Invalid output port");
    document = packetGraph(); document["graph"][0]["connections"] = Json::array({"source"});
    rejects("input node cannot receive", document, "Invalid input port");
    document = packetGraph(); document["graph"][0]["connections"] = Json::array({"sink", "sink"});
    rejects("implicit output bound", document, "Invalid output port");
    document = portGraph(); document["graph"][1]["connections"][1]["output_port"] = 0;
    rejects("output collision", document, "Output port is connected more than once");
    document = portGraph(); document["graph"][1]["connections"][1]["input_port"] = 1;
    rejects("input collision", document, "Input port is connected more than once");
    document = portGraph(); document["graph"][1]["connections"].erase(1);
    rejects("mux requires every input", document, "All MuxerNode input ports must be connected");

    for (const auto& type : {"InputNode", "OutputNode", "RTMPOutputNode", "DemuxNode", "MuxerNode", "VideoDecoderNode",
                             "AudioDecoderNode", "VideoEncoderNode", "AudioEncoderNode", "VideoScalerNode",
                             "VideoOverlayNode", "AudioResamplerNode"}) {
        auto config = Json::array();
        if (std::string_view(type) == "InputNode" || std::string_view(type) == "OutputNode")
            config = Json::array({{{"filename", "absent.mp4"}}});
        if (std::string_view(type) == "RTMPOutputNode")
            config = Json::array({{{"url", "rtmp://127.0.0.1:1935/live/test"}}});
        document = {{"graph", Json::array({node(type, "solo", config)})}};
        if (std::string_view(type) == "MuxerNode")
            rejects("unconnected mux", document, "All MuxerNode input ports must be connected");
        else accepts(std::string("node type ") + type, document);
    }
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
        {"InputNode", "DemuxNode"}, {"DemuxNode", "VideoDecoderNode"},
        {"DemuxNode", "AudioDecoderNode"}, {"VideoDecoderNode", "VideoScalerNode"},
        {"VideoScalerNode", "VideoOverlayNode"}, {"VideoOverlayNode", "VideoEncoderNode"},
        {"AudioDecoderNode", "AudioResamplerNode"}, {"AudioResamplerNode", "AudioEncoderNode"},
        {"VideoEncoderNode", "OutputNode"}, {"AudioEncoderNode", "OutputNode"},
        {"InputNode", "OutputNode"}, {"MuxerNode", "OutputNode"}
    }) {
        auto sourceConfig = Json::array();
        auto targetConfig = Json::array();
        if (from == "InputNode") sourceConfig = Json::array({{{"filename", "absent.mp4"}}});
        if (to == "OutputNode") targetConfig = Json::array({{{"filename", "absent.mp4"}}});
        if (from == "MuxerNode") {
            document = {{"graph", Json::array({node("InputNode", "in", Json::array({{{"filename", "absent.mp4"}}}), Json::array({"source"})),
                node(from, "source", Json::array({{{"ports", "1"}}}), Json::array({"target"})), node(to, "target", targetConfig)})}};
        } else document = {{"graph", Json::array({node(from, "source", sourceConfig, Json::array({"target"})), node(to, "target", targetConfig)})}};
        accepts(from + " -> " + to, document);
    }
    for (const auto& [from, to, error] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"VideoDecoderNode", "AudioEncoderNode", "Incompatible audio/video"},
        {"AudioDecoderNode", "VideoEncoderNode", "Incompatible audio/video"},
        {"VideoDecoderNode", "OutputNode", "Incompatible packet/frame"},
        {"InputNode", "VideoEncoderNode", "Incompatible packet/frame"},
        {"AudioResamplerNode", "VideoScalerNode", "Incompatible audio/video"},
    }) {
        auto sourceConfig = from == "InputNode" ? Json::array({{{"filename", "absent.mp4"}}}) : Json::array();
        auto targetConfig = to == "OutputNode" ? Json::array({{{"filename", "absent.mp4"}}}) : Json::array();
        document = {{"graph", Json::array({node(from, "source", sourceConfig, Json::array({"target"})), node(to, "target", targetConfig)})}};
        rejects(from + " -> " + to, document, error);
    }
}

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& extension = ".json") {
        static unsigned counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("async_nodes_parser_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(++counter) + extension);
    }
    ~TempFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    void write(const std::string& value) const {
        std::ofstream file(path, std::ios::binary);
        require(static_cast<bool>(file), "could not create temporary file");
        file << value;
        require(static_cast<bool>(file), "could not write temporary file");
    }
    std::string read() const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};

void parseFileAndSideEffects() {
    TempFile output(".mp4"); output.write("keep existing output\n");
    TempFile input(".mp4"); // The input deliberately does not exist.
    auto document = packetGraph();
    document["graph"][0]["configuration"][0]["filename"] = input.path.string();
    document["graph"][1]["configuration"][0]["filename"] = output.path.string();
    accepts("validation without opening input or output", document);
    require(output.read() == "keep existing output\n", "validation changed the output file");
    require(!std::filesystem::exists(input.path), "validation created the input file");
    document["graph"][0]["connections"] = Json::array({"missing"});
    rejects("invalid graph without side effects", document, "Unknown connection target");
    require(output.read() == "keep existing output\n", "rejected validation changed the output file");

    TempFile config;
    auto parseRejects = [&](const std::string& label, std::string_view expected) {
        try { (void)JsonGraphParser::parseFile(config.path); }
        catch (const std::exception& error) {
            const std::string message = error.what();
            require(message.find("Configuration " + config.path.string() + ": ") != std::string::npos,
                    label + " lost file context: " + message);
            require(message.find(expected) != std::string::npos, label + " gave wrong cause: " + message);
            return;
        }
        throw std::runtime_error(label + " unexpectedly accepted");
    };
    parseRejects("missing file", "Cannot open configuration file");
    config.write("{ broken"); parseRejects("invalid JSON", "parse error");
    config.write(Json{{"graph", Json::array()}}.dump()); parseRejects("invalid graph", "graph must be a non-empty array");
    config.write(document.dump()); parseRejects("invalid topology before node configuration", "Unknown connection target");
    require(output.read() == "keep existing output\n", "parseFile changed output before rejecting topology");
    config.write(Json{{"graph", Json::array({node("VideoScalerNode", "scale", Json::array({{{"width", "0"}}}))})}}.dump());
    parseRejects("node configuration", "Node scale: configuration failed, code=");
    config.write(Json{{"graph", Json::array({node("DemuxNode", "demux", Json::array({{{"ports", "1"}}}))})}}.dump());
    auto graph = JsonGraphParser::parseFile(config.path);
    require(graph != nullptr, "parseFile returned null graph");
    require(graph->poll().state == Graph::State::Running, "parseFile unexpectedly started or stopped graph");
    config.write(Json{{"graph", Json::array({node("RTMPOutputNode", "rtmp",
        Json::array({{{"url", "rtmp://127.0.0.1:1/live/test"}}}))})}}.dump());
    auto rtmp = JsonGraphParser::parseFile(config.path);
    require(rtmp != nullptr && rtmp->poll().state == Graph::State::Running,
            "parseFile tried to connect RTMP before graph start");
}
} // namespace

int main() {
    try {
        schemaAndConfiguration();
        capacityAndPorts();
        topologyAndTypes();
        parseFileAndSideEffects();
        std::cout << "JsonGraphParser unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
