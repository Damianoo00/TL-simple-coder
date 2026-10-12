#pragma once

#include "core/Graph.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

class JsonGraphParser {
public:
    // Checks JSON and topology without configuring nodes or opening their files.
    static void validateJson(std::string_view document);
    // Builds a new, configured graph without starting its worker threads.
    // Relative file paths are resolved against the current working directory.
    static std::unique_ptr<Graph> parseFile(const std::filesystem::path& filename);
};
