#pragma once

#include "core/Node.hpp"
#include "FFmpeg.hpp"

#include <unordered_map>

class DemuxNode : public NodeBase {
public:
    explicit DemuxNode(std::size_t ports = 2);
    ~DemuxNode() override { stop(); }
    int configure(std::unordered_map<std::string, std::string> config) override;
    int process() override;

private:
    const char* nodeType() const override { return "DemuxNode"; }
    struct Route { std::size_t port; bool ended = false; };
    std::unordered_map<int, Route> m_routes;
    std::size_t m_ports;
    std::size_t m_ended = 0;
    bool m_configs_complete = false;
    void finishConfigs();
};
