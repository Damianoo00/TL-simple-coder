#pragma once

#include "core/Node.hpp"

#include <cstddef>
#include <fstream>
#include <string>

class InputNode : public Node {
public:
    InputNode() = default;
    ~InputNode() override;

    void configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    std::ifstream m_file;
    std::string m_filename;
    std::size_t m_line_number = 0;
};
