#pragma once

#include "Graph.hpp"

#include <filesystem>
#include <memory>

class JsonGraphParser {
public:
    // Builds a new, configured graph without starting its worker threads.
    // Relative file paths are resolved against the current working directory.
    static std::unique_ptr<Graph> parseFile(const std::filesystem::path& filename);
};
