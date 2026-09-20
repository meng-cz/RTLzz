#include "backend/beopt.hpp"
#include "backend/beopt_structure.hpp"
#include <cstdlib>
#include <iostream>
#include <map>
#include <functional>

using namespace pred::beir;
using namespace pred::beir::opt;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while (0)

static Operand literal(uint64_t value, int width = 8) {
    Operand o; o.kind = OperandKind::Literal; o.type = {width, {}};
    o.constant.width = width; o.constant.limbs = {value}; return o;
}
static Operand node(Program& p, std::string name, int width, std::optional<Operation> op = {}) {
    Signal s; s.id = p.signals.size(); s.name = name; s.type = {width, {}}; s.driver = op;
    p.signals.push_back(s);
    Operand o; o.kind = OperandKind::Symbol; o.node = s.id; o.text = name; o.type = s.type; return o;
}
static Operand operation(Program& p, OperationKind kind, OpCode opcode,
                         std::vector<Operand> args, int width = 8) {
    Operation op; op.kind = kind; op.op = opcode; op.operands = std::move(args); op.type = {width, {}};
    return node(p, "n" + std::to_string(p.signals.size()), width, op);
}
static Operand binary(Program& p, OpCode code, Operand a, Operand b, int width = 8) {
    return operation(p, OperationKind::Binary, code, {a, b}, width);
}
static Operand mux(Program& p, Operand c, Operand a, Operand b) {
    return operation(p, OperationKind::Ite, OpCode::None, {c, a, b});
}
static void output(Program& p, Operand o) { p.outputs.push_back(p.signal(o.node).name); }

// Independent small unsigned BEIR evaluator; all tests use widths <= 8.
static uint64_t eval(const Program& p, std::string output, const std::map<std::string, uint64_t>& input) {
    std::map<NodeId, uint64_t> cache;
    std::function<uint64_t(const Operand&)> operand;
    std::function<uint64_t(NodeId)> value = [&](NodeId id) -> uint64_t {
        if (cache.count(id)) return cache[id];
        const auto& s = p.signal(id);
        uint64_t result = 0;
        if (!s.driver) result = input.at(s.name);
        else {
            const auto& op = *s.driver;
            auto a = [&](int i) { return operand(op.operands.at(i)); };
            switch (op.kind) {
            case OperationKind::Assign: case OperationKind::ZExt: case OperationKind::Trunc:
            case OperationKind::Cast: result = a(0); break;
            case OperationKind::Ite: result = a(0) ? a(1) : a(2); break;
            case OperationKind::Unary: result = op.op == OpCode::LogicNot ? !a(0) : ~a(0); break;
            case OperationKind::Repeat:
                for (int i = 0; i < op.times; ++i) result = (result << op.operands[0].type.width) | a(0);
                break;
            case OperationKind::Slice: result = a(0) >> op.lo; break;
            case OperationKind::BitSelect: result = (a(0) >> op.bit) & 1; break;
            case OperationKind::Concat:
                for (std::size_t i = 0; i < op.operands.size(); ++i)
                    result = (result << op.operands[i].type.width) | a(i);
                break;
            case OperationKind::Binary:
                switch (op.op) {
                case OpCode::BitAnd: result = a(0) & a(1); break;
                case OpCode::BitOr: result = a(0) | a(1); break;
                case OpCode::BitXor: result = a(0) ^ a(1); break;
                case OpCode::LogicAnd: result = a(0) && a(1); break;
                case OpCode::LogicOr: result = a(0) || a(1); break;
                case OpCode::Eq: result = a(0) == a(1); break;
                case OpCode::Add: result = a(0) + a(1); break;
                case OpCode::Mul: result = a(0) * a(1); break;
                default: CHECK(false);
                } break;
            default: CHECK(false);
            }
        }
        return cache[id] = result & ((uint64_t{1} << s.type.width) - 1);
    };
    operand = [&](const Operand& o) {
        return o.kind == OperandKind::Literal ? o.constant.toU64() : value(o.node);
    };
    for (const auto& s : p.signals) if (s.name == output) return value(s.id);
    CHECK(false); return 0;
}
static int depth(const Program& p, NodeId root) {
    std::map<NodeId, int> cache;
    std::function<int(NodeId)> visit = [&](NodeId id) {
        if (cache.count(id)) return cache[id];
        const auto& s = p.signal(id); if (!s.driver) return 0;
        int result = 0;
        for (const auto& o : s.driver->operands)
            if (o.kind == OperandKind::Symbol) result = std::max(result, visit(o.node));
        return cache[id] = result + (s.driver->kind == OperationKind::Assign ? 0 :
                                     s.driver->op == OpCode::Mul ? 3 : 1);
    };
    return visit(root);
}
static void relations() {
    using namespace predicate_detail;
    Program p;
    auto a = node(p, "a", 1), b = node(p, "b", 1), select = node(p, "select", 2);
    auto na = operation(p, OperationKind::Unary, OpCode::LogicNot, {a}, 1);
    auto both = binary(p, OpCode::LogicAnd, a, b, 1);
    auto either = binary(p, OpCode::LogicOr, a, b, 1);
    auto eq0 = binary(p, OpCode::Eq, select, literal(0, 2), 1);
    auto eq1 = binary(p, OpCode::Eq, select, literal(1, 2), 1);
    PredicateRelations q(p);
    CHECK(q.implies(a, a) == Proof::Proven);
    CHECK(q.isExclusive(a, na) == Proof::Proven);
    CHECK(q.implies(both, a) == Proof::Proven);
    CHECK(q.implies(a, either) == Proof::Proven);
    CHECK(q.isExclusive(eq0, eq1) == Proof::Proven);
    CHECK(q.isExclusive(a, b) == Proof::Unknown);
    CHECK(q.implies(either, a) == Proof::Unknown);
    auto context = branchContext(guardedContext(a, true), b, true);
    CHECK(context.predicates.size() == 2);
    CHECK(q.implies(context, {both, true}) == Proof::Proven);
    // Reusing a query object after graph mutation must not retain stale proofs.
    p.signal(eq1.node).driver->operands[1] = literal(0, 2);
    CHECK(q.isExclusive(eq0, eq1) == Proof::Unknown);
    for (int i = 0; i < 20; ++i) context = branchContext(context, node(p, "g" + std::to_string(i), 1), true);
    CHECK(context.predicates.size() <= 8);
    CHECK(q.implies(context, {a, true}) == Proof::Unknown);
    auto many = a;
    for (int i = 0; i < 10; ++i) many = binary(p, OpCode::LogicOr, many, node(p, "u"+std::to_string(i), 1), 1);
    CHECK(q.implies(a, many) == Proof::Unknown); // budget exhaustion is conservative
    std::vector<Context> contexts;
    for (int i = 0; i < 20; ++i)
        appendContext(contexts, guardedContext(node(p, "ctx" + std::to_string(i), 1), true));
    CHECK(contexts.size() == 1 && isUnconditional(contexts[0]));
    // Different limb padding must not manufacture exclusivity of equal constants.
    auto padded = literal(0, 2); padded.constant.limbs.push_back(0);
    auto equal_again = binary(p, OpCode::Eq, select, padded, 1);
    CHECK(q.isExclusive(eq0, equal_again) == Proof::Unknown);
}
static Program muxProgram(bool exclusive, bool shared = false) {
    Program p; p.function_name = "mux";
    auto sel = node(p, "sel", 2), a = node(p, "a", 8), b = node(p, "b", 8);
    auto eq0 = binary(p, OpCode::Eq, sel, literal(0, 2), 1);
    auto eq1 = binary(p, OpCode::Eq, sel, literal(1, 2), 1);
    auto eq2 = binary(p, OpCode::Eq, sel, literal(exclusive ? 2 : 0, 2), 1);
    auto tail = mux(p, eq2, a, literal(93));
    auto middle = mux(p, eq1, b, tail);
    auto root = mux(p, eq0, literal(211), middle);
    output(p, root); if (shared) output(p, middle);
    return p;
}
static void muxes() {
    for (bool exclusive : {false, true}) {
        auto p = muxProgram(exclusive); MutableProgram g(p);
        CHECK(parallelizeExclusiveMuxes(g) == exclusive);
        for (unsigned sel = 0; sel < 4; ++sel)
            for (unsigned a = 0; a < 256; ++a) {
                std::map<std::string,uint64_t> in{{"sel",sel},{"a",a},{"b",255-a}};
                CHECK(eval(p,p.outputs[0],in) == eval(g.program(),p.outputs[0],in));
            }
    }
    MutableProgram shared(muxProgram(true, true)); CHECK(!parallelizeExclusiveMuxes(shared));
    MutableProgram bounded(muxProgram(true)); CHECK(!parallelizeExclusiveMuxes(bounded, 2));
}
static void trees() {
    for (auto opcode : {OpCode::BitAnd, OpCode::BitOr, OpCode::BitXor, OpCode::Add}) {
        Program p; std::vector<Operand> inputs;
        for (int i = 0; i < 8; ++i) inputs.push_back(node(p, "a" + std::to_string(i), 8));
        auto root = inputs[0];
        for (int i = 1; i < 8; ++i) root = binary(p, opcode, root, inputs[i]);
        output(p, root); MutableProgram g(p);
        CHECK(balanceAssociativeTrees(g));
        CHECK(depth(p,root.node) == 7); CHECK(depth(g.program(),root.node) == 3);
        CHECK(!balanceAssociativeTrees(g));
        for (unsigned k = 0; k < 256; ++k) {
            std::map<std::string,uint64_t> in;
            for (int i = 0; i < 8; ++i) in["a"+std::to_string(i)] = (k * (i + 1) + i * 19) & 255;
            CHECK(eval(p,p.outputs[0],in) == eval(g.program(),p.outputs[0],in));
        }
        MutableProgram limited(p); CHECK(!balanceAssociativeTrees(limited, 3));
    }
    Program p;
    auto a = node(p,"a",8), b = node(p,"b",8), c = node(p,"c",8), d = node(p,"d",8);
    auto late = binary(p,OpCode::Mul,a,b);
    auto root = binary(p,OpCode::BitXor,binary(p,OpCode::BitXor,binary(p,OpCode::BitXor,late,a),c),d);
    output(p,root); MutableProgram g(p); CHECK(balanceAssociativeTrees(g));
    CHECK(depth(g.program(),root.node) == 4);
    // A shared internal node is a leaf for balancing, not duplicated/expanded.
    Program shared;
    auto v = node(shared,"v",8);
    auto pair = binary(shared,OpCode::BitXor,v,literal(7));
    auto end = binary(shared,OpCode::BitXor,pair,literal(8));
    output(shared,pair); output(shared,end);
    MutableProgram sg(shared); CHECK(!balanceAssociativeTrees(sg));
    // Do not traverse a truncation boundary or signed operand view.
    Program boundary;
    auto input = node(boundary,"input",8);
    auto add = binary(boundary,OpCode::Add,input,literal(255));
    auto trunc = operation(boundary,OperationKind::Trunc,OpCode::None,{add},4);
    auto extend = operation(boundary,OperationKind::ZExt,OpCode::None,{trunc},8);
    auto result = binary(boundary,OpCode::Add,extend,literal(1)); output(boundary,result);
    MutableProgram bg(boundary); CHECK(!balanceAssociativeTrees(bg));
    Program signed_tree;
    auto v0 = node(signed_tree,"v0",8); v0.signed_view = true;
    auto first = binary(signed_tree,OpCode::Add,v0,literal(1));
    auto second = binary(signed_tree,OpCode::Add,first,literal(2)); output(signed_tree,second);
    MutableProgram signed_graph(signed_tree); CHECK(!balanceAssociativeTrees(signed_graph));
}
static void postSinkingCleanup() {
    Program p;
    auto a = node(p,"a",1), b = node(p,"b",1);
    auto both = binary(p,OpCode::LogicAnd,a,b,1);
    auto inner = mux(p,both,literal(9),literal(10));
    auto root = mux(p,a,mux(p,b,inner,literal(9)),literal(9)); output(p,root);
    Options options; options.max_iterations = 0;
    auto result = optimizeProgram(p,options);
    for (const auto& signal : result.signals) if (signal.name == p.outputs[0]) {
        CHECK(signal.driver->kind == OperationKind::Assign);
        CHECK(signal.driver->operands[0].kind == OperandKind::Literal);
        CHECK(signal.driver->operands[0].constant.toU64() == 9);
    }
    for (unsigned a_value=0;a_value<2;++a_value) for(unsigned b_value=0;b_value<2;++b_value) {
        std::map<std::string,uint64_t> in{{"a",a_value},{"b",b_value}};
        CHECK(eval(p,p.outputs[0],in)==eval(result,p.outputs[0],in));
    }
    auto none = parseOptions({"none"});
    CHECK(!none.exclusive_muxes && !none.balance_trees && !none.predicate_sinking);
}
int main() { relations(); muxes(); trees(); postSinkingCleanup(); }
