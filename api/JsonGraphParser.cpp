#include "JsonGraphParser.hpp"

#include "nodes/InputNode.hpp"
#include "nodes/DemuxNode.hpp"
#include "nodes/MuxerNode.hpp"
#include "nodes/OutputNode.hpp"
#include "nodes/VideoDecoderNode.hpp"
#include "nodes/AudioDecoderNode.hpp"
#include "nodes/VideoEncoderNode.hpp"
#include "nodes/AudioEncoderNode.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct Connection {
    std::string target;
    std::size_t output_port;
    std::size_t input_port;
};

struct NodeDefinition {
    std::string type;
    std::string name;
    std::unordered_map<std::string, std::string> configuration;
    std::vector<Connection> connections;
};

std::string requiredString(const nlohmann::json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string(key) + " must be a non-empty string");
    }
    return value.get<std::string>();
}

std::vector<NodeDefinition> readDefinitions(const nlohmann::json& document) {
    const auto& entries = document.at("graph");
    if (!entries.is_array() || entries.empty()) {
        throw std::runtime_error("graph must be a non-empty array");
    }

    std::vector<NodeDefinition> definitions;
    std::unordered_map<std::string, std::size_t> names;
    for (const auto& entry : entries) {
        NodeDefinition definition;
        definition.name = requiredString(entry, "name");
        definition.type = requiredString(entry, "type");
        if (definition.type != "InputNode" && definition.type != "OutputNode" &&
            definition.type != "DemuxNode" && definition.type != "MuxerNode" &&
            definition.type != "VideoDecoderNode" && definition.type != "AudioDecoderNode" &&
            definition.type != "VideoEncoderNode" && definition.type != "AudioEncoderNode") {
            throw std::runtime_error("Unknown node type: " + definition.type);
        }
        if (!names.emplace(definition.name, definitions.size()).second) {
            throw std::runtime_error("Duplicate node name: " + definition.name);
        }

        const auto& configuration = entry.at("configuration");
        if (!configuration.is_array()) {
            throw std::runtime_error("configuration must be an array for node " + definition.name);
        }
        for (const auto& properties : configuration) {
            if (!properties.is_object()) {
                throw std::runtime_error("configuration entries must be objects for node " + definition.name);
            }
            for (const auto& [key, value] : properties.items()) {
                if (!value.is_string()) {
                    throw std::runtime_error("Configuration values must be strings for node " + definition.name);
                }
                if (!definition.configuration.emplace(key, value.get<std::string>()).second) {
                    throw std::runtime_error("Duplicate configuration key " + key + " for node " + definition.name);
                }
            }
        }
        if (definition.type == "InputNode" || definition.type == "OutputNode") {
            const auto file = definition.configuration.find("filename");
            if (file == definition.configuration.end() || file->second.empty()) {
                throw std::runtime_error("Missing non-empty filename for node " + definition.name);
            }
        }

        const auto& connections = entry.at("connections");
        if (!connections.is_array())
            throw std::runtime_error("connections must be an array for node " + definition.name);
        for (const auto& target : connections) {
            Connection connection;
            connection.output_port = definition.connections.size();
            connection.input_port = 0;
            if (target.is_string()) {
                connection.target = target.get<std::string>();
                if (connection.target.empty()) throw std::runtime_error("Empty connection target");
            } else if (target.is_object()) {
                connection.target = requiredString(target, "node");
                auto port = [&target](const char* key, std::size_t fallback) {
                    if (!target.contains(key)) return fallback;
                    const auto& value = target.at(key);
                    if (!value.is_number_integer() ||
                        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
                        throw std::runtime_error(std::string(key) + " must be a non-negative integer");
                    return value.get<std::size_t>();
                };
                connection.output_port = port("output_port", connection.output_port);
                connection.input_port = port("input_port", 0);
            } else throw std::runtime_error("Connection must be a node name or a port mapping");
            definition.connections.push_back(std::move(connection));
        }
        definitions.push_back(std::move(definition));
    }

    // Validate references, port counts and collisions before opening any files.
    auto inputCount = [](const NodeDefinition& node) -> std::size_t {
        if (node.type == "InputNode") return 0;
        if (node.type == "MuxerNode") return mediaPortCount(node.configuration);
        return 1;
    };
    auto outputCount = [](const NodeDefinition& node) -> std::size_t {
        if (node.type == "OutputNode") return 0;
        if (node.type == "DemuxNode") return mediaPortCount(node.configuration);
        return 1;
    };
    std::vector<std::unordered_map<std::size_t, bool>> incoming(definitions.size());
    for (const auto& definition : definitions) {
        // Validate ports configuration even for an unconnected node.
        const auto outputs = outputCount(definition);
        inputCount(definition);
        std::unordered_map<std::size_t, bool> used_outputs;
        for (const auto& connection : definition.connections) {
            const auto target = names.find(connection.target);
            if (target == names.end())
                throw std::runtime_error("Unknown connection target " + connection.target);
            if (connection.output_port >= outputs)
                throw std::runtime_error("Invalid output port for node " + definition.name);
            if (connection.input_port >= inputCount(definitions[target->second]))
                throw std::runtime_error("Invalid input port for node " + connection.target);
            const auto& target_type = definitions[target->second].type;
            const bool sends_frames = definition.type == "VideoDecoderNode" || definition.type == "AudioDecoderNode";
            const bool takes_frames = target_type == "VideoEncoderNode" || target_type == "AudioEncoderNode";
            if (sends_frames != takes_frames)
                throw std::runtime_error("Incompatible packet/frame connection from " + definition.name + " to " + connection.target);
            if (sends_frames && ((definition.type == "VideoDecoderNode") != (target_type == "VideoEncoderNode")))
                throw std::runtime_error("Incompatible audio/video connection from " + definition.name + " to " + connection.target);
            if (!used_outputs.emplace(connection.output_port, true).second)
                throw std::runtime_error("Output port is connected more than once");
            if (!incoming[target->second].emplace(connection.input_port, true).second)
                throw std::runtime_error("Input port is connected more than once");
        }
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (definitions[i].type == "MuxerNode" && incoming[i].size() != inputCount(definitions[i]))
            throw std::runtime_error("All MuxerNode input ports must be connected");
    }
    return definitions;
}
} // namespace

void JsonGraphParser::validateJson(std::string_view document) {
    readDefinitions(nlohmann::json::parse(document));
}

std::unique_ptr<Graph> JsonGraphParser::parseFile(const std::filesystem::path& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open configuration file");
        }
        const auto definitions = readDefinitions(nlohmann::json::parse(file));
        auto graph = std::make_unique<Graph>();
        std::unordered_map<std::string, NodeBase*> nodes;
        for (const auto& definition : definitions) {
            NodeBase* node;
            if (definition.type == "InputNode") {
                node = &graph->addNode<InputNode>();
            } else if (definition.type == "OutputNode") {
                node = &graph->addNode<OutputNode>();
            } else if (definition.type == "DemuxNode") {
                node = &graph->addNode<DemuxNode>();
            } else if (definition.type == "MuxerNode") {
                node = &graph->addNode<MuxerNode>();
            } else if (definition.type == "VideoDecoderNode") {
                node = &graph->addNode<VideoDecoderNode>();
            } else if (definition.type == "AudioDecoderNode") {
                node = &graph->addNode<AudioDecoderNode>();
            } else if (definition.type == "VideoEncoderNode") {
                node = &graph->addNode<VideoEncoderNode>();
            } else {
                node = &graph->addNode<AudioEncoderNode>();
            }
            try {
                const int result = node->configure(definition.configuration);
                if (result != NodeBase::Success)
                    throw std::runtime_error("configuration failed, code=" + std::to_string(result));
            } catch (const std::exception& error) {
                throw std::runtime_error("Node " + definition.name + ": " + error.what());
            }
            nodes.emplace(definition.name, node);
        }
        for (const auto& definition : definitions) {
            for (const auto& connection : definition.connections) {
                graph->connect(*nodes.at(definition.name), *nodes.at(connection.target),
                               connection.output_port, connection.input_port);
            }
        }
        return graph;
    } catch (const std::exception& error) {
        throw std::runtime_error("Configuration " + filename.string() + ": " + error.what());
    }
}
