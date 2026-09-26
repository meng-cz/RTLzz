#pragma once

#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"
#include "backend/beopt_dce.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred::beir::opt {
namespace boolean_detail {

// One multi-output AND/inverter DAG. Edge 0/1 is false/true; other low bits
// represent inversion. Unsupported operations are opaque primary inputs.
using Edge = std::size_t;
struct Node {
    bool input = false;
    Operand operand;
    Edge a = 0, b = 0;
    Edge fact = 0;
};

inline bool isBool(const ValueType& type) {
    return type.width == 1 && !type.isArray();
}
inline bool supported(const Signal& signal) {
    if (!isBool(signal.type) || !signal.driver) return false;
    const auto& op = *signal.driver;
    if (!isBool(op.type)) return false;
    for (const auto& operand : op.operands)
        if (!isBool(operand.type)) return false;
    switch (op.kind) {
    case OperationKind::Assign: return op.operands.size() == 1;
    case OperationKind::Unary:
        return op.operands.size() == 1 &&
               (op.op == OpCode::LogicNot || op.op == OpCode::BitNot);
    case OperationKind::Binary:
        return op.operands.size() == 2 &&
               (op.op == OpCode::BitAnd || op.op == OpCode::LogicAnd ||
                op.op == OpCode::BitOr || op.op == OpCode::LogicOr ||
                op.op == OpCode::BitXor || op.op == OpCode::Eq || op.op == OpCode::Ne);
    case OperationKind::Ite: return op.operands.size() == 3;
    case OperationKind::Case: return hasValidCaseShape(op);
    default: return false;
    }
}

inline constexpr unsigned kCutDepth = 8;
inline constexpr unsigned kCutInputs = 16;
inline constexpr std::size_t kCutNodes = 256;
// 4 -> 16 inputs grows the truth table from 16 to 65536 rows (4096x).
inline constexpr std::size_t kTruthBudget = 65536ULL * 4096;

// Facts use stable input IDs, independent of DAG rebuilding and BEIR NodeIds.
class InputRelations {
    std::map<Operand, Edge, decltype(&predicate_detail::operandLess)> ids_{
        &predicate_detail::operandLess};
    std::vector<Operand> operands_;
    std::set<std::pair<Edge, Edge>> implications_;
    std::map<Edge, Edge> aliases_;
    struct DecoderFact { Operand selector; std::vector<std::uint64_t> value; bool equal; };
    std::map<Edge, DecoderFact> decoders_;
    std::size_t decoder_revision_ = 0;
public:
    std::size_t decoderRevision() const { return decoder_revision_; }
    bool equalityDecoder(Edge edge) const {
        auto it = decoders_.find(edge & ~Edge{1});
        return it != decoders_.end() && (it->second.equal != bool(edge & 1));
    }
    // A positive equality fixes the entire selector. Eq/Ne decoder queries
    // sharing that exact typed selector need no generic implication search.
    std::optional<bool> decoderValue(Edge assumption, Edge query) const {
        if (!equalityDecoder(assumption)) return {};
        auto q = decoders_.find(query & ~Edge{1});
        if (q == decoders_.end()) return {};
        const auto& a = decoders_.at(assumption & ~Edge{1});
        if (!predicate_detail::sameOperand(a.selector, q->second.selector)) return {};
        bool equal = a.value == q->second.value;
        return (q->second.equal ? equal : !equal) != bool(query & 1);
    }
    Edge input(const Operand& operand) {
        auto found = ids_.find(operand);
        if (found != ids_.end()) return found->second;
        Edge id = (operands_.size() + 1) * 2;
        operands_.push_back(operand); ids_.emplace(operand, id); return id;
    }
    void imply(Edge a, Edge b) {
        implications_.emplace(a, b);
        implications_.emplace(b ^ 1, a ^ 1); // Contrapositive.
    }
    bool empty() const { return implications_.empty() && decoders_.empty(); }
    bool implies(Edge a, Edge b) const {
        return a == b || implications_.count({a,b});
    }
    Edge canonical(Edge e) const {
        while (aliases_.count(e)) e = aliases_.at(e);
        return e;
    }
    // Bit 0/1/2 denotes less/equal/greater. Require identical comparison
    // widths and signed interpretation; mixed extensions remain opaque.
    struct Compare { Operand a, b; unsigned mask; };
    static std::optional<Compare> comparison(const Operand& value, const Program& program) {
        const auto* op = predicate_detail::symbolDriver(value, program);
        if (!op || op->kind != OperationKind::Binary || op->operands.size()!=2) return {};
        unsigned mask;
        switch(op->op) {
        case OpCode::Lt: mask=1; break; case OpCode::Eq: mask=2; break;
        case OpCode::Le: mask=3; break; case OpCode::Gt: mask=4; break;
        case OpCode::Ne: mask=5; break; case OpCode::Ge: mask=6; break;
        default: return {};
        }
        Operand a=op->operands[0], b=op->operands[1];
        if (a.type.isArray() || a.type.width<=0 ||
            !predicate_detail::sameValueType(a.type,b.type) || a.signed_view!=b.signed_view)
            return {};
        for (const auto& v : {a,b}) if (v.kind==OperandKind::Literal &&
            (v.constant.width!=v.type.width || v.constant.signed_view!=v.signed_view)) return {};
        if (predicate_detail::operandLess(b,a)) {
            std::swap(a,b); mask=((mask&1)<<2)|(mask&2)|((mask&4)>>2);
        }
        return Compare{a,b,mask};
    }
    static unsigned possible(const Operand& x, const Operand& y, const Program& program) {
        auto a=comparison(x,program), b=comparison(y,program);
        if (!a || !b) return 15; // Four possible pairs of Boolean values.
        unsigned result=0;
        auto add=[&](unsigned lhs,unsigned rhs) {
            result |= 1u << ((bool(a->mask&lhs)?2:0) | (bool(b->mask&rhs)?1:0));
        };
        if (predicate_detail::sameOperand(a->a,b->a) && predicate_detail::sameOperand(a->b,b->b)) {
            if (predicate_detail::sameOperand(a->a,a->b)) add(2,2);
            else for (unsigned r : {1u,2u,4u}) add(r,r);
            return result;
        }
        // Constant thresholds sharing a selector. Use an overapproximation of
        // the ordered intervals, valid even when adjacent integers leave a gap.
        auto orient=[](Compare& c) {
            if (c.a.kind==OperandKind::Literal) {
                std::swap(c.a,c.b); c.mask=((c.mask&1)<<2)|(c.mask&2)|((c.mask&4)>>2);
            }
        };
        orient(*a); orient(*b);
        if (!predicate_detail::sameOperand(a->a,b->a) || a->b.kind!=OperandKind::Literal ||
            b->b.kind!=OperandKind::Literal || a->a.signed_view) return 15;
        auto limbs=[](Operand v) {
            v.constant.limbs.resize((v.type.width+63)/64,0);
            if (v.type.width%64) v.constant.limbs.back() &= (std::uint64_t{1}<<(v.type.width%64))-1;
            return v.constant.limbs;
        };
        const auto av=limbs(a->b), bv=limbs(b->b);
        int order=0;
        for (std::size_t k=av.size(); k-- > 0;) if (av[k]!=bv[k]) { order=av[k]<bv[k]?-1:1; break; }
        if (!order) { for(unsigned r:{1u,2u,4u}) add(r,r); }
        else if (order<0) { add(1,1);add(2,1);add(4,1);add(4,2);add(4,4); }
        else { add(1,1);add(1,2);add(1,4);add(2,4);add(4,4); }
        return result;
    }
    void prove(const Program& program) {
        ++decoder_revision_;
        decoders_.clear();
        for (std::size_t i = 0; i < operands_.size(); ++i) {
            auto c = comparison(operands_[i], program);
            if (!c || (c->mask != 2 && c->mask != 5)) continue;
            if (c->a.kind == OperandKind::Literal) std::swap(c->a, c->b);
            if (c->a.kind == OperandKind::Literal || c->b.kind != OperandKind::Literal) continue;
            auto bits = c->b.constant.limbs;
            bits.resize((c->b.type.width + 63) / 64, 0);
            if (c->b.type.width % 64) bits.back() &= (std::uint64_t{1} << (c->b.type.width % 64)) - 1;
            decoders_.emplace((i + 1) * 2, DecoderFact{c->a, std::move(bits), c->mask == 2});
        }
        // Index every visited signal in each input's three-level driving cone.
        // There is no pair-count cap: all pairs with a common signal are tried.
        std::map<Operand,std::vector<std::size_t>,decltype(&predicate_detail::operandLess)>
            postings{&predicate_detail::operandLess};
        for (std::size_t i=0;i<operands_.size();++i) {
            std::map<Operand,unsigned,decltype(&predicate_detail::operandLess)> seen{&predicate_detail::operandLess};
            std::function<void(const Operand&,unsigned)> visit=[&](const Operand& v,unsigned depth) {
                if(v.kind==OperandKind::Literal) return;
                auto found=seen.find(v);
                if(found!=seen.end() && found->second<=depth) return;
                seen[v]=depth;
                if(depth==3) return;
                if(const auto* op=predicate_detail::symbolDriver(v,program))
                    for(const auto& child:op->operands) visit(child,depth+1);
            };
            visit(operands_[i],0);
            for(const auto& entry:seen) postings[entry.first].push_back(i);
        }
        std::vector<std::set<std::size_t>> peers(operands_.size());
        for(const auto& entry:postings) {
            const auto& ids=entry.second;
            for(std::size_t i=0;i<ids.size();++i)
                for(std::size_t j=0;j<i;++j) peers[ids[i]].insert(ids[j]);
        }
        auto record=[&](std::size_t i,std::size_t j,unsigned allowed) {
            for(unsigned x=0;x<2;++x) for(unsigned y=0;y<2;++y)
                if(!(allowed&(1u<<(2*x+y))))
                    imply((i+1)*2+!x,(j+1)*2+y);
        };
        for(std::size_t i=0;i<peers.size();++i) for(auto j:peers[i]) {
            unsigned direct=possible(operands_[i],operands_[j],program);
            if(direct!=15) { record(i,j,direct); continue; }
            // Exact enumeration of a bounded Boolean abstraction. Comparisons
            // at the leaves constrain the assignments, instead of treating all
            // comparison outcomes as independent SAT atoms.
            struct Formula { int kind; unsigned a,b; }; // atom, constant, not, and, or
            std::vector<Formula> formulas;
            std::vector<Operand> atoms;
            bool overflow=false;
            std::function<unsigned(const Operand&,unsigned)> build=[&](const Operand& v,unsigned depth) {
                auto add=[&](int kind,unsigned a=0,unsigned b=0) {
                    formulas.push_back({kind,a,b}); return unsigned(formulas.size()-1);
                };
                if(v.kind==OperandKind::Literal) return add(1,!v.constant.isZero());
                const auto* op=predicate_detail::symbolDriver(v,program);
                if(op && depth<3) {
                    if(op->kind==OperationKind::Assign && op->operands.size()==1 &&
                       predicate_detail::sameValueType(op->type,op->operands[0].type))
                        return build(op->operands[0],depth+1);
                    if(op->kind==OperationKind::Unary && op->op==OpCode::LogicNot && op->operands.size()==1)
                        return add(2,build(op->operands[0],depth+1));
                    if(op->kind==OperationKind::Binary && op->operands.size()==2 &&
                       (op->op==OpCode::LogicAnd || op->op==OpCode::LogicOr ||
                        (op->operands[0].type.width==1 && op->operands[1].type.width==1 &&
                         !op->operands[0].type.isArray() && !op->operands[1].type.isArray() &&
                         (op->op==OpCode::BitAnd || op->op==OpCode::BitOr)))) {
                        auto x=build(op->operands[0],depth+1),y=build(op->operands[1],depth+1);
                        return add(op->op==OpCode::LogicAnd || op->op==OpCode::BitAnd?3:4,x,y);
                    }
                }
                unsigned k=0;
                while(k<atoms.size() && !predicate_detail::sameOperand(atoms[k],v)) ++k;
                if(k==atoms.size()) atoms.push_back(v);
                overflow |= atoms.size()>8;
                return add(0,k);
            };
            auto x=build(operands_[i],0),y=build(operands_[j],0);
            if(overflow) continue;
            struct Constraint { unsigned a,b,allowed; };
            std::vector<Constraint> constraints;
            for(unsigned a=0;a<atoms.size();++a) for(unsigned b=0;b<a;++b) {
                unsigned allowed=possible(atoms[a],atoms[b],program);
                if(allowed!=15) constraints.push_back({a,b,allowed});
            }
            unsigned allowed=0;
            std::vector<bool> values(formulas.size());
            for(unsigned bits=0;bits<(1u<<atoms.size());++bits) {
                bool valid=true;
                for(const auto& c:constraints)
                    if(!(c.allowed&(1u<<((((bits>>c.a)&1)*2)+((bits>>c.b)&1))))) {valid=false;break;}
                if(!valid) continue;
                for(unsigned k=0;k<formulas.size();++k) {
                    const auto& f=formulas[k];
                    values[k]=f.kind==0?bool((bits>>f.a)&1):f.kind==1?bool(f.a):
                              f.kind==2?!values[f.a]:f.kind==3?(values[f.a]&&values[f.b]):
                              (values[f.a]||values[f.b]);
                }
                allowed |= 1u << (unsigned(values[x])*2+unsigned(values[y]));
            }
            record(i,j,allowed);
        }
        for (const auto& relation : implications_) {
            if (!implications_.count({relation.second, relation.first})) continue;
            Edge a = canonical(relation.first), b = canonical(relation.second);
            if (a == b || a == (b ^ 1)) continue;
            if (a > b) std::swap(a,b);
            aliases_[b] = a; aliases_[b ^ 1] = a ^ 1;
        }
    }
};

class Dag {
    std::shared_ptr<InputRelations> relations_;
    std::map<Edge, Edge> canonical_inputs_;
    std::map<std::pair<Edge, Edge>, Edge> unique_;
    std::map<Operand, Edge, decltype(&predicate_detail::operandLess)> inputs_{
        &predicate_detail::operandLess};
    mutable std::vector<std::size_t> depths_{0};
    mutable std::size_t decoder_revision_ = 0;
    mutable std::map<Edge, std::unordered_map<std::size_t, int>> decoder_values_;
public:
    std::size_t depth(Edge root) const {
        // Nodes are immutable and topologically appended. Cache their actual
        // depths so proof setup does not repeatedly traverse shared cones.
        while (depths_.size() < nodes.size()) {
            const auto& n = nodes[depths_.size()];
            depths_.push_back(n.input ? 0 : 1 + std::max(depths_[n.a / 2], depths_[n.b / 2]));
        }
        return depths_[root / 2];
    }
    std::optional<bool> decoderImplies(Edge a, Edge b) const {
        if (a <= 1 || !nodes[a / 2].input) return {};
        const Edge fact = nodes[a / 2].fact ^ (a & 1);
        if (!relations_->equalityDecoder(fact)) return {};
        // Three-valued cofactor of a shared decoder DAG, on an explicit stack.
        // Its linear work is separate from the generic recursive proof budget.
        if (decoder_revision_ != relations_->decoderRevision()) {
            decoder_values_.clear(); decoder_revision_ = relations_->decoderRevision();
        }
        auto& known = decoder_values_[fact];
        known.emplace(0, 0);
        std::vector<std::pair<std::size_t, bool>> pending{{b / 2, false}};
        auto value = [&](Edge e) { int v = known.at(e / 2); return v < 0 ? -1 : v ^ int(e & 1); };
        while (!pending.empty()) {
            auto [id, expanded] = pending.back(); pending.pop_back();
            if (known.count(id)) continue;
            const Node n = nodes[id];
            if (n.input) {
                auto v = relations_->decoderValue(fact, n.fact);
                known[id] = v ? int(*v) : -1;
            } else if (!expanded) {
                pending.push_back({id, true});
                pending.push_back({n.b / 2, false}); pending.push_back({n.a / 2, false});
            } else {
                const int x = value(n.a), y = value(n.b);
                known[id] = (x == 0 || y == 0) ? 0 : (x == 1 && y == 1) ? 1 : -1;
            }
        }
        const int result = value(b);
        return result < 0 ? std::optional<bool>{} : std::optional<bool>{result != 0};
    }
    explicit Dag(std::shared_ptr<InputRelations> relations = std::make_shared<InputRelations>())
        : relations_(std::move(relations)) {}
    const auto& relations() const { return relations_; }
    std::vector<Node> nodes{Node{}};
    Edge input(Operand operand) {
        if (operand.kind == OperandKind::Literal) return !operand.constant.isZero();
        // Every imported operation is bit-level; signedness does not change
        // the value of an input bit. Boundary uses retain their original view.
        operand.signed_view = false;
        auto found = inputs_.find(operand);
        if (found != inputs_.end()) return found->second;
        const Edge fact = relations_->input(operand);
        const Edge canonical = relations_->canonical(fact);
        if (auto alias = canonical_inputs_.find(canonical & ~Edge{1}); alias != canonical_inputs_.end()) {
            const Edge result = alias->second ^ (canonical & 1);
            inputs_.emplace(std::move(operand), result); return result;
        }
        const Edge result = nodes.size() * 2;
        canonical_inputs_.emplace(canonical & ~Edge{1}, result ^ (canonical & 1));
        nodes.push_back({true, operand, 0, 0, fact});
        inputs_.emplace(std::move(operand), result);
        return result;
    }
    bool implies(Edge a, Edge b) const {
        if (auto decoded = decoderImplies(a, b)) return *decoded;
        const std::size_t max_depth = depth(a) + depth(b) + 1;
        // A DAG-aware allowance grows quadratically with actual depth, rather than
        // rejecting the ninth level of an otherwise inexpensive proof.
        std::size_t budget = 4 * (max_depth + 1) * (depth(a) + depth(b) + 2);
        std::map<std::pair<Edge, Edge>, bool> memo;
        return impliesWithin(a, b, max_depth, budget, memo);
    }
    bool impliesWithin(Edge a, Edge b, std::size_t remaining_depth, std::size_t& budget,
                       std::map<std::pair<Edge, Edge>, bool>& memo) const {
        if (!a || b == 1 || a == b) return true;
        if (a <= 1 || b <= 1) return false;
        const Node x = nodes[a / 2], y = nodes[b / 2];
        if (x.input && y.input) {
            if (auto decoded = relations_->decoderValue(x.fact ^ (a & 1), y.fact ^ (b & 1)))
                return *decoded;
            return relations_->implies(x.fact ^ (a & 1), y.fact ^ (b & 1));
        }
        // A recursive generic query may itself reach a decoder root.
        // Resolve that entire decoder cofactor before charging generic work.
        if (auto decoded = decoderImplies(a, b)) return *decoded;
        const auto key = std::make_pair(a, b);
        if (auto found = memo.find(key); found != memo.end()) return found->second;
        if (!budget || !remaining_depth) return false;
        --budget;
        auto prove = [&](Edge lhs, Edge rhs) { return impliesWithin(lhs, rhs, remaining_depth - 1, budget, memo); };
        bool result = false;
        if (!y.input && !(b & 1)) result = prove(a, y.a) && prove(a, y.b);
        else if (!x.input && (a & 1)) result = prove(x.a ^ 1, b) && prove(x.b ^ 1, b);
        else {
            if (!x.input && !(a & 1)) result = prove(x.a, b) || prove(x.b, b);
            if (!result && !y.input && (b & 1)) result = prove(a, y.a ^ 1) || prove(a, y.b ^ 1);
        }
        memo.emplace(key, result);
        return result;
    }
    Edge land(Edge a, Edge b, unsigned depth = 0) {
        if (a > b) std::swap(a, b);
        if (!a || a == (b ^ 1)) return 0;
        if (a == 1 || a == b) return b;
        if (!relations_->empty()) {
            if (implies(a, b ^ 1) || implies(b, a ^ 1)) return 0;
            if (implies(a, b)) return a;
            if (implies(b, a)) return b;
        }
        // Bounded, two-level absorption and contradiction. No tree expansion.
        auto contains = [&](Edge tree, Edge leaf) {
            return tree > 1 && !(tree & 1) && !nodes[tree / 2].input &&
                   (nodes[tree / 2].a == leaf || nodes[tree / 2].b == leaf);
        };
        if (contains(a, b)) return a;
        if (contains(b, a)) return b;
        if (contains(a, b ^ 1) || contains(b, a ^ 1)) return 0;
        // !(x & y) & !(x & !y) = !x. Complemented edges also
        // cover the dual (x | y) & (x | !y) = x without SAT/truth tables.
        if ((a & 1) && (b & 1) && !nodes[a / 2].input && !nodes[b / 2].input) {
            const Node lhs = nodes[a / 2], rhs = nodes[b / 2];
            for (auto pair : {std::make_pair(lhs.a, lhs.b), std::make_pair(lhs.b, lhs.a)}) {
                if (pair.first == rhs.a && pair.second == (rhs.b ^ 1)) return pair.first ^ 1;
                if (pair.first == rhs.b && pair.second == (rhs.a ^ 1)) return pair.first ^ 1;
            }
        }
        // x & !(x & y) = x & !y; dual OR absorption follows from inversion.
        auto reduce = [&](Edge leaf, Edge tree) -> std::optional<Edge> {
            if (depth >= 4 || tree <= 1 || !(tree & 1) || nodes[tree / 2].input) return {};
            const Node node = nodes[tree / 2];
            if (node.a == (leaf ^ 1) || node.b == (leaf ^ 1)) return leaf;
            if (node.a == leaf) return land(leaf, node.b ^ 1, depth + 1);
            if (node.b == leaf) return land(leaf, node.a ^ 1, depth + 1);
            return {};
        };
        if (auto result = reduce(a, b)) return *result;
        if (auto result = reduce(b, a)) return *result;
        const auto key = std::make_pair(a, b);
        if (auto found = unique_.find(key); found != unique_.end()) return found->second;
        const Edge result = nodes.size() * 2;
        nodes.push_back({false, {}, a, b});
        unique_.emplace(key, result);
        return result;
    }
    Edge lor(Edge a, Edge b) { return land(a ^ 1, b ^ 1) ^ 1; }
    Edge lxor(Edge a, Edge b) {
        const Edge polarity = (a ^ b) & 1;
        a &= ~Edge{1}; b &= ~Edge{1};
        if (a == b) return polarity;
        if (!a) return b ^ polarity;
        if (!b) return a ^ polarity;
        if (a > b) std::swap(a, b);
        return lor(land(a, b ^ 1), land(a ^ 1, b)) ^ polarity;
    }
    // Decode a mux represented as an AND/inverter network. Return values,
    // never references: reconstruction can append to nodes and reallocate it.
    std::optional<std::array<Edge, 3>> decodeMux(Edge root) const {
        if (root <= 1 || nodes[root / 2].input) return {};
        const Node top = nodes[root / 2];
        if (!(top.a & 1) || !(top.b & 1) || top.a <= 1 || top.b <= 1 ||
            nodes[top.a / 2].input || nodes[top.b / 2].input) return {};
        const Node a = nodes[top.a / 2], b = nodes[top.b / 2];
        for (auto x : {std::make_pair(a.a, a.b), std::make_pair(a.b, a.a)})
            for (auto y : {std::make_pair(b.a, b.b), std::make_pair(b.b, b.a)})
                if (x.first == (y.first ^ 1)) {
                    const Edge invert = (root & 1) ^ 1;
                    return std::array<Edge, 3>{x.first, x.second ^ invert, y.second ^ invert};
                }
        return {};
    }
    Edge mux(Edge c, Edge t, Edge f, unsigned nesting = 0) {
        // mux(c, mux(d,x,z), z) = mux(c & d,x,z), and the dual
        // false-arm form. Expose the conjunction to input-relation proofs.
        // Shared inner muxes are left intact for their other consumers.
        if (nesting < 4) {
            if (auto inner = decodeMux(t)) {
                auto [d, x, z] = *inner;
                if (x == f) { std::swap(x, z); d ^= 1; }
                if (z == f) return mux(land(c, d), x, f, nesting + 1);
            }
            if (auto inner = decodeMux(f)) {
                auto [d, x, z] = *inner;
                if (x == t) { std::swap(x, z); d ^= 1; }
                if (z == t) return mux(land(c ^ 1, d), x, t, nesting + 1);
            }
        }
        if (t == f) return t;
        if (c <= 1) return c ? t : f;
        if (t == 1 || t == c) return lor(c, f);
        if (t == 0 || t == (c ^ 1)) return land(c ^ 1, f);
        if (f == 0 || f == c) return land(c, t);
        if (f == 1 || f == (c ^ 1)) return lor(c ^ 1, t);
        if (t == (f ^ 1)) return lxor(c, f);
        return lor(land(c, t), land(c ^ 1, f));
    }

    // Inspect at most three AND levels. Complemented edges implement NOT,
    // OR/De Morgan without expanding into alternating rewrite directions.
    Edge localAnd(Edge a, Edge b) {
        std::vector<Edge> factors;
        std::function<void(Edge, unsigned)> collect = [&](Edge e, unsigned depth) {
            if (depth < 3 && e > 1 && !(e & 1) && !nodes[e / 2].input) {
                const Node n = nodes[e / 2];
                collect(n.a, depth + 1); collect(n.b, depth + 1);
            } else factors.push_back(e);
        };
        collect(a, 1); collect(b, 1);
        std::sort(factors.begin(), factors.end());
        bool redundant = false;
        for (std::size_t i = 1; i < factors.size(); ++i) {
            if (factors[i] == (factors[i - 1] ^ 1)) return 0;
            redundant |= factors[i] == factors[i - 1];
        }
        if (redundant) {
            factors.erase(std::unique(factors.begin(), factors.end()), factors.end());
            Edge result = 1;
            for (Edge e : factors) result = land(result, e);
            return result;
        }
        // Recover the mux hidden in an AIG: !((s&t)|(!s&f)).
        if (a > 1 && b > 1 && (a & 1) && (b & 1) &&
            !nodes[a / 2].input && !nodes[b / 2].input) {
            const Node lhs = nodes[a / 2], rhs = nodes[b / 2];
            for (auto l : {std::make_pair(lhs.a, lhs.b), std::make_pair(lhs.b, lhs.a)})
                for (auto r : {std::make_pair(rhs.a, rhs.b), std::make_pair(rhs.b, rhs.a)})
                    if (l.first == (r.first ^ 1)) return mux(l.first, l.second, r.second) ^ 1;
        }
        return land(a, b);
    }

    void rewriteLocal(std::vector<Edge>& roots) {
        // Node IDs are topological; appended nodes are deferred to the next
        // sweep. Never mutate an existing node or invalidate its unique key.
        for (unsigned round = 0; round < 4; ++round) {
            const std::size_t count = nodes.size();
            std::vector<Edge> mapped(count);
            for (std::size_t id = 1; id < count; ++id) {
                const Node n = nodes[id];
                mapped[id] = n.input ? id * 2 :
                    localAnd(mapped[n.a / 2] ^ (n.a & 1), mapped[n.b / 2] ^ (n.b & 1));
            }
            bool changed = false;
            for (auto& root : roots) {
                const Edge replacement = mapped[root / 2] ^ (root & 1);
                changed |= replacement != root;
                root = replacement;
            }
            if (!changed) break;
        }
    }

    // The imported Boolean representation is already an AIG (AND + inverted
    // edges). Rebuild it in topological order to propagate constants, share
    // identical gates and balance associative regions without copying fanout.
    void optimizeAig(std::vector<Edge>& roots, const std::vector<bool>& outputs) {
        for (unsigned round = 0; round < 3; ++round) {
            std::vector<bool> boundary(nodes.size()), live(nodes.size());
            std::vector<Edge> pending;
            for (std::size_t i = 0; i < roots.size(); ++i) if (outputs[i]) {
                boundary[roots[i] / 2] = true;
                pending.push_back(roots[i]);
            }
            while (!pending.empty()) {
                const auto id = pending.back() / 2; pending.pop_back();
                if (!id || live[id]) continue;
                live[id] = true;
                if (!nodes[id].input) {
                    pending.push_back(nodes[id].a); pending.push_back(nodes[id].b);
                }
            }
            std::vector<unsigned> fanout(nodes.size());
            for (std::size_t id = 1; id < nodes.size(); ++id)
                if (live[id] && !nodes[id].input) {
                    ++fanout[nodes[id].a / 2]; ++fanout[nodes[id].b / 2];
                }
            Dag next(relations_);
            std::vector<Edge> mapped(nodes.size());
            std::vector<unsigned> depth(1, 0);
            auto level = [&](Edge edge) {
                while (depth.size() < next.nodes.size()) {
                    const Node& n = next.nodes[depth.size()];
                    depth.push_back(n.input ? 0 : 1 + std::max(depth[n.a / 2], depth[n.b / 2]));
                }
                return depth[edge / 2];
            };
            bool balanced = false;
            for (std::size_t id = 1; id < nodes.size(); ++id) {
                if (!live[id]) continue;
                const Node n = nodes[id];
                if (n.input) { mapped[id] = next.input(n.operand); continue; }
                auto translate = [&](Edge e) { return mapped[e / 2] ^ (e & 1); };
                Edge result = next.land(translate(n.a), translate(n.b));
                std::vector<Edge> leaves;
                std::function<void(Edge, unsigned)> collect = [&](Edge e, unsigned distance) {
                    if (leaves.size() > 64) return;
                    const auto child = e / 2;
                    if (e > 1 && !(e & 1) && !nodes[child].input &&
                        fanout[child] == 1 && !boundary[child] && distance < 8) {
                        collect(nodes[child].a, distance + 1);
                        collect(nodes[child].b, distance + 1);
                    } else leaves.push_back(translate(e));
                };
                collect(n.a, 1); collect(n.b, 1);
                if (leaves.size() > 2 && leaves.size() <= 64) {
                    using Item = std::pair<unsigned, Edge>;
                    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
                    for (Edge leaf : leaves) queue.push({level(leaf), leaf});
                    while (queue.size() > 1) {
                        Edge a = queue.top().second; queue.pop();
                        Edge b = queue.top().second; queue.pop();
                        Edge joined = next.land(a, b);
                        queue.push({level(joined), joined});
                    }
                    const Edge candidate = queue.top().second;
                    // Equal-depth alternatives are not rewritten back and forth.
                    if (level(candidate) < level(result)) {
                        result = candidate;
                        balanced = true;
                    }
                }
                mapped[id] = result;
            }
            for (auto& root : roots) root = mapped[root / 2] ^ (root & 1);
            *this = std::move(next);
            if (!balanced) break;
        }
    }

    // Replay saved input facts after balancing has changed the AIG shape.
    // Stable input operands reconnect facts even when node IDs were renumbered.
    // One topological sweep propagates each simplification to every consumer.
    void propagateRelations(std::vector<Edge>& roots, const InputRelations& snapshot) {
        Dag next(std::make_shared<InputRelations>(snapshot));
        std::vector<Edge> mapped(nodes.size());
        for (std::size_t id = 1; id < nodes.size(); ++id) {
            const Node node = nodes[id];
            mapped[id] = node.input ? next.input(node.operand) :
                next.land(mapped[node.a / 2] ^ (node.a & 1),
                          mapped[node.b / 2] ^ (node.b & 1));
        }
        for (auto& root : roots) root = mapped[root / 2] ^ (root & 1);
        *this = std::move(next);
    }

    // Exact truth table of a bounded cut. Only single-fanout, non-output
    // interiors are expanded, so replacing it cannot duplicate shared work.
    Edge optimizeCut(Edge root, const std::vector<std::size_t>& fanout,
                     const std::vector<bool>& outputs, std::size_t& budget) {
        if (root <= 1 || nodes[root / 2].input || !budget) return root;
        std::vector<Edge> leaves;
        std::vector<std::size_t> inside;
        bool overflow = false;
        std::function<void(Edge, unsigned)> collect = [&](Edge edge, unsigned depth) {
            const auto id = edge / 2;
            if (!id || overflow) return;
            if (std::find(inside.begin(), inside.end(), id) != inside.end()) return;
            if (id != root / 2 && (nodes[id].input || depth >= kCutDepth ||
                id >= fanout.size() || fanout[id] != 1 || outputs[id])) {
                const Edge leaf = id * 2;
                if (std::find(leaves.begin(), leaves.end(), leaf) == leaves.end())
                    leaves.push_back(leaf);
                overflow = leaves.size() > kCutInputs;
                return;
            }
            inside.push_back(id);
            if (inside.size() > kCutNodes) { overflow = true; return; }
            collect(nodes[id].a, depth + 1);
            collect(nodes[id].b, depth + 1);
        };
        collect(root, 0);
        if (overflow || inside.size() < 2) return root;
        const std::size_t rows = std::size_t{1} << leaves.size();
        // Charge before allocation, including evaluation, input construction
        // and all levels of Shannon scans. Large cuts cannot evade the budget.
        const std::size_t work = rows * (inside.size() + 2 * leaves.size() + 1);
        if (budget < work) return root;
        budget -= work;
        std::sort(leaves.begin(), leaves.end());
        std::sort(inside.begin(), inside.end());
        using Table = std::vector<std::uint64_t>;
        const std::size_t words = (rows + 63) / 64;
        std::unordered_map<std::size_t, Table> tables;
        tables.emplace(0, Table(words, 0));
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            Table table(words, 0);
            for (std::size_t row = 0; row < rows; ++row)
                if ((row >> i) & 1) table[row / 64] |= std::uint64_t{1} << (row % 64);
            tables.emplace(leaves[i] / 2, std::move(table));
        }
        for (auto id : inside) {
            const Node node = nodes[id];
            const auto& a = tables.at(node.a / 2);
            const auto& b = tables.at(node.b / 2);
            Table table(words);
            for (std::size_t w = 0; w < words; ++w)
                table[w] = (a[w] ^ ((node.a & 1) ? ~std::uint64_t{0} : 0)) &
                           (b[w] ^ ((node.b & 1) ? ~std::uint64_t{0} : 0));
            tables.emplace(id, std::move(table));
        }
        const auto& truth = tables.at(root / 2);
        auto bit = [&](std::size_t row) { return ((truth[row / 64] >> (row % 64)) & 1) ^ (root & 1); };
        // Read cofactors as ranges, avoiding overflow-prone integer masks and
        // copying complete tables at every recursion level (at most 16).
        std::function<Edge(std::size_t, unsigned)> synth = [&](std::size_t offset, unsigned count) -> Edge {
            const std::size_t length = std::size_t{1} << count;
            const auto first = bit(offset);
            bool uniform = true;
            for (std::size_t i = 1; i < length; ++i)
                if (bit(offset + i) != first) { uniform = false; break; }
            if (uniform) return first;
            const auto half = length / 2;
            Edge lo = synth(offset, count - 1);
            Edge hi = synth(offset + half, count - 1);
            return mux(leaves[count - 1], hi, lo);
        };
        Edge replacement = synth(0, static_cast<unsigned>(leaves.size()));
        std::unordered_set<std::size_t> used;
        std::vector<Edge> pending{replacement};
        while (!pending.empty()) {
            Edge edge = pending.back(); pending.pop_back();
            if (edge <= 1 || std::binary_search(leaves.begin(), leaves.end(), edge & ~Edge{1})) continue;
            if (!used.insert(edge / 2).second) continue;
            if (used.size() >= inside.size()) return root;
            pending.push_back(nodes[edge / 2].a);
            pending.push_back(nodes[edge / 2].b);
        }
        return replacement;
    }
};

inline Operand constant(bool value) {
    Operand operand;
    operand.kind = OperandKind::Literal;
    operand.type = {1, {}};
    operand.constant.width = 1;
    operand.constant.limbs = {value ? 1u : 0u};
    return operand;
}

} // namespace boolean_detail

inline bool normalizeBooleanControl(MutableProgram& graph) {
    using namespace boolean_detail;
    Program& program = graph.program();
    const auto order = width_detail::topologicalOrder(program);
    const std::size_t count = program.signals.size();
    std::vector<bool> internal(count), output(count);
    for (const auto& signal : program.signals) {
        internal[signal.id] = supported(signal);
        output[signal.id] = isBool(signal.type) && graph.isObservable(signal);
    }
    for (const auto& signal : program.signals) {
        if (!signal.driver || internal[signal.id]) continue;
        for (const auto& operand : signal.driver->operands)
            if (operand.kind == OperandKind::Symbol && operand.node < count &&
                isBool(operand.type)) output[operand.node] = true;
    }
    Dag dag;
    std::vector<Edge> values(count);
    auto edge = [&](const Operand& operand) -> Edge {
        if (operand.kind == OperandKind::Symbol) return values.at(operand.node);
        return dag.input(operand);
    };
    for (NodeId id : order) {
        const auto& signal = program.signal(id);
        if (!isBool(signal.type)) continue;
        if (!internal[id]) {
            Operand operand;
            operand.kind = OperandKind::Symbol; operand.node = id;
            operand.text = signal.name; operand.type = signal.type;
            values[id] = dag.input(operand);
            continue;
        }
        const auto& op = *signal.driver;
        auto get = [&](std::size_t i) { return edge(op.operands[i]); };
        Edge result = 0;
        if (op.kind == OperationKind::Assign) result = get(0);
        else if (op.kind == OperationKind::Unary) result = get(0) ^ 1;
        else if (op.kind == OperationKind::Ite) result = dag.mux(get(0), get(1), get(2));
        else if (op.kind == OperationKind::Case) {
            result = edge(op.operands.back());
            for (std::size_t i = caseBranchCount(op); i-- > 0;)
                result = dag.mux(get(i * 2), get(i * 2 + 1), result);
        } else if (op.op == OpCode::BitAnd || op.op == OpCode::LogicAnd)
            result = dag.land(get(0), get(1));
        else if (op.op == OpCode::BitOr || op.op == OpCode::LogicOr)
            result = dag.lor(get(0), get(1));
        else {
            result = dag.lxor(get(0), get(1));
            if (op.op == OpCode::Eq) result ^= 1;
        }
        values[id] = result;
    }
    dag.relations()->prove(program);
    const InputRelations input_relations_snapshot = *dag.relations();
    dag.rewriteLocal(values);
    dag.optimizeAig(values, output);
    dag.propagateRelations(values, input_relations_snapshot);
    // Prune the temporary DAG before cut analysis; dead original logic must
    // neither count as shared fanout nor consume the truth-table budget.
    const std::size_t initial = dag.nodes.size();
    std::vector<bool> live(initial), boundaries(initial);
    std::vector<Edge> pending;
    for (std::size_t id = 0; id < count; ++id) if (output[id]) {
        pending.push_back(values[id]); boundaries[values[id] / 2] = true;
    }
    while (!pending.empty()) {
        const auto id = pending.back() / 2; pending.pop_back();
        if (!id || live[id]) continue;
        live[id] = true;
        if (!dag.nodes[id].input) {
            pending.push_back(dag.nodes[id].a); pending.push_back(dag.nodes[id].b);
        }
    }
    std::vector<std::size_t> fanout(initial);
    for (std::size_t id = 1; id < initial; ++id) if (live[id] && !dag.nodes[id].input) {
        ++fanout[dag.nodes[id].a / 2]; ++fanout[dag.nodes[id].b / 2];
    }
    std::vector<Edge> mapped(initial);
    std::size_t budget = kTruthBudget;
    for (std::size_t id = 1; id < initial; ++id) if (live[id]) {
        Edge optimized = dag.optimizeCut(id * 2, fanout, boundaries, budget);
        mapped[id] = optimized;
    }
    // Rebuild through a second canonical DAG so substitutions propagate to all
    // outputs and newly equal expressions merge, without iterating whole cones.
    Dag final(dag.relations());
    // Resolve mapped edges with an explicit stack; replacements depend only on
    // their cut leaves, so the resulting graph remains acyclic.
    std::vector<Edge> resolved(dag.nodes.size());
    std::vector<bool> done(dag.nodes.size(), false); done[0] = true;
    auto resolve = [&](Edge root) {
        std::vector<std::pair<std::size_t, bool>> stack{{root / 2, false}};
        while (!stack.empty()) {
            auto [id, expanded] = stack.back(); stack.pop_back();
            if (done[id]) continue;
            Edge replacement = id < initial && live[id] ? mapped[id] : id * 2;
            if (replacement != id * 2) {
                if (!done[replacement / 2]) {
                    stack.push_back({id, true}); stack.push_back({replacement / 2, false}); continue;
                }
                resolved[id] = resolved[replacement / 2] ^ (replacement & 1);
            } else if (dag.nodes[id].input) resolved[id] = final.input(dag.nodes[id].operand);
            else {
                const auto node = dag.nodes[id];
                if (!expanded) {
                    stack.push_back({id, true}); stack.push_back({node.b / 2, false});
                    stack.push_back({node.a / 2, false}); continue;
                }
                resolved[id] = final.land(resolved[node.a / 2] ^ (node.a & 1),
                                          resolved[node.b / 2] ^ (node.b & 1));
            }
            done[id] = true;
        }
        return resolved[root / 2] ^ (root & 1);
    };
    for (std::size_t id = 0; id < count; ++id)
        if (output[id]) values[id] = resolve(values[id]);
    // Reuse original signal slots where possible, including complemented roots.
    // This makes an already canonical graph a fixed point rather than emitting
    // a fresh copy on every optimizer iteration.
    std::unordered_map<Edge, NodeId> representatives;
    for (NodeId id : order) if (internal[id] && !output[id] && done[values[id] / 2]) {
        // Internal values still refer to the first DAG.
        Edge value = resolved[values[id] / 2] ^ (values[id] & 1);
        if (value > 1 && ((value & 1) || !final.nodes[value / 2].input)) representatives.emplace(value, id);
    }
    for (NodeId id : order) if (internal[id] && output[id] && values[id] > 1 &&
                                ((values[id] & 1) || !final.nodes[values[id] / 2].input))
        representatives.emplace(values[id], id);
    std::unordered_map<Edge, Operand> emitted;
    emitted.emplace(0, constant(false)); emitted.emplace(1, constant(true));
    bool changed = false;
    auto write = [&](Edge value, Operation op) {
        Operand operand;
        auto found = representatives.find(value);
        if (found == representatives.end()) {
            operand = width_detail::appendTemp(program, {1, {}}, std::move(op), "normalized shared Boolean DAG");
            changed = true;
        } else {
            auto& signal = program.signal(found->second);
            op.debug = signal.driver->debug; op.source_locs = signal.driver->source_locs;
            if (!(graph.operationSignature(*signal.driver) == graph.operationSignature(op))) {
                signal.driver = std::move(op); changed = true;
            }
            operand.kind = OperandKind::Symbol; operand.node = signal.id;
            operand.text = signal.name; operand.type = signal.type;
        }
        emitted.emplace(value, operand);
    };
    std::vector<std::pair<Edge, bool>> stack;
    for (std::size_t id = 0; id < count; ++id) if (output[id]) stack.push_back({values[id], false});
    while (!stack.empty()) {
        auto [value, expanded] = stack.back(); stack.pop_back();
        if (emitted.count(value)) continue;
        const Node node = final.nodes[value / 2];
        if (!(value & 1) && node.input) { emitted.emplace(value, node.operand); continue; }
        if (!expanded) {
            stack.push_back({value, true});
            if (value & 1) stack.push_back({value ^ 1, false});
            else { stack.push_back({node.b, false}); stack.push_back({node.a, false}); }
            continue;
        }
        Operation op; op.type = {1, {}};
        if (value & 1) {
            op.kind = OperationKind::Unary; op.op = OpCode::LogicNot;
            op.operands = {emitted.at(value ^ 1)};
        } else {
            op.kind = OperationKind::Binary; op.op = OpCode::BitAnd;
            op.operands = {emitted.at(node.a), emitted.at(node.b)};
        }
        write(value, std::move(op));
    }
    for (std::size_t id = 0; id < count; ++id) if (internal[id] && output[id]) {
        const auto operand = emitted.at(values[id]);
        if (operand.kind == OperandKind::Symbol && operand.node == id) continue;
        auto& signal = program.signal(static_cast<NodeId>(id));
        Operation op; op.kind = OperationKind::Assign; op.type = signal.type;
        op.operands = {operand}; op.debug = signal.driver->debug;
        op.source_locs = signal.driver->source_locs;
        if (!(graph.operationSignature(*signal.driver) == graph.operationSignature(op))) {
            signal.driver = std::move(op); changed = true;
        }
    }
    if (changed) graph.markValueFactsDirty();
    // Remove superseded Boolean nodes and their now-unused cones from BEIR.
    return eliminateDeadNodes(graph) || changed;
}

} // namespace pred::beir::opt
