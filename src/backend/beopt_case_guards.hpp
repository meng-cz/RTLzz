#pragma once

#include "backend/beopt_structure.hpp"
#include <map>
#include <set>

namespace pred::beir::opt {
namespace case_guard_detail {

// A complemented-edge Boolean DAG. 0/1 are false/true; other even IDs
// name an atom or an AND, and the low bit denotes complementation.
// No arithmetic relation is assumed: small constant equality decoders are
// expanded exactly, all other data-dependent computations remain atoms.
class Guards {
    using Id = unsigned;
    struct Node { Id a = 0, b = 0; Operand atom; int bit = -1; bool conjunction = false; };
    Program& program_;
    unsigned limit_;
    std::vector<Node> nodes_{Node{}};
    std::map<std::pair<Id, Id>, Id> ands_;
    struct AtomLess {
        bool operator()(const std::pair<Operand,int>& a, const std::pair<Operand,int>& b) const {
            if (!predicate_detail::sameOperand(a.first,b.first))
                return predicate_detail::operandLess(a.first,b.first);
            return a.second < b.second;
        }
    };
    std::map<std::pair<Operand,int>, Id, AtomLess> atoms_;
    std::map<std::pair<Operand,int>, Operand, AtomLess> decoders_;
    std::map<NodeId,Id> converted_;
    std::map<Id,Operand> emitted_;
    std::map<Id,Id> simplified_;
    std::set<Id> exclusion_roots_;
    DebugInfo debug_;

    Id atom(Operand value, int bit = -1) {
        // Signed views do not change the actual bits of an atom.
        value.signed_view = false; value.constant.signed_view = false;
        const auto key = std::make_pair(value,bit);
        if (auto it = atoms_.find(key); it != atoms_.end()) return it->second;
        const Id id = static_cast<Id>(nodes_.size() * 2);
        nodes_.push_back({0,0,value,bit,false}); atoms_.emplace(key,id);
        return id;
    }
    Id land(Id a, Id b) {
        if (!a || !b || a == (b ^ 1u)) return 0;
        if (a == 1 || a == b) return b;
        if (b == 1) return a;
        if (a > b) std::swap(a,b);
        const auto key = std::make_pair(a,b);
        if (auto it = ands_.find(key); it != ands_.end()) return it->second;
        const Id id = static_cast<Id>(nodes_.size() * 2);
        nodes_.push_back({a,b,{},-1,true}); ands_.emplace(key,id);
        return id;
    }
    Id lor(Id a, Id b) { return land(a ^ 1u,b ^ 1u) ^ 1u; }
    // Aligned power-of-two blocks share work between successive prefixes.
    // Unlike a serial accumulated OR, every prefix has logarithmic depth.
    Id tree(const std::vector<Id>& terms, std::size_t first, std::size_t count, bool is_or) {
        if (!count) return is_or ? 0 : 1;
        if (count == 1) return terms[first];
        std::size_t split = 1;
        while (split * 2 < count) split *= 2;
        const Id a = tree(terms,first,split,is_or);
        const Id b = tree(terms,first+split,count-split,is_or);
        return is_or ? lor(a,b) : land(a,b);
    }
    Id bit(Operand value, int index, unsigned depth = 0) {
        if (depth > limit_) return atom(value,index);
        if (value.kind == OperandKind::Literal) {
            const auto limb = static_cast<std::size_t>(index / 64);
            return limb < value.constant.limbs.size()
                ? ((value.constant.limbs[limb] >> (index % 64)) & 1u) : 0;
        }
        const auto* op = predicate_detail::symbolDriver(value,program_);
        if (op && op->operands.size() == 1) {
            const auto& source = op->operands[0];
            if (op->kind == OperationKind::Assign && source.type.width == value.type.width)
                return bit(source,index,depth+1);
            if (op->kind == OperationKind::Slice) return bit(source,index+op->lo,depth+1);
            if (op->kind == OperationKind::BitSelect) return bit(source,op->bit,depth+1);
            if (op->kind == OperationKind::Trunc || op->kind == OperationKind::ZExt ||
                op->kind == OperationKind::SExt || op->kind == OperationKind::Cast) {
                if (op->signed_truncation) return atom(value,index);
                if (index < source.type.width) return bit(source,index,depth+1);
                if (op->kind == OperationKind::ZExt || op->kind == OperationKind::Cast)
                    return 0;
                if (op->kind == OperationKind::SExt)
                    return bit(source,source.type.width-1,depth+1);
            }
        }
        return atom(value, value.type.width == 1 && index == 0 ? -1 : index);
    }
    Id convert(const Operand& value, unsigned depth = 0) {
        if (value.kind == OperandKind::Literal) return !value.constant.isZero();
        if (auto it = converted_.find(value.node); value.kind == OperandKind::Symbol && it != converted_.end())
            return it->second;
        if (depth > limit_) return atom(value);
        Id result = atom(value);
        const auto* op = predicate_detail::symbolDriver(value,program_);
        if (op) {
            if (op->kind == OperationKind::Assign && op->operands.size() == 1 && op->operands[0].type.width == 1)
                result = convert(op->operands[0],depth+1);
            else if (op->kind == OperationKind::BitSelect || op->kind == OperationKind::Slice ||
                     op->kind == OperationKind::Trunc)
                result = bit(value,0);
            else if (op->kind == OperationKind::Unary && op->operands.size() == 1 &&
                     op->operands[0].type.width == 1 &&
                     (op->op == OpCode::LogicNot || op->op == OpCode::BitNot))
                result = convert(op->operands[0],depth+1) ^ 1u;
            else if (op->kind == OperationKind::Binary && op->operands.size() == 2) {
                const auto& a = op->operands[0]; const auto& b = op->operands[1];
                if (a.type.width == 1 && b.type.width == 1 &&
                    (op->op == OpCode::BitAnd || op->op == OpCode::LogicAnd ||
                     op->op == OpCode::BitOr || op->op == OpCode::LogicOr)) {
                    Id x = convert(a,depth+1), y = convert(b,depth+1);
                    result = op->op == OpCode::BitAnd || op->op == OpCode::LogicAnd ? land(x,y) : lor(x,y);
                } else if ((op->op == OpCode::Eq || op->op == OpCode::Ne) &&
                           a.type.width == b.type.width && a.type.width <= 16 &&
                           !a.type.isArray() && !b.type.isArray() &&
                           (a.kind == OperandKind::Literal || b.kind == OperandKind::Literal)) {
                    std::vector<Id> terms;
                    for (int i=0; i<a.type.width; ++i) {
                        const Id x = bit(a,i), y = bit(b,i);
                        terms.push_back(y <= 1 ? (y ? x : x ^ 1u) : (x ? y : y ^ 1u));
                    }
                    result = tree(terms,0,terms.size(),false) ^ (op->op == OpCode::Ne ? 1u : 0u);
                }
            }
        }
        if (value.kind == OperandKind::Symbol) converted_[value.node] = result;
        return result;
    }
    void factors(Id id, std::set<Id>& out) {
        std::vector<Id> pending{id}; std::set<Id> visited;
        while (!pending.empty()) {
            Id next = pending.back(); pending.pop_back();
            if (!visited.insert(next).second || next == 1) continue;
            const auto node = nodes_[next/2];
            if (!(next & 1u) && node.conjunction && out.size()+pending.size() < limit_) {
                pending.push_back(node.b); pending.push_back(node.a);
            } else out.insert(next);
        }
    }
    Id cofactor(Id id, const std::set<Id>& facts, std::map<Id,Id>& memo, unsigned& visits) {
        if (id <= 1) return id;
        if (facts.count(id)) return 1;
        if (facts.count(id ^ 1u)) return 0;
        if (auto it=memo.find(id); it!=memo.end()) return it->second;
        if (++visits > limit_*16u) return id;
        const auto node = nodes_[id/2];
        if (!node.conjunction) return id;
        const Id a = cofactor(node.a,facts,memo,visits);
        const Id b = cofactor(node.b,facts,memo,visits);
        return memo[id] = land(a,b) ^ (id & 1u);
    }
    Id simplify(Id id) {
        if (auto it=simplified_.find(id); it!=simplified_.end()) return it->second;
        std::set<Id> terms; factors(id,terms);
        // Each rewrite is justified by the other retained conjuncts, never
        // by the term being removed. This also eliminates dead leaf paths.
        for (unsigned round=0; round<8; ++round) {
            if (terms.count(0)) return simplified_[id]=0;
            for (Id term: terms) if (terms.count(term^1u)) return simplified_[id]=0;
            bool changed=false;
            auto candidates=terms;
            for (Id term:candidates) {
                if (!terms.count(term) || !nodes_[term/2].conjunction) continue;
                terms.erase(term);
                std::map<Id,Id> memo; unsigned visits=0;
                const Id replacement=cofactor(term,terms,memo,visits);
                factors(replacement,terms);
                changed |= replacement!=term;
                if (terms.count(0)) return simplified_[id]=0;
            }
            if (!changed) break;
        }
        std::vector<Id> positive, excluded;
        for (Id term:terms) {
            if (term & 1u) excluded.push_back(term^1u);
            else positive.push_back(term);
        }
        // Rebuild exclusions from their unmasked predicates, not from a
        // recursively masked preceding arm. De Morgan gives a prefix OR.
        const Id enabled=tree(positive,0,positive.size(),false);
        const Id blocked=tree(excluded,0,excluded.size(),true);
        if (excluded.size()>1 && blocked>1) exclusion_roots_.insert(blocked^1u);
        return simplified_[id]=land(enabled,blocked^1u);
    }
    std::optional<Operand> emitDecoders(Id id) {
        if (id<=1 || (id&1u) || !nodes_[id/2].conjunction) return std::nullopt;
        std::set<Id> terms; factors(id,terms);
        struct Decode { unsigned mask=0,value=0; std::vector<Id> terms; };
        std::map<std::pair<Operand,int>,Decode,AtomLess> groups;
        for (Id term:terms) {
            const auto& node=nodes_[term/2];
            if (node.conjunction || node.bit<0 || node.atom.type.width>16 ||
                node.atom.type.width<2) continue;
            auto& group=groups[{node.atom,0}];
            group.mask|=1u<<node.bit;
            if (!(term&1u)) group.value|=1u<<node.bit;
            group.terms.push_back(term);
        }
        std::vector<Operand> values;
        for (const auto& entry:groups) {
            const auto& source=entry.first.first;
            const auto& group=entry.second;
            if (group.mask!=((1u<<source.type.width)-1)) continue;
            const auto key=std::make_pair(source,static_cast<int>(group.value));
            auto found=decoders_.find(key);
            if (found==decoders_.end()) {
                Operand literal; literal.kind=OperandKind::Literal; literal.type=source.type;
                literal.constant.width=source.type.width; literal.constant.limbs={group.value};
                auto value=structure_detail::emit(program_,OperationKind::Binary,OpCode::Eq,
                                                  {1,{}},{source,literal},debug_);
                found=decoders_.emplace(key,value).first;
            }
            values.push_back(found->second);
            for (Id term:group.terms) terms.erase(term);
        }
        if (values.empty()) return std::nullopt;
        for (Id term:terms) values.push_back(emit(term));
        return structure_detail::balanced(program_,std::move(values),OpCode::BitAnd,{1,{}},debug_);
    }
    Operand emit(Id id) {
        if (auto it=emitted_.find(id); it!=emitted_.end()) return it->second;
        if (auto decoded=emitDecoders(id)) return emitted_[id]=*decoded;
        Operand result;
        if (id<=1) {
            result.kind=OperandKind::Literal; result.type={1,{}};
            result.constant.width=1; result.constant.limbs={id};
        } else if (id&1u) {
            // Emit De Morgan ORs directly so shared exclusion prefixes are
            // explicit in BEIR/RTL rather than a NOT-of-AND-of-NOT network.
            const auto node=nodes_[id/2];
            if (node.conjunction) {
                const auto a=emit(node.a^1u), b=emit(node.b^1u);
                result=structure_detail::emit(program_,OperationKind::Binary,OpCode::BitOr,{1,{}},{a,b},debug_);
            } else {
                const auto value=emit(id^1u);
                result=structure_detail::emit(program_,OperationKind::Unary,OpCode::LogicNot,{1,{}},{value},debug_);
            }
        } else if (exclusion_roots_.count(id)) {
            const auto prefix=emit(id^1u);
            result=structure_detail::emit(program_,OperationKind::Unary,OpCode::LogicNot,{1,{}},{prefix},debug_);
        } else {
            const auto node=nodes_[id/2];
            if (node.conjunction) {
                const auto a=emit(node.a), b=emit(node.b);
                result=structure_detail::emit(program_,OperationKind::Binary,OpCode::BitAnd,{1,{}},{a,b},debug_);
            } else if (node.bit>=0) {
                Operation op; op.kind=OperationKind::BitSelect; op.type={1,{}};
                op.bit=node.bit; op.operands={node.atom}; op.debug=debug_;
                result=width_detail::appendTemp(program_,op.type,std::move(op),"case guard selector bit");
            } else result=node.atom;
        }
        emitted_[id]=result; return result;
    }
public:
    Guards(Program& program,unsigned limit):program_(program),limit_(limit) {}
    void runCase(Operation& op) {
        debug_=op.debug;
        addDebugMessage(debug_,"case guard cofactored under retained path facts; balanced exclusion prefixes");
        std::vector<Id> bases;
        for (std::size_t i=0;i<caseBranchCount(op);++i)
            bases.push_back(simplify(convert(op.operands[2*i])));
        std::vector<Id> results;
        for (std::size_t i=0;i<bases.size();++i) {
            // For native ordered Case as well as flattened muxes: earlier
            // *unmasked* conditions form a balanced, shared prefix OR.
            const Id prefix=tree(bases,0,i,true);
            results.push_back(simplify(land(bases[i],prefix^1u)));
        }
        for (std::size_t i=0;i<results.size();++i) op.operands[2*i]=emit(results[i]);
    }
};
} // namespace case_guard_detail

// Run once after the generic fixed point: generic associative/CSE rewrites
// must not reintroduce serial prefix sharing after this final construction.
inline bool simplifyCaseGuards(MutableProgram& graph,unsigned max_terms=4096) {
    auto& program=graph.program();
    case_guard_detail::Guards guards(program,std::min(max_terms,16384u));
    const auto live=structure_detail::reachable(graph);
    std::vector<std::pair<NodeId,Operation>> replacements;
    const auto count=program.signals.size();
    for (NodeId id=0;id<count;++id) {
        if (!live.count(id) || !program.signal(id).driver ||
            program.signal(id).driver->kind!=OperationKind::Case) continue;
        auto op=*program.signal(id).driver;
        guards.runCase(op);
        replacements.emplace_back(id,std::move(op));
    }
    for (auto& entry:replacements) program.signal(entry.first).driver=std::move(entry.second);
    if (!replacements.empty()) graph.markValueFactsDirty();
    return !replacements.empty();
}
} // namespace pred::beir::opt
