#include "backend/pipeline.hpp"
#include "backend/rtl_names.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace pred::pipeline {
namespace {
using namespace beir;
using Key = rtlzz::PipelineOperation;

Key key(const Operation& op) {
    if (op.kind == OperationKind::Binary || op.kind == OperationKind::Unary) switch (op.op) {
    case OpCode::Add: return Key::Add;
    case OpCode::Sub: return Key::Subtract;
    case OpCode::Mul: return Key::Multiply;
    case OpCode::Div: return Key::Divide;
    case OpCode::Mod: return Key::Modulo;
    case OpCode::BitAnd: return Key::BitAnd;
    case OpCode::BitOr: return Key::BitOr;
    case OpCode::BitXor: return Key::BitXor;
    case OpCode::LogicAnd: return Key::LogicalAnd;
    case OpCode::LogicOr: return Key::LogicalOr;
    case OpCode::Eq: return Key::Equal;
    case OpCode::Ne: return Key::NotEqual;
    case OpCode::Lt: return Key::Less;
    case OpCode::Le: return Key::LessEqual;
    case OpCode::Gt: return Key::Greater;
    case OpCode::Ge: return Key::GreaterEqual;
    case OpCode::Shl: return Key::ShiftLeft;
    case OpCode::Shr: return Key::ShiftRight;
    case OpCode::LogicNot: return Key::LogicalNot;
    case OpCode::BitNot: return Key::BitNot;
    case OpCode::Neg: return Key::Negate;
    case OpCode::None: break;
    }
    switch (op.kind) {
    case OperationKind::Assign: return Key::Assign;
    case OperationKind::AddCarry: return Key::AddCarry;
    case OperationKind::Ite: return Key::Mux;
    case OperationKind::Case: return Key::Case;
    case OperationKind::Lookup: return Key::Lookup;
    case OperationKind::ArrayAccess: return Key::ArrayAccess;
    case OperationKind::Cast: return Key::Cast;
    case OperationKind::ZExt: return Key::ZeroExtend;
    case OperationKind::SExt: return Key::SignExtend;
    case OperationKind::Trunc: return Key::Truncate;
    case OperationKind::Slice: return Key::Slice;
    case OperationKind::BitSelect: return Key::BitSelect;
    case OperationKind::WriteSlice: return Key::WriteSlice;
    case OperationKind::WriteBit: return Key::WriteBit;
    case OperationKind::Concat: return Key::Concat;
    case OperationKind::Repeat: return Key::Repeat;
    case OperationKind::ReduceOr: return Key::ReduceOr;
    case OperationKind::ReduceAnd: return Key::ReduceAnd;
    case OperationKind::ReduceXor: return Key::ReduceXor;
    case OperationKind::DynamicBitSelect: return Key::DynamicBitSelect;
    case OperationKind::DynamicSlice: return Key::DynamicSlice;
    case OperationKind::DynamicWriteSlice: return Key::DynamicWriteSlice;
    case OperationKind::DynamicWriteBit: return Key::DynamicWriteBit;
    case OperationKind::Aggregate: return Key::Aggregate;
    case OperationKind::Call: return Key::Call;
    default: throw std::runtime_error("pipeline: unsupported operation cost key");
    }
}

std::uint64_t bits(const ValueType& type) {
    if (type.width <= 0) throw std::runtime_error("pipeline: invalid signal width");
    auto result = static_cast<std::uint64_t>(type.width);
    for (int dim : type.array_dims) {
        if (dim <= 0 || result > UINT64_MAX / static_cast<unsigned>(dim))
            throw std::runtime_error("pipeline: register size overflow");
        result *= static_cast<unsigned>(dim);
    }
    return result;
}

struct Graph {
    const beir::Program& p;
    int last;
    std::vector<NodeId> order;
    std::vector<std::vector<NodeId>> users;
    std::vector<bool> live, input, output, constant;
    std::vector<double> costs;

    Graph(const beir::Program& program, const Options& options)
        : p(program), last(options.register_stages), users(p.signals.size()),
          live(p.signals.size()), input(live.size()), output(live.size()),
          constant(live.size()), costs(live.size()) {
        std::vector<NodeId> pending;
        for (const auto& port : p.ports) {
            for (auto id : port.element_nodes) {
                p.signal(id);
                if (port.direction == PortDirection::Input) input[id] = true;
                if (port.direction == PortDirection::Output) output[id] = true;
                pending.push_back(id);
            }
        }
        while (!pending.empty()) {
            auto id = pending.back(); pending.pop_back();
            const auto& s = p.signal(id);
            if (live[id]) continue;
            live[id] = true;
            if (s.driver) for (const auto& v : s.driver->operands)
                if (v.kind == OperandKind::Symbol) pending.push_back(v.node);
        }
        std::vector<std::size_t> indegree(live.size());
        std::size_t count = 0;
        for (const auto& s : p.signals) {
            if (!live.at(s.id)) continue;
            ++count;
            if (!s.driver) {
                if (!input[s.id]) throw std::runtime_error("pipeline: undriven live signal " + s.name);
                continue;
            }
            if (s.driver->kind == OperationKind::PortRead) input[s.id] = true;
            for (const auto& v : s.driver->operands) {
                if (v.kind == OperandKind::Symbol) {
                    users[v.node].push_back(s.id);
                    ++indegree[s.id];
                } else if (v.kind == OperandKind::Port && !input[s.id]) {
                    throw std::runtime_error("pipeline: raw port dependency outside PortRead");
                }
            }
        }
        std::priority_queue<NodeId, std::vector<NodeId>, std::greater<NodeId>> ready;
        for (const auto& s : p.signals) if (live[s.id] && !indegree[s.id]) ready.push(s.id);
        while (!ready.empty()) {
            auto id = ready.top(); ready.pop(); order.push_back(id);
            for (auto user : users[id]) if (!--indegree[user]) ready.push(user);
        }
        if (order.size() != count) throw std::runtime_error("pipeline: cyclic dependency graph");
        for (auto id : order) {
            const auto& s = p.signal(id);
            if (input[id]) continue; // External Q and PortRead are wiring.
            const auto& op = *s.driver;
            constant[id] = op.kind != OperationKind::Call;
            for (const auto& v : op.operands)
                if (v.kind != OperandKind::Literal &&
                    (v.kind != OperandKind::Symbol || !constant[v.node])) constant[id] = false;
            if (constant[id]) continue;
            auto k = key(op);
            auto it = options.operation_delay_weights.find(k);
            costs[id] = it != options.operation_delay_weights.end() ? it->second
                : k == Key::Assign ? 0 : k == Key::Multiply ? 3 : 1;
        }
    }

    // Earliest feasible placement under a cost cap, with output sinks fixed at N.
    bool schedule(double cap, std::vector<int>& stage) const {
        stage.assign(live.size(), 0);
        std::vector<double> arrival(live.size());
        for (auto id : order) {
            if (input[id] || constant[id]) continue;
            const auto& op = *p.signal(id).driver;
            int s = 0;
            for (const auto& v : op.operands) if (v.kind == OperandKind::Symbol && !constant[v.node])
                s = std::max(s, stage[v.node]);
            if (output[id]) s = last;
            double a = 0;
            for (const auto& v : op.operands) if (v.kind == OperandKind::Symbol && !constant[v.node] && stage[v.node] == s)
                a = std::max(a, arrival[v.node]);
            if (a + costs[id] > cap) { ++s; a = 0; }
            if (s > last || costs[id] > cap) return false;
            stage[id] = s; arrival[id] = a + costs[id];
        }
        // Constants have no transaction state; output wiring still belongs to N.
        for (auto id : order) if (output[id] && constant[id]) stage[id] = last;
        return true;
    }

    Report evaluate(const std::vector<int>& stage) const {
        Report r; r.stage_delays.assign(static_cast<std::size_t>(last) + 1, 0);
        std::vector<double> arrival(live.size());
        for (auto id : order) {
            if (stage[id] < 0 || stage[id] > last || (input[id] && stage[id] != 0) ||
                (output[id] && stage[id] != last)) {
                r.final_delay = std::numeric_limits<double>::infinity(); return r;
            }
            const auto& s = p.signal(id);
            if (constant[id] || input[id]) continue;
            double a = 0;
            for (const auto& v : s.driver->operands) {
                if (v.kind != OperandKind::Symbol || constant[v.node]) continue;
                if (stage[v.node] > stage[id]) {
                    r.final_delay = std::numeric_limits<double>::infinity(); return r;
                }
                if (stage[v.node] == stage[id]) a = std::max(a, arrival[v.node]);
            }
            arrival[id] = a + costs[id];
            r.stage_delays[stage[id]] = std::max(r.stage_delays[stage[id]], arrival[id]);
        }
        for (auto id : order) {
            if (constant[id]) continue;
            int end = stage[id];
            for (auto user : users[id]) end = std::max(end, stage[user]);
            auto width = bits(p.signal(id).type);
            auto length = static_cast<std::uint64_t>(end - stage[id]);
            if (length && width > (UINT64_MAX - r.register_bits) / length)
                throw std::runtime_error("pipeline: register bit count overflow");
            r.register_bits += width * length;
            r.register_count += static_cast<std::size_t>(length);
        }
        r.final_delay = *std::max_element(r.stage_delays.begin(), r.stage_delays.end());
        return r;
    }
};
} // namespace

void validateOptions(const Options& o) {
    if (o.register_stages < 0 || o.register_stages == std::numeric_limits<int>::max())
        throw std::runtime_error("pipeline: register_stages must be non-negative and less than INT_MAX");
    if (!std::isfinite(o.timing_relaxation_ratio) || o.timing_relaxation_ratio < 0)
        throw std::runtime_error("pipeline: timing_relaxation_ratio must be finite and non-negative");
    for (const auto& [k, v] : o.operation_delay_weights) {
        if (k < Key::Assign || k > Key::Call || !std::isfinite(v) || v < 0)
            throw std::runtime_error("pipeline: invalid operation delay weight");
    }
    if (o.reset_enabled) {
        if (o.reset_name.empty() || !(std::isalpha(static_cast<unsigned char>(o.reset_name[0])) || o.reset_name[0] == '_') ||
            o.reset_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos || rtl_names::isSystemVerilogKeyword(o.reset_name))
            throw std::runtime_error("pipeline: reset_name must be a non-reserved RTL identifier");
        if (o.register_stages && o.reset_name == o.clock_name) throw std::runtime_error("pipeline: clock and reset names must differ");
    } else if (!o.input_reset_values.empty()) throw std::runtime_error("pipeline: input_reset_values requires reset_enabled");
    if (o.register_stages) {
        if (o.clock_name.empty() || !(std::isalpha(static_cast<unsigned char>(o.clock_name[0])) || o.clock_name[0] == '_') ||
            o.clock_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::runtime_error("pipeline: clock_name must be an RTL identifier");
        if (rtl_names::isSystemVerilogKeyword(o.clock_name))
            throw std::runtime_error("pipeline: clock_name is a reserved RTL identifier");
    }
}

Program build(const beir::Program& program, const Options& options) {
    validateOptions(options);
    Program result;
    result.options = options;
    result.combinational = program;
    result.rtl_program = program;
    if (!options.register_stages) {
        if (options.reset_enabled) {
            Graph graph(program, options);
            result.stages.assign(program.signals.size(), 0);
            result.report = graph.evaluate(result.stages);
            result.report.baseline_delay = result.report.final_delay;
            result.report.allowed_delay = result.report.final_delay * (1 + options.timing_relaxation_ratio);
        }
        initializeResetValues(result);
        return result;
    }
    Graph g(program, options);
    double upper = 0, lower = 0;
    for (auto id : g.order) {
        upper += g.costs[id]; lower = std::max(lower, g.costs[id]);
    }
    if (!std::isfinite(upper)) throw std::runtime_error("pipeline: delay sum overflow");
    std::vector<int> baseline;
    if (!g.schedule(upper, baseline)) throw std::runtime_error("pipeline: no feasible schedule");
    // Monotonic earliest-stage feasibility; keep the feasible upper endpoint.
    for (int i = 0; i < 64 && lower < upper; ++i) {
        double middle = lower + (upper - lower) / 2;
        if (middle == lower || middle == upper) break;
        std::vector<int> candidate;
        if (g.schedule(middle, candidate)) { upper = middle; baseline = std::move(candidate); }
        else lower = middle;
    }
    result.stages = baseline;
    auto before = g.evaluate(baseline);
    const double allowed = before.final_delay * (1 + options.timing_relaxation_ratio);
    if (!std::isfinite(allowed)) throw std::runtime_error("pipeline: timing allowance overflow");
    auto current = before;
    for (;;) {
        auto best = current;
        NodeId best_node = kInvalidNodeId;
        int best_stage = 0;
        for (auto id : g.order) {
            if (g.input[id] || g.output[id] || g.constant[id]) continue;
            int original = result.stages[id];
            for (int stage = 0; stage <= g.last; ++stage) {
                if (stage == original) continue;
                result.stages[id] = stage;
                auto candidate = g.evaluate(result.stages);
                if (candidate.final_delay <= allowed && candidate.register_bits < current.register_bits &&
                    (candidate.register_bits < best.register_bits ||
                     (candidate.register_bits == best.register_bits && candidate.final_delay < best.final_delay))) {
                    best = std::move(candidate); best_node = id; best_stage = stage;
                }
            }
            result.stages[id] = original;
        }
        if (best_node == kInvalidNodeId) break;
        result.stages[best_node] = best_stage; current = std::move(best);
    }
    result.report = current;
    result.report.baseline_delay = before.final_delay;
    result.report.allowed_delay = allowed;
    result.report.baseline_register_bits = before.register_bits;

    auto& rtl = result.rtl_program;
    std::vector<std::vector<NodeId>> delayed(program.signals.size());
    std::unordered_set<std::string> names;
    for (const auto& port : rtl.ports) names.insert(rtl_names::sanitizeIdentifier(port.name));
    for (const auto& s : rtl.signals) names.insert(rtl_names::sanitizeIdentifier(s.name));
    if (!names.insert(options.clock_name).second)
        throw std::runtime_error("pipeline: clock name collides with data port or signal: " + options.clock_name);
    // Existing emitter sanitization must not silently rename a requested clock.
    // The emitter performs its full reserved-keyword and sanitized-name check.
    for (auto id : g.order) {
        delayed[id].push_back(id);
        if (g.constant[id]) continue;
        int end = result.stages[id];
        for (auto user : g.users[id]) end = std::max(end, result.stages[user]);
        for (int stage = result.stages[id] + 1; stage <= end; ++stage) {
            Signal reg;
            reg.id = rtl.signals.size();
            reg.name = "rtlzz_pipe_" + std::to_string(id) + "_s" + std::to_string(stage);
            while (!names.insert(reg.name).second) reg.name += "_";
            reg.type = program.signal(id).type;
            reg.debug = program.signal(id).debug;
            if (program.signal(id).driver) addDebugInfoLocs(reg.debug, program.signal(id).driver->debug);
            addDebugMessage(reg.debug, "pipeline register for node " + std::to_string(id) + " at stage " + std::to_string(stage));
            addDebugDerivedOperand(reg.debug, Operand{OperandKind::Symbol, id});
            Operation d; d.kind = OperationKind::Assign; d.type = reg.type; d.debug = reg.debug;
            Operand v; v.kind = OperandKind::Symbol; v.node = delayed[id].back(); v.type = reg.type;
            d.operands.push_back(v); reg.driver = std::move(d);
            result.registers.push_back({reg.id, v.node, stage});
            delayed[id].push_back(reg.id);
            rtl.signals.push_back(std::move(reg));
        }
    }
    for (auto id : g.order) {
        auto& s = rtl.signal(id);
        if (!s.driver) continue;
        for (auto& v : s.driver->operands) {
            if (v.kind != OperandKind::Symbol || g.constant[v.node]) continue;
            auto offset = result.stages[id] - result.stages[v.node];
            if (offset < 0) throw std::runtime_error("pipeline: backwards stage dependency");
            v.node = delayed[v.node].at(static_cast<std::size_t>(offset));
        }
        addDebugMessage(s.debug, "pipeline computation stage " + std::to_string(result.stages[id]));
    }
    // Drop dead computations, preserving dense IDs and debug identities.
    for (const auto& original : program.signals)
        if (!g.live[original.id]) rtl.signal(original.id).driver.reset();
    initializeResetValues(result);
    if (options.reset_enabled) {
        if (!names.insert(options.reset_name).second) throw std::runtime_error("pipeline: reset name collides with data port, signal or clock");
        Port reset; reset.name = options.reset_name; reset.direction = PortDirection::Input; reset.type.width = 1;
        rtl.ports.push_back(reset); rtl.inputs.push_back(reset.name);
    }
    Port clock; clock.name = options.clock_name; clock.direction = PortDirection::Input; clock.type.width = 1;
    rtl.ports.push_back(clock);
    rtl.inputs.push_back(clock.name);
    if (result.registers.size() != result.report.register_count)
        throw std::runtime_error("pipeline: register accounting mismatch");
    validateProgram(result);
    return result;
}

void validateProgram(const Program& p) {
    validateOptions(p.options);
    if (!p.options.register_stages) return;
    Graph g(p.combinational, p.options);
    if (p.stages.size() != p.combinational.signals.size() ||
        p.rtl_program.signals.size() != p.combinational.signals.size() + p.registers.size())
        throw std::runtime_error("pipeline: invalid materialized signal count");
    auto expected = g.evaluate(p.stages);
    if (!std::isfinite(expected.final_delay) || !std::isfinite(p.report.allowed_delay) ||
        expected.final_delay != p.report.final_delay || expected.stage_delays != p.report.stage_delays ||
        expected.final_delay > p.report.allowed_delay ||
        expected.register_bits != p.report.register_bits || p.registers.size() != expected.register_count)
        throw std::runtime_error("pipeline: invalid schedule or register report");
    auto stages = p.stages;
    stages.resize(p.rtl_program.signals.size(), -1);
    std::vector<NodeId> origins(stages.size(), kInvalidNodeId);
    for (auto id : g.order) origins[id] = id;
    std::uint64_t storage = 0;
    for (const auto& reg : p.registers) {
        const auto& q = p.rtl_program.signal(reg.node);
        const auto& d = p.rtl_program.signal(reg.source);
        if (reg.node < p.combinational.signals.size() || stages[reg.node] != -1 ||
            origins[reg.source] == kInvalidNodeId || reg.stage != stages[reg.source] + 1 ||
            reg.stage > g.last || !sameType(q.type, d.type) || !q.driver ||
            q.driver->kind != OperationKind::Assign || q.driver->operands.size() != 1 ||
            q.driver->operands[0].kind != OperandKind::Symbol || q.driver->operands[0].node != reg.source)
            throw std::runtime_error("pipeline: invalid register connection");
        stages[reg.node] = reg.stage; origins[reg.node] = origins[reg.source];
        auto width = bits(q.type);
        if (storage > UINT64_MAX - width) throw std::runtime_error("pipeline: register storage overflow");
        storage += width;
    }
    if (storage != expected.register_bits) throw std::runtime_error("pipeline: register storage mismatch");
    for (auto id : g.order) {
        const auto& before = p.combinational.signal(id);
        const auto& after = p.rtl_program.signal(id);
        if (!sameType(before.type, after.type)) throw std::runtime_error("pipeline: computation type changed");
        if (!before.driver) continue;
        if (!after.driver || before.driver->operands.size() != after.driver->operands.size())
            throw std::runtime_error("pipeline: computation driver changed");
        for (std::size_t i = 0; i < before.driver->operands.size(); ++i) {
            const auto& v = before.driver->operands[i];
            const auto& adjusted = after.driver->operands[i];
            if (v.kind != OperandKind::Symbol || g.constant[v.node]) continue;
            p.rtl_program.signal(adjusted.node);
            if (adjusted.kind != OperandKind::Symbol || origins[adjusted.node] != v.node ||
                stages[adjusted.node] != p.stages[id] || adjusted.signed_view != v.signed_view ||
                !sameType(adjusted.type, v.type))
                throw std::runtime_error("pipeline: unaligned computation dependency");
        }
    }
}

std::string emitReport(const Program& p) {
    std::ostringstream os;
    os.precision(17);
    os << "pipeline internal register stages " << p.options.register_stages << "\n"
       << "baseline delay " << p.report.baseline_delay << "\n"
       << "allowed delay " << p.report.allowed_delay << "\n"
       << "final delay " << p.report.final_delay << "\n"
       << "baseline register bits " << p.report.baseline_register_bits << "\n"
       << "register bits " << p.report.register_bits << "\n"
       << "register count " << p.report.register_count << "\n";
    for (std::size_t i = 0; i < p.report.stage_delays.size(); ++i)
        os << "stage " << i << " delay " << p.report.stage_delays[i] << "\n";
    for (const auto &[name, values] : p.report.output_reset_values) {
        os << "output reset " << name << " [";
        for (std::size_t i=0; i<values.size(); ++i) os << (i ? "," : "") << values[i];
        os << "]\n";
    }
    return os.str();
}

std::string emitText(const Program& p) {
    std::ostringstream os;
    os << emitReport(p);
    for (std::size_t id = 0; id < p.stages.size(); ++id)
        os << "node #" << id << " stage " << p.stages[id] << "\n";
    for (const auto& r : p.registers)
        os << "register #" << r.node << " <- #" << r.source << " stage " << r.stage << "\n";
    os << beir::emitText(p.rtl_program);
    return os.str();
}
} // namespace pred::pipeline
