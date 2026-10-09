#include "InputNode.hpp"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <stdexcept>

void InputNode::configure(std::unordered_map<std::string, std::string> config) {
    const std::string& filename = config.at("filename");
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open input file: " + filename);
    }
    m_file = std::move(file);
    m_filename = filename;
    m_line_number = 0;
    std::printf("InputNode: reading %s\n",
                std::filesystem::absolute(filename).string().c_str());
}

InputNode::~InputNode() {
    // Join before destroying the file accessed by the worker thread.
    stop();
}

int InputNode::process() {
    if (m_stop_requested.load()) {
        return -1;
    }

    std::string line;
    if (!std::getline(m_file, line)) {
        if (m_file.bad()) {
            std::fprintf(stderr, "InputNode: read error in %s\n", m_filename.c_str());
        }
        return -1;
    }
    ++m_line_number;

    std::istringstream parser(line);
    int value;
    if (!(parser >> value) || !(parser >> std::ws).eof()) {
        std::fprintf(stderr, "InputNode: invalid integer in %s at line %zu\n",
                     m_filename.c_str(), m_line_number);
        return -1;
    }

    if (m_output.at(0)) {
        return m_output.at(0)->push(value) ? 0 : -1;
    }
    std::printf("End of graph: %d\n", value);
    return 0;
}
