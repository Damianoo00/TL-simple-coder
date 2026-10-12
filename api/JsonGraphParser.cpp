#include "JsonGraphParser.hpp"

#include "nodes/InputNode.hpp"
#include "nodes/OutputNode.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
struct NodeDefinition {
    std::string type;
    std::string name;
    std::unordered_map<std::string, std::string> configuration;
    std::vector<std::string> connections;
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
        if (definition.type != "Node" && definition.type != "InputNode" &&
            definition.type != "OutputNode") {
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
        if (!connections.is_array() || connections.size() > 1) {
            throw std::runtime_error("connections must be an array with at most one target for node " + definition.name);
        }
        for (const auto& target : connections) {
            if (!target.is_string() || target.get_ref<const std::string&>().empty()) {
                throw std::runtime_error("Connection targets must be non-empty strings for node " + definition.name);
            }
            definition.connections.push_back(target.get<std::string>());
        }
        definitions.push_back(std::move(definition));
    }

    // Validate all references before configure() can open or truncate files.
    for (const auto& definition : definitions) {
        for (const auto& target : definition.connections) {
            if (!names.contains(target)) {
                throw std::runtime_error("Unknown connection target " + target + " for node " + definition.name);
            }
        }
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
        std::unordered_map<std::string, Node*> nodes;
        for (const auto& definition : definitions) {
            Node* node;
            if (definition.type == "InputNode") {
                node = &graph->addNode<InputNode>();
            } else if (definition.type == "OutputNode") {
                node = &graph->addNode<OutputNode>();
            } else {
                node = &graph->addNode();
            }
            try {
                node->configure(definition.configuration);
            } catch (const std::exception& error) {
                throw std::runtime_error("Node " + definition.name + ": " + error.what());
            }
            nodes.emplace(definition.name, node);
        }
        for (const auto& definition : definitions) {
            for (const auto& target : definition.connections) {
                graph->connect(*nodes.at(definition.name), *nodes.at(target));
            }
        }
        return graph;
    } catch (const std::exception& error) {
        throw std::runtime_error("Configuration " + filename.string() + ": " + error.what());
    }
}
