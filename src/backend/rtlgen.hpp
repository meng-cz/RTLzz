#pragma once

#include "backend/beir.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pred::rtlgen {

struct RtlDebugDerivedNode {
    beir::NodeId id = beir::kInvalidNodeId;
    std::string name;
};

struct RtlDebugSignal {
    beir::NodeId id = beir::kInvalidNodeId;
    std::string signal_name;
    int rtl_line = 0;
    std::string port_name;
    int port_element_index = -1;
    beir::ValueType type;
    std::optional<DebugLoc> decl_loc;
    std::vector<DebugLoc> primary_locs;
    std::vector<DebugLoc> related_locs;
    std::vector<std::string> messages;
    std::vector<RtlDebugDerivedNode> derived_nodes;
    std::vector<std::string> derived_names;
};

std::string emitSystemVerilog(const beir::Program& program);
std::vector<RtlDebugSignal> collectDebugSignals(const beir::Program& program,
                                                const std::string& rtl_text);
std::string emitDebugReport(const beir::Program& program,
                            const std::string& rtl_text);

} // namespace pred::rtlgen
