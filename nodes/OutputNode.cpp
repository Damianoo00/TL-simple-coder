#include "OutputNode.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

void OutputNode::configure(std::unordered_map<std::string, std::string> config) {
    const std::string& filename = config.at("filename");
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open output file: " + filename);
    }
    m_file = std::move(file);
    m_filename = filename;
    std::printf("OutputNode: writing %s\n",
                std::filesystem::absolute(filename).string().c_str());
}

OutputNode::~OutputNode() {
    // Join before destroying the file accessed by the worker thread.
    stop();
}

int OutputNode::process() {
    auto value = m_input.at(0)->pop();
    if (!value) {
        return -1;
    }

    m_file << *value << '\n';
    m_file.flush();
    if (!m_file) {
        std::fprintf(stderr, "OutputNode: write error in %s\n", m_filename.c_str());
        return -1;
    }
    return 0;
}
