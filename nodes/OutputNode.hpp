#pragma once

#include "core/Node.hpp"

#include <fstream>
#include <string>

class OutputNode : public Node {
public:
    OutputNode() = default;
    ~OutputNode() override;

    void configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    std::ofstream m_file;
    std::string m_filename;
};
