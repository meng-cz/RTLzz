#include "s7flatten/S7Flatten.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s7flatten {
namespace {

using namespace pred::s3statementize;
using namespace pred::s4cfg;
using namespace pred::s6inline;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s7flatten";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

std::string canonicalName(std::string name) {
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

std::string typeLabel(const TypeInfo& type) {
    std::string out = !type.struct_name.empty() ? type.struct_name :
        (!type.name.empty() ? type.name : type.hw_kind);
    if (out.empty()) out = "<anon>";
    if (type.is_array) {
        for (int dim : type.array_dims) out += "[" + std::to_string(dim) + "]";
        if (type.array_dims.empty()) out += "[" + std::to_string(type.array_size) + "]";
    }
    return out;
}

bool isScalarType(const TypeInfo& type) {
    if (type.is_pointer || type.is_reference || type.is_array ||
        !type.struct_name.empty()) {
        return false;
    }
    if (type.name == "bool" || type.hw_kind == "bool") return true;
    if (type.width <= 0) return false;
    if (type.hw_kind == "Int" || type.hw_kind == "UInt" ||
        type.hw_kind == "builtin") {
        return true;
    }
    if (type.name.rfind("Int<", 0) == 0 ||
        type.name.rfind("UInt<", 0) == 0 ||
        type.name == "int" || type.name == "unsigned int" ||
        type.name == "uint8_t" || type.name == "int8_t" ||
        type.name == "uint16_t" || type.name == "int16_t" ||
        type.name == "uint32_t" || type.name == "int32_t" ||
        type.name == "uint64_t" || type.name == "int64_t") {
        return true;
    }
    return false;
}

int arrayExtent(const TypeInfo& type) {
    if (!type.is_array) return 0;
    if (!type.array_dims.empty()) return type.array_dims.front();
    return type.array_size;
}

TypeInfo arrayElement(TypeInfo type) {
    if (!type.is_array) return type;
    if (!type.array_dims.empty()) {
        type.array_dims.erase(type.array_dims.begin());
        if (type.array_dims.empty()) {
            type.is_array = false;
            type.array_size = 0;
        } else {
            type.array_size = type.array_dims.front();
        }
        return type;
    }
    type.is_array = false;
    type.array_size = 0;
    return type;
}

std::vector<int> arrayDimsOf(const TypeInfo& type) {
    if (!type.is_array) return {};
    if (!type.array_dims.empty()) return type.array_dims;
    if (type.array_size > 0) return {type.array_size};
    return {};
}

std::vector<int> indicesForPath(const std::vector<std::string>& path) {
    std::vector<int> indices;
    for (const auto& part : path) {
        constexpr const char* prefix = "idx_";
        if (part.rfind(prefix, 0) != 0) continue;
        try {
            std::size_t pos = 0;
            int value = std::stoi(part.substr(4), &pos, 10);
            if (pos == part.size() - 4) indices.push_back(value);
        } catch (...) {
        }
    }
    return indices;
}

std::optional<int> literalIndex(const Operand& operand) {
    if (operand.kind != OperandKind::Literal) return std::nullopt;
    try {
        std::size_t pos = 0;
        int value = std::stoi(operand.literal_value, &pos, 0);
        if (pos == operand.literal_value.size()) return value;
    } catch (...) {
    }
    return std::nullopt;
}

S7Operand literalOperand(const Operand& operand) {
    S7Operand out;
    out.kind = S7OperandKind::Literal;
    out.type = operand.type;
    out.signed_view = operand.signed_view;
    out.debug_loc = operand.debug_loc;
    out.literal_value = operand.literal_value;
    return out;
}

struct LeafInfo {
    SymbolId id = -1;
    std::string name;
    TypeInfo type;
    std::vector<std::string> path;
    DebugLoc debug_loc;
};

struct SymbolLeafMap {
    SymbolId source_symbol = -1;
    std::string source_name;
    TypeInfo source_type;
    std::vector<LeafInfo> leaves;
};

S7Operand varOperand(const LeafInfo& leaf) {
    S7Operand out;
    out.kind = S7OperandKind::Var;
    out.type = leaf.type;
    out.debug_loc = leaf.debug_loc;
    out.symbol = leaf.id;
    return out;
}

void applyUseSignedView(S7Operand& out, const Operand& operand) {
    out.signed_view = out.signed_view || operand.signed_view;
}

std::string pathName(const std::string& root, const std::vector<std::string>& path) {
    std::string out = root;
    for (const auto& part : path) out += "__" + part;
    return out;
}

std::string pathText(const std::vector<std::string>& path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i) out += ".";
        out += path[i];
    }
    return out;
}

struct LeafTemplate {
    TypeInfo type;
    std::vector<std::string> path;
};

struct DynamicSelection;

struct Selection {
    std::vector<LeafInfo> leaves;
    std::shared_ptr<DynamicSelection> dynamic;
};

struct DynamicSelection {
    S7Operand index;
    std::vector<Selection> choices;
};

struct Value {
    std::vector<S7Operand> operands;
    std::shared_ptr<DynamicSelection> dynamic;
    TypeInfo type;
    DebugLoc debug_loc;
};

struct Context {
    const InlinedFunction& input;
    FlattenedCFG output;
    const std::unordered_map<std::string, std::vector<StructFieldInfo>>& struct_fields;
    const std::unordered_map<std::string, std::vector<StructConstructorInfo>>& constructors;
    FlattenOptions options;
    FlattenSummary summary;
    std::unordered_map<SymbolId, SymbolLeafMap> maps;
    int temp_counter = 0;
};

const std::vector<StructFieldInfo>* findStructFields(
    const std::unordered_map<std::string, std::vector<StructFieldInfo>>& structs,
    const TypeInfo& type) {
    std::vector<std::string> keys;
    if (!type.struct_name.empty()) keys.push_back(type.struct_name);
    if (!type.name.empty()) keys.push_back(type.name);
    if (!type.struct_name.empty()) keys.push_back(canonicalName(type.struct_name));
    if (!type.name.empty()) keys.push_back(canonicalName(type.name));
    for (const auto& key : keys) {
        if (key.empty()) continue;
        auto it = structs.find(key);
        if (it != structs.end()) return &it->second;
        it = structs.find("struct " + key);
        if (it != structs.end()) return &it->second;
    }
    return nullptr;
}

void buildLeafTemplates(const std::unordered_map<std::string, std::vector<StructFieldInfo>>& structs,
                        const TypeInfo& type,
                        std::vector<std::string>& path,
                        std::vector<LeafTemplate>& out,
                        DebugLoc loc) {
    if (type.is_pointer || type.is_reference) {
        fail("Pointer/reference type reached S7 flatten: '" + typeLabel(type) + "'", loc);
    }
    if (isScalarType(type)) {
        out.push_back(LeafTemplate{canonicalize_bool_type(type), path});
        return;
    }
    if (type.is_array) {
        int extent = arrayExtent(type);
        if (extent <= 0) {
            fail("Dynamic or unknown-size array cannot be flattened: '" +
                 typeLabel(type) + "'", loc);
        }
        TypeInfo elem = arrayElement(type);
        for (int i = 0; i < extent; ++i) {
            path.push_back("idx_" + std::to_string(i));
            buildLeafTemplates(structs, elem, path, out, loc);
            path.pop_back();
        }
        return;
    }
    if (!type.struct_name.empty()) {
        const auto* fields = findStructFields(structs, type);
        if (!fields) {
            fail("Missing struct metadata for flattening type '" + typeLabel(type) + "'", loc);
        }
        for (const auto& field : *fields) {
            path.push_back(field.name);
            buildLeafTemplates(structs, field.type, path, out, loc);
            path.pop_back();
        }
        return;
    }
    fail("Unsupported non-scalar type reached S7 flatten: '" + typeLabel(type) + "'", loc);
}

std::vector<LeafInfo> createLeaves(Context& ctx, const SymbolInfo& source) {
    std::vector<LeafTemplate> templates;
    std::vector<std::string> path;
    buildLeafTemplates(ctx.struct_fields, source.type, path, templates, source.type.name.empty() ? DebugLoc{} : DebugLoc{});
    if (templates.empty()) fail("No leaves produced for symbol '" + source.name + "'");
    if (static_cast<int>(ctx.output.symbols.size() + templates.size()) >
        ctx.options.max_leaf_symbols) {
        fail("S7 leaf symbol limit exceeded while flattening '" + source.name + "'");
    }

    std::vector<LeafInfo> leaves;
    leaves.reserve(templates.size());
    for (const auto& one : templates) {
        S7Symbol symbol;
        symbol.id = static_cast<SymbolId>(ctx.output.symbols.size());
        symbol.debug_name = one.path.empty() ? source.name : pathName(source.name, one.path);
        symbol.type = one.type;
        symbol.role = source.is_temp ? S7SymbolRole::Temp :
            (source.is_param ? S7SymbolRole::Port : S7SymbolRole::Local);
        ctx.output.symbols.push_back(symbol);

        LeafInfo leaf;
        leaf.id = symbol.id;
        leaf.name = symbol.debug_name;
        leaf.type = symbol.type;
        leaf.path = one.path;
        leaves.push_back(std::move(leaf));
    }
    return leaves;
}

S7UnaryOp convertUnaryOp(UnaryOp op) {
    switch (op) {
    case UnaryOp::LogicalNot: return S7UnaryOp::LogicalNot;
    case UnaryOp::BitNot: return S7UnaryOp::BitNot;
    case UnaryOp::Negate: return S7UnaryOp::Negate;
    case UnaryOp::Plus: return S7UnaryOp::Plus;
    }
    fail("Unknown unary op in S7");
}

S7BinaryOp convertBinaryOp(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return S7BinaryOp::Add;
    case BinaryOp::Sub: return S7BinaryOp::Sub;
    case BinaryOp::Mul: return S7BinaryOp::Mul;
    case BinaryOp::Div: return S7BinaryOp::Div;
    case BinaryOp::Mod: return S7BinaryOp::Mod;
    case BinaryOp::Shl: return S7BinaryOp::Shl;
    case BinaryOp::Shr: return S7BinaryOp::Shr;
    case BinaryOp::BitAnd: return S7BinaryOp::BitAnd;
    case BinaryOp::BitOr: return S7BinaryOp::BitOr;
    case BinaryOp::BitXor: return S7BinaryOp::BitXor;
    case BinaryOp::LogicalAnd: return S7BinaryOp::LogicalAnd;
    case BinaryOp::LogicalOr: return S7BinaryOp::LogicalOr;
    case BinaryOp::Eq: return S7BinaryOp::Eq;
    case BinaryOp::Ne: return S7BinaryOp::Ne;
    case BinaryOp::Lt: return S7BinaryOp::Lt;
    case BinaryOp::Le: return S7BinaryOp::Le;
    case BinaryOp::Gt: return S7BinaryOp::Gt;
    case BinaryOp::Ge: return S7BinaryOp::Ge;
    }
    fail("Unknown binary op in S7");
}

S7HardwareOp convertHardwareOp(HardwareOp op) {
    switch (op) {
    case HardwareOp::ZExt: return S7HardwareOp::ZExt;
    case HardwareOp::SExt: return S7HardwareOp::SExt;
    case HardwareOp::Trunc: return S7HardwareOp::Trunc;
    case HardwareOp::Slice: return S7HardwareOp::Slice;
    case HardwareOp::BitSelect: return S7HardwareOp::BitSelect;
    case HardwareOp::DynamicSlice: return S7HardwareOp::DynamicSlice;
    case HardwareOp::DynamicBitSelect: return S7HardwareOp::DynamicBitSelect;
    case HardwareOp::WriteSlice: return S7HardwareOp::WriteSlice;
    case HardwareOp::WriteBit: return S7HardwareOp::WriteBit;
    case HardwareOp::DynamicWriteSlice: return S7HardwareOp::DynamicWriteSlice;
    case HardwareOp::DynamicWriteBit: return S7HardwareOp::DynamicWriteBit;
    case HardwareOp::Concat: return S7HardwareOp::Concat;
    case HardwareOp::Repeat: return S7HardwareOp::Repeat;
    case HardwareOp::ReduceOr: return S7HardwareOp::ReduceOr;
    case HardwareOp::ReduceAnd: return S7HardwareOp::ReduceAnd;
    case HardwareOp::ReduceXor: return S7HardwareOp::ReduceXor;
    }
    fail("Unknown hardware op in S7");
}

S7OpKind convertOpKind(OpExpr::Kind kind) {
    switch (kind) {
    case OpExpr::Kind::Unary: return S7OpKind::Unary;
    case OpExpr::Kind::Binary: return S7OpKind::Binary;
    case OpExpr::Kind::Ternary: return S7OpKind::Ternary;
    case OpExpr::Kind::Cast: return S7OpKind::Cast;
    case OpExpr::Kind::Hardware: return S7OpKind::Hardware;
    }
    fail("Unknown op kind in S7");
}

S7TermKind convertTermKind(TermKind kind) {
    switch (kind) {
    case TermKind::Jump: return S7TermKind::Jump;
    case TermKind::Branch: return S7TermKind::Branch;
    case TermKind::Switch: return S7TermKind::Switch;
    case TermKind::Exit: return S7TermKind::Exit;
    case TermKind::Unreachable: return S7TermKind::Unreachable;
    case TermKind::Return:
        return S7TermKind::Exit;
    }
    fail("Unknown terminator kind in S7");
}

const SymbolLeafMap& leafMap(const Context& ctx, SymbolId source) {
    auto it = ctx.maps.find(source);
    if (it == ctx.maps.end()) fail("Missing leaf map for source symbol");
    return it->second;
}

bool hasPrefix(const std::vector<std::string>& path,
               const std::vector<std::string>& prefix) {
    if (prefix.size() > path.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (path[i] != prefix[i]) return false;
    }
    return true;
}

std::vector<LeafInfo> filterLeaves(const std::vector<LeafInfo>& leaves,
                                   const std::vector<std::string>& prefix) {
    std::vector<LeafInfo> out;
    for (const auto& leaf : leaves) {
        if (hasPrefix(leaf.path, prefix)) out.push_back(leaf);
    }
    return out;
}

std::optional<int> indexTokenValue(const std::string& token) {
    const std::string prefix = "idx_";
    if (token.rfind(prefix, 0) != 0) return std::nullopt;
    try {
        return std::stoi(token.substr(prefix.size()));
    } catch (...) {
        return std::nullopt;
    }
}

S7Operand flattenOperand(Context& ctx,
                           const Operand& operand,
                           std::vector<S7Stmt>& out);

Selection resolveAccesses(Context& ctx,
                          const std::vector<LeafInfo>& leaves,
                          const std::vector<LValueAccess>& accesses,
                          std::size_t access_i,
                          std::vector<std::string> prefix,
                          DebugLoc loc,
                          std::vector<S7Stmt>& out) {
    for (; access_i < accesses.size(); ++access_i) {
        const auto& access = accesses[access_i];
        if (access.kind == LValueAccessKind::Field) {
            prefix.push_back(access.field);
            continue;
        }
        if (!access.index) fail("Array index access missing operand", loc);
        if (auto static_index = literalIndex(*access.index)) {
            prefix.push_back("idx_" + std::to_string(*static_index));
            continue;
        }

        S7Operand index = flattenOperand(ctx, *access.index, out);
        std::vector<LeafInfo> candidates = filterLeaves(leaves, prefix);
        if (candidates.empty()) fail("Dynamic index has no candidate leaves", loc);
        int max_index = -1;
        for (const auto& leaf : candidates) {
            if (leaf.path.size() <= prefix.size()) continue;
            auto parsed = indexTokenValue(leaf.path[prefix.size()]);
            if (parsed) max_index = std::max(max_index, *parsed);
        }
        if (max_index < 0) fail("Dynamic index does not select an array", loc);

        auto dyn = std::make_shared<DynamicSelection>();
        dyn->index = std::move(index);
        dyn->choices.reserve(static_cast<std::size_t>(max_index + 1));
        for (int i = 0; i <= max_index; ++i) {
            std::vector<std::string> group_prefix = prefix;
            group_prefix.push_back("idx_" + std::to_string(i));
            dyn->choices.push_back(resolveAccesses(ctx, leaves, accesses,
                                                   access_i + 1,
                                                   std::move(group_prefix),
                                                   loc, out));
        }
        if (dyn->choices.empty()) {
            fail("Dynamic index produced no selected leaves", loc);
        }
        return Selection{{}, std::move(dyn)};
    }
    Selection out_sel;
    out_sel.leaves = filterLeaves(leaves, prefix);
    if (out_sel.leaves.empty()) {
        fail("LValue path '" + pathText(prefix) +
             "' did not resolve to any scalar leaves", loc);
    }
    return out_sel;
}

std::size_t selectionWidth(const Selection& selection);

std::size_t selectionWidth(const Selection& selection) {
    if (!selection.dynamic) return selection.leaves.size();
    if (selection.dynamic->choices.empty()) fail("Dynamic selection has no choices");
    std::size_t width = selectionWidth(selection.dynamic->choices.front());
    if (width == 0) fail("Dynamic selection produced no leaves");
    for (const auto& choice : selection.dynamic->choices) {
        if (selectionWidth(choice) != width) {
            fail("Dynamic index groups have inconsistent aggregate shape");
        }
    }
    return width;
}

std::vector<LeafInfo> allSelectionLeaves(const Selection& selection) {
    if (!selection.dynamic) return selection.leaves;
    std::vector<LeafInfo> out;
    for (const auto& choice : selection.dynamic->choices) {
        auto child = allSelectionLeaves(choice);
        out.insert(out.end(), child.begin(), child.end());
    }
    return out;
}

bool hasNestedDynamic(const DynamicSelection& dyn) {
    for (const auto& choice : dyn.choices) {
        if (choice.dynamic) return true;
    }
    return false;
}

Selection resolveLValue(Context& ctx,
                        const LValue& lv,
                        std::vector<S7Stmt>& out) {
    const auto& map = leafMap(ctx, lv.root_symbol);
    auto selection = resolveAccesses(ctx, map.leaves, lv.accesses, 0, {},
                                     lv.debug_loc, out);
    (void)selectionWidth(selection);
    return selection;
}

S7Stmt makeAssign(const LeafInfo& leaf, S7Operand value, DebugLoc loc = {}) {
    S7Stmt stmt;
    stmt.kind = S7StmtKind::Assign;
    stmt.debug_loc = std::move(loc);
    stmt.target = leaf.id;
    stmt.value = std::move(value);
    return stmt;
}

S7Stmt makeOp(const LeafInfo& leaf, S7Operation op, DebugLoc loc = {}) {
    S7Stmt stmt;
    stmt.kind = S7StmtKind::Op;
    stmt.debug_loc = std::move(loc);
    stmt.target = leaf.id;
    stmt.op = std::move(op);
    return stmt;
}

LeafInfo createTemp(Context& ctx, TypeInfo type, const std::string& hint) {
    if (static_cast<int>(ctx.output.symbols.size() + 1) > ctx.options.max_leaf_symbols) {
        fail("S7 leaf symbol limit exceeded while creating temporary");
    }
    S7Symbol symbol;
    symbol.id = static_cast<SymbolId>(ctx.output.symbols.size());
    symbol.debug_name = "__s7_flatten_" + hint + "_" + std::to_string(ctx.temp_counter++);
    symbol.type = canonicalize_bool_type(std::move(type));
    symbol.role = S7SymbolRole::Temp;
    ctx.output.symbols.push_back(symbol);

    LeafInfo leaf;
    leaf.id = symbol.id;
    leaf.name = symbol.debug_name;
    leaf.type = symbol.type;
    return leaf;
}

S7Stmt makeLookup(const LeafInfo& target,
                            S7Operand index,
                            std::vector<S7Operand> elements,
                            DebugLoc loc = {}) {
    S7Stmt stmt;
    stmt.kind = S7StmtKind::Lookup;
    stmt.debug_loc = std::move(loc);
    stmt.target = target.id;
    stmt.lookup_index = std::move(index);
    stmt.lookup_elements = std::move(elements);
    return stmt;
}

S7Stmt makeLookupWrite(std::vector<LeafInfo> targets,
                                 S7Operand index,
                                 S7Operand value,
                                 std::vector<S7Operand> old_values,
                                 DebugLoc loc = {}) {
    S7Stmt stmt;
    stmt.kind = S7StmtKind::LookupWrite;
    stmt.debug_loc = std::move(loc);
    stmt.lookup_index = std::move(index);
    stmt.lookup_value = std::move(value);
    stmt.lookup_elements = std::move(old_values);
    for (const auto& leaf : targets) {
        stmt.lookup_write_targets.push_back(leaf.id);
    }
    return stmt;
}

S7Operand materializeSelectionRead(Context& ctx,
                                     const Selection& selection,
                                     std::size_t leaf_index,
                                     std::vector<S7Stmt>& out,
                                     DebugLoc loc);

S7Operand materializeDynamicRead(Context& ctx,
                                   const DynamicSelection& dyn,
                                   std::size_t leaf_index,
                                   std::vector<S7Stmt>& out,
                                   DebugLoc loc) {
    std::vector<S7Operand> elements;
    elements.reserve(dyn.choices.size());
    TypeInfo type;
    for (const auto& choice : dyn.choices) {
        auto elem = materializeSelectionRead(ctx, choice, leaf_index, out, loc);
        type = elem.type;
        elements.push_back(std::move(elem));
    }
    LeafInfo temp = createTemp(ctx, type, "lookup");
    out.push_back(makeLookup(temp, dyn.index, std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
    return varOperand(temp);
}

void materializeDynamicReadInto(Context& ctx,
                                const DynamicSelection& dyn,
                                std::size_t leaf_index,
                                const LeafInfo& target,
                                std::vector<S7Stmt>& out,
                                DebugLoc loc) {
    std::vector<S7Operand> elements;
    elements.reserve(dyn.choices.size());
    for (const auto& choice : dyn.choices) {
        elements.push_back(materializeSelectionRead(ctx, choice, leaf_index, out, loc));
    }
    out.push_back(makeLookup(target, dyn.index, std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
}

S7Operand materializeSelectionRead(Context& ctx,
                                     const Selection& selection,
                                     std::size_t leaf_index,
                                     std::vector<S7Stmt>& out,
                                     DebugLoc loc) {
    if (selection.dynamic) {
        return materializeDynamicRead(ctx, *selection.dynamic, leaf_index, out, loc);
    }
    if (leaf_index >= selection.leaves.size()) {
        fail("Dynamic read leaf index out of range", loc);
    }
    return varOperand(selection.leaves[leaf_index]);
}

Value flattenValue(Context& ctx, const Operand& operand, std::vector<S7Stmt>& out) {
    Value value;
    value.type = operand.type;
    value.debug_loc = operand.debug_loc;
    switch (operand.kind) {
    case OperandKind::Literal:
        value.operands.push_back(literalOperand(operand));
        return value;
    case OperandKind::Var: {
        const auto& map = leafMap(ctx, operand.var_symbol);
        for (const auto& leaf : map.leaves) value.operands.push_back(varOperand(leaf));
        if (value.operands.size() == 1) applyUseSignedView(value.operands.front(), operand);
        return value;
    }
    case OperandKind::LValueRead: {
        Selection selection = resolveLValue(ctx, operand.lvalue, out);
        if (selection.dynamic) {
            value.dynamic = std::move(selection.dynamic);
            return value;
        }
        for (const auto& leaf : selection.leaves) value.operands.push_back(varOperand(leaf));
        if (value.operands.size() == 1) applyUseSignedView(value.operands.front(), operand);
        return value;
    }
    }
    return value;
}

S7Operand flattenOperand(Context& ctx,
                           const Operand& operand,
                           std::vector<S7Stmt>& out) {
    Value value = flattenValue(ctx, operand, out);
    if (value.dynamic) {
        S7Operand result = materializeDynamicRead(ctx, *value.dynamic, 0, out,
                                                  operand.debug_loc);
        applyUseSignedView(result, operand);
        return result;
    }
    if (value.operands.size() != 1) {
        fail("Expected scalar operand after flattening, got " +
             std::to_string(value.operands.size()) + " leaves", operand.debug_loc);
    }
    return value.operands.front();
}

std::vector<S7Operand> flattenOpOperands(Context& ctx,
                                           const std::vector<Operand>& operands,
                                           std::vector<S7Stmt>& out,
                                           DebugLoc loc) {
    std::vector<S7Operand> flat;
    flat.reserve(operands.size());
    for (const auto& operand : operands) {
        Value value = flattenValue(ctx, operand, out);
        if (value.dynamic) {
            S7Operand result = materializeDynamicRead(ctx, *value.dynamic, 0, out, loc);
            applyUseSignedView(result, operand);
            flat.push_back(std::move(result));
            continue;
        }
        if (value.operands.size() != 1) {
            fail("Operation operand is aggregate after flattening", loc);
        }
        flat.push_back(std::move(value.operands.front()));
    }
    return flat;
}

std::vector<LeafInfo> createTempLeavesLike(Context& ctx,
                                           const std::vector<LeafInfo>& source,
                                           const std::string& hint,
                                           std::vector<S7Stmt>& out,
                                           DebugLoc loc) {
    std::vector<LeafInfo> temps;
    temps.reserve(source.size());
    for (const auto& leaf : source) {
        LeafInfo temp = createTemp(ctx, leaf.type, hint);
        temps.push_back(std::move(temp));
    }
    return temps;
}

S7Operand makeLookupTemp(Context& ctx,
                           S7Operand index,
                           std::vector<S7Operand> elements,
                           DebugLoc loc,
                           std::vector<S7Stmt>& out) {
    TypeInfo type = elements.empty() ? TypeInfo{} : elements.front().type;
    LeafInfo temp = createTemp(ctx, type, "lookup");
    out.push_back(makeLookup(temp, std::move(index), std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
    return varOperand(temp);
}

std::vector<S7Operand> applySelectionWrite(Context& ctx,
                                             const Selection& selection,
                                             const std::vector<S7Operand>& values,
                                             bool actual_targets,
                                             std::vector<S7Stmt>& out,
                                             DebugLoc loc) {
    if (!selection.dynamic) {
        if (selection.leaves.size() != values.size()) {
            fail("Dynamic write value leaf count does not match target shape", loc);
        }
        if (actual_targets) {
            for (std::size_t i = 0; i < selection.leaves.size(); ++i) {
                out.push_back(makeAssign(selection.leaves[i], values[i], loc));
            }
            std::vector<S7Operand> result;
            result.reserve(selection.leaves.size());
            for (const auto& leaf : selection.leaves) result.push_back(varOperand(leaf));
            return result;
        }
        return values;
    }

    const auto& dyn = *selection.dynamic;
    std::size_t width = selectionWidth(selection);
    if (values.size() != width) {
        fail("Dynamic write value leaf count does not match target shape", loc);
    }
    if (dyn.choices.empty()) fail("Dynamic selection has no choices", loc);

    if (!hasNestedDynamic(dyn)) {
        std::size_t choice_count = dyn.choices.size();
        std::vector<S7Operand> result(choice_count * width);
        for (std::size_t leaf_i = 0; leaf_i < width; ++leaf_i) {
            std::vector<LeafInfo> old_targets;
            old_targets.reserve(choice_count);
            std::vector<S7Operand> old_values;
            old_values.reserve(choice_count);
            for (const auto& choice : dyn.choices) {
                if (leaf_i >= choice.leaves.size()) {
                    fail("Dynamic write leaf index out of range", loc);
                }
                old_targets.push_back(choice.leaves[leaf_i]);
                old_values.push_back(varOperand(choice.leaves[leaf_i]));
            }
            std::vector<LeafInfo> targets = actual_targets
                ? old_targets
                : createTempLeavesLike(ctx, old_targets, "lookupwrite", out, loc);
            out.push_back(makeLookupWrite(targets, dyn.index, values[leaf_i],
                                          std::move(old_values), loc));
            ++ctx.summary.dynamic_writes;
            for (std::size_t choice_i = 0; choice_i < choice_count; ++choice_i) {
                result[choice_i * width + leaf_i] = varOperand(targets[choice_i]);
            }
        }
        return result;
    }

    std::vector<std::vector<S7Operand>> candidates;
    std::vector<std::vector<LeafInfo>> old_leaf_groups;
    candidates.reserve(dyn.choices.size());
    old_leaf_groups.reserve(dyn.choices.size());
    std::size_t child_leaf_count = 0;
    bool have_child_leaf_count = false;
    for (const auto& choice : dyn.choices) {
        auto updated = applySelectionWrite(ctx, choice, values, false, out, loc);
        auto old_leaves = allSelectionLeaves(choice);
        if (updated.size() != old_leaves.size()) {
            fail("Nested dynamic write produced inconsistent candidate shape", loc);
        }
        if (!have_child_leaf_count) {
            child_leaf_count = updated.size();
            have_child_leaf_count = true;
        } else if (updated.size() != child_leaf_count) {
            fail("Nested dynamic write choices have inconsistent aggregate shape", loc);
        }
        candidates.push_back(std::move(updated));
        old_leaf_groups.push_back(std::move(old_leaves));
    }

    std::vector<S7Operand> result(dyn.choices.size() * child_leaf_count);
    for (std::size_t leaf_i = 0; leaf_i < child_leaf_count; ++leaf_i) {
        std::vector<S7Operand> candidate_values;
        candidate_values.reserve(candidates.size());
        std::vector<LeafInfo> old_targets;
        old_targets.reserve(old_leaf_groups.size());
        std::vector<S7Operand> old_values;
        old_values.reserve(old_leaf_groups.size());
        for (std::size_t choice_i = 0; choice_i < candidates.size(); ++choice_i) {
            candidate_values.push_back(candidates[choice_i][leaf_i]);
            old_targets.push_back(old_leaf_groups[choice_i][leaf_i]);
            old_values.push_back(varOperand(old_leaf_groups[choice_i][leaf_i]));
        }
        S7Operand selected_value = makeLookupTemp(ctx, dyn.index, std::move(candidate_values),
                                                    loc, out);
        std::vector<LeafInfo> targets = actual_targets
            ? old_targets
            : createTempLeavesLike(ctx, old_targets, "lookupwrite", out, loc);
        out.push_back(makeLookupWrite(targets, dyn.index, std::move(selected_value),
                                      std::move(old_values), loc));
        ++ctx.summary.dynamic_writes;
        for (std::size_t choice_i = 0; choice_i < targets.size(); ++choice_i) {
            result[choice_i * child_leaf_count + leaf_i] = varOperand(targets[choice_i]);
        }
    }
    return result;
}

void lowerAssign(Context& ctx,
                 const LValue& target,
                 const Operand& value,
                 std::vector<S7Stmt>& out,
                 DebugLoc loc) {
    Selection lhs = resolveLValue(ctx, target, out);
    Value rhs = flattenValue(ctx, value, out);
    if (lhs.dynamic) {
        std::size_t width = selectionWidth(lhs);
        std::vector<S7Operand> values;
        values.reserve(width);
        if (rhs.dynamic) {
            for (std::size_t i = 0; i < width; ++i) {
                values.push_back(materializeDynamicRead(ctx, *rhs.dynamic, i, out, loc));
            }
        } else {
            values = rhs.operands;
        }
        (void)applySelectionWrite(ctx, lhs, values, true, out, loc);
        return;
    }
    if (rhs.dynamic) {
        std::size_t width = selectionWidth(Selection{{}, rhs.dynamic});
        if (lhs.leaves.size() != width) {
            fail("Dynamic read leaf count does not match assignment target shape", loc);
        }
        for (std::size_t i = 0; i < width; ++i) {
            materializeDynamicReadInto(ctx, *rhs.dynamic, i, lhs.leaves[i], out, loc);
        }
        return;
    }
    if (lhs.leaves.size() != rhs.operands.size()) {
        fail("Assignment leaf count mismatch: lhs=" + std::to_string(lhs.leaves.size()) +
             " rhs=" + std::to_string(rhs.operands.size()), loc);
    }
    for (std::size_t i = 0; i < lhs.leaves.size(); ++i) {
        out.push_back(makeAssign(lhs.leaves[i], rhs.operands[i], loc));
    }
}

void lowerOp(Context& ctx,
             const S3Stmt& stmt,
             std::vector<S7Stmt>& out) {
    Selection target = resolveLValue(ctx, stmt.target, out);
    if (stmt.op.kind == OpExpr::Kind::Ternary && target.leaves.size() > 1) {
        if (stmt.op.operands.size() != 3) fail("Malformed ternary op", stmt.debug_loc);
        S7Operand cond = flattenOperand(ctx, stmt.op.operands[0], out);
        Value then_value = flattenValue(ctx, stmt.op.operands[1], out);
        Value else_value = flattenValue(ctx, stmt.op.operands[2], out);
        if (then_value.dynamic || else_value.dynamic) {
            fail("Aggregate ternary with dynamic array read is unsupported", stmt.debug_loc);
        }
        if (then_value.operands.size() != target.leaves.size() ||
            else_value.operands.size() != target.leaves.size()) {
            fail("Aggregate ternary shape mismatch", stmt.debug_loc);
        }
        for (std::size_t i = 0; i < target.leaves.size(); ++i) {
            S7Operation op;
            op.kind = S7OpKind::Ternary;
            op.debug_loc = stmt.op.debug_loc;
            op.operands.push_back(cond);
            op.operands.push_back(then_value.operands[i]);
            op.operands.push_back(else_value.operands[i]);
            out.push_back(makeOp(target.leaves[i], std::move(op), stmt.debug_loc));
        }
        return;
    }
    if (target.dynamic) fail("Operation target may not be a dynamic array element", stmt.debug_loc);
    if (target.leaves.size() != 1) fail("Non-ternary operation target is aggregate", stmt.debug_loc);
    S7Operation op;
    op.kind = convertOpKind(stmt.op.kind);
    op.debug_loc = stmt.op.debug_loc;
    op.unary_op = convertUnaryOp(stmt.op.unary_op);
    op.binary_op = convertBinaryOp(stmt.op.binary_op);
    op.hardware_op = convertHardwareOp(stmt.op.hardware_op);
    op.cast_type = stmt.op.cast_type;
    op.hi = stmt.op.hi;
    op.lo = stmt.op.lo;
    op.bit = stmt.op.bit;
    op.times = stmt.op.times;
    op.to_width = stmt.op.to_width;
    op.operands = flattenOpOperands(ctx, stmt.op.operands, out, stmt.debug_loc);
    out.push_back(makeOp(target.leaves.front(), std::move(op), stmt.debug_loc));
}

std::optional<std::vector<S7Operand>> argsByConstructorMetadata(
    Context& ctx,
    const S3Stmt& stmt,
    const std::vector<LeafInfo>& target_leaves,
    std::vector<S7Stmt>& out) {
    auto it = ctx.constructors.find(stmt.callee);
    if (it == ctx.constructors.end()) {
        it = ctx.constructors.find(canonicalName(stmt.callee));
    }
    if (it == ctx.constructors.end() || it->second.empty()) return std::nullopt;
    const StructConstructorInfo* matched = nullptr;
    for (const auto& candidate : it->second) {
        if (candidate.param_names.size() == stmt.args.size()) {
            matched = &candidate;
            break;
        }
    }
    if (!matched) return std::nullopt;
    std::unordered_map<std::string, S7Operand> by_param;
    for (std::size_t i = 0; i < matched->param_names.size(); ++i) {
        by_param[matched->param_names[i]] = flattenOperand(ctx, stmt.args[i], out);
    }
    std::vector<S7Operand> values;
    values.reserve(target_leaves.size());
    for (const auto& leaf : target_leaves) {
        if (leaf.path.empty()) return std::nullopt;
        const std::string& field = leaf.path.front();
        auto map_it = matched->field_to_param.find(field);
        if (map_it == matched->field_to_param.end()) {
            fail("Constructor metadata does not bind field '" + field + "'", stmt.debug_loc);
        }
        auto param_it = by_param.find(map_it->second);
        if (param_it == by_param.end()) {
            fail("Constructor metadata references missing parameter '" +
                 map_it->second + "'", stmt.debug_loc);
        }
        values.push_back(param_it->second);
    }
    return values;
}

void lowerConstruct(Context& ctx,
                    const S3Stmt& stmt,
                    std::vector<S7Stmt>& out) {
    Selection target = resolveLValue(ctx, stmt.target, out);
    if (target.dynamic) fail("Construct target may not be dynamic array element", stmt.debug_loc);
    if (target.leaves.empty()) return;
    if (target.leaves.size() == 1) {
        if (stmt.args.empty()) return;
        if (stmt.args.size() != 1) {
            fail("Scalar construct expects at most one value argument", stmt.debug_loc);
        }
        out.push_back(makeAssign(target.leaves.front(),
                                 flattenOperand(ctx, stmt.args.front(), out),
                                 stmt.debug_loc));
        return;
    }
    if (stmt.args.empty()) return;
    if (auto metadata_values = argsByConstructorMetadata(ctx, stmt, target.leaves, out)) {
        for (std::size_t i = 0; i < target.leaves.size(); ++i) {
            out.push_back(makeAssign(target.leaves[i], metadata_values->at(i), stmt.debug_loc));
        }
        return;
    }
    std::vector<S7Operand> values;
    for (const auto& arg : stmt.args) {
        Value v = flattenValue(ctx, arg, out);
        if (v.dynamic) fail("Constructor argument may not be dynamic aggregate read", stmt.debug_loc);
        values.insert(values.end(), v.operands.begin(), v.operands.end());
    }
    if (values.size() != target.leaves.size()) {
        fail("Aggregate construct leaf count mismatch for '" + stmt.callee + "'", stmt.debug_loc);
    }
    for (std::size_t i = 0; i < target.leaves.size(); ++i) {
        out.push_back(makeAssign(target.leaves[i], values[i], stmt.debug_loc));
    }
}

void lowerStmt(Context& ctx,
               const CFGStmt& cfg_stmt,
               std::vector<S7Stmt>& out) {
    if (!cfg_stmt.stmt) return;
    const S3Stmt& stmt = *cfg_stmt.stmt;
    switch (stmt.kind) {
    case S3StmtKind::Decl:
        return;
    case S3StmtKind::Assign:
        lowerAssign(ctx, stmt.target, stmt.value, out, stmt.debug_loc);
        return;
    case S3StmtKind::Op:
        lowerOp(ctx, stmt, out);
        return;
    case S3StmtKind::Construct:
        lowerConstruct(ctx, stmt, out);
        return;
    case S3StmtKind::Eval:
        return;
    case S3StmtKind::Call:
        fail("S7 expects all calls to be inlined before flatten", stmt.debug_loc);
    default:
        fail("Control statement reached flattened basic block body", stmt.debug_loc);
    }
}

S7Terminator flattenTerminator(Context& ctx,
                                      const Terminator& term,
                                      std::vector<S7Stmt>& prelude) {
    S7Terminator out;
    out.kind = convertTermKind(term.kind);
    out.jump_target = term.jump_target;
    out.true_target = term.true_target;
    out.false_target = term.false_target;
    out.default_target = term.default_target;
    switch (term.kind) {
    case TermKind::Branch:
        out.condition = flattenOperand(ctx, term.condition, prelude);
        break;
    case TermKind::Switch:
        out.switch_value = flattenOperand(ctx, term.switch_value, prelude);
        for (const auto& target : term.switch_targets) {
            S7SwitchTarget ft;
            ft.target = target.target;
            if (target.value) ft.value = flattenOperand(ctx, target.value.value(), prelude);
            out.switch_targets.push_back(std::move(ft));
        }
        break;
    case TermKind::Return:
        if (term.return_value) fail("Return value reached S7 after S6 inline");
        break;
    case TermKind::Jump:
    case TermKind::Unreachable:
    case TermKind::Exit:
        break;
    }
    return out;
}

void buildSymbolMaps(Context& ctx) {
    ctx.output.name = ctx.input.name;
    ctx.output.entry = ctx.input.entry;
    ctx.output.exit = ctx.input.exit;

    std::unordered_set<SymbolId> param_symbols;
    for (const auto& param : ctx.input.params) {
        for (const auto& symbol : ctx.input.symbols) {
            if (symbol.name == param.name) {
                param_symbols.insert(symbol.id);
                break;
            }
        }
    }

    for (const auto& symbol : ctx.input.symbols) {
        SymbolLeafMap map;
        map.source_symbol = symbol.id;
        map.source_name = symbol.name;
        map.source_type = symbol.type;
        map.leaves = createLeaves(ctx, symbol);
        if (map.leaves.size() > 1) ++ctx.summary.aggregate_symbols;
        ctx.summary.leaf_symbols += static_cast<int>(map.leaves.size());
        if (param_symbols.count(symbol.id)) {
            ParamDirection direction = ParamDirection::Input;
            ParamPassingKind passing = ParamPassingKind::Value;
            for (const auto& param : ctx.input.params) {
                if (param.name == symbol.name) {
                    direction = param.direction;
                    passing = param.passing;
                    break;
                }
            }
            for (const auto& leaf : map.leaves) {
                if (leaf.id >= 0 && leaf.id < static_cast<SymbolId>(ctx.output.symbols.size())) {
                    ctx.output.symbols[static_cast<std::size_t>(leaf.id)].role = S7SymbolRole::Port;
                }
                S7Port port;
                port.symbol = leaf.id;
                port.direction = direction;
                port.passing = passing;
                ctx.output.ports.push_back(port);
            }
            S7PortGroup group;
            group.source_name = symbol.name;
            group.direction = direction;
            group.passing = passing;
            group.source_type = symbol.type;
            group.array_dims = arrayDimsOf(symbol.type);
            group.scalar_type = map.leaves.empty()
                ? TypeInfo{}
                : map.leaves.front().type;
            for (const auto& leaf : map.leaves) {
                S7PortElement element;
                element.symbol = leaf.id;
                element.indices = indicesForPath(leaf.path);
                group.elements.push_back(std::move(element));
            }
            ctx.output.port_groups.push_back(std::move(group));
        }
        ctx.maps[symbol.id] = std::move(map);
    }
}

FlattenedCFG flattenFunction(const InlinedFunction& fn,
                             const std::unordered_map<std::string, std::vector<StructFieldInfo>>& structs,
                             const std::unordered_map<std::string, std::vector<StructConstructorInfo>>& constructors,
                             const FlattenOptions& options,
                             FlattenSummary& summary) {
    Context ctx{fn, {}, structs, constructors, options};
    ctx.summary.function_name = fn.name;
    if (fn.return_slot || fn.return_slot_symbol >= 0) {
        fail("Return slot reached S7 after S6 inline");
    }
    buildSymbolMaps(ctx);
    for (const auto& block : fn.blocks) {
        if (!block) continue;
        S7BasicBlock out_block;
        out_block.id = block->id;
        for (const auto& stmt : block->stmts) lowerStmt(ctx, stmt, out_block.stmts);
        out_block.terminator = flattenTerminator(ctx, block->terminator, out_block.stmts);
        ctx.output.blocks.push_back(std::move(out_block));
    }
    summary = ctx.summary;
    return std::move(ctx.output);
}

const S7Symbol& symbolAt(const FlattenedCFG& fn, SymbolId id) {
    if (id < 0 || id >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("Invalid S7 symbol reference");
    }
    return fn.symbols[static_cast<std::size_t>(id)];
}

std::string symbolName(const FlattenedCFG& fn, SymbolId id) {
    return symbolAt(fn, id).debug_name;
}

std::string operandText(const FlattenedCFG& fn, const S7Operand& operand) {
    switch (operand.kind) {
    case S7OperandKind::Literal: return operand.literal_value;
    case S7OperandKind::Var: return symbolName(fn, operand.symbol);
    }
    return "<operand>";
}

std::string unaryName(S7UnaryOp op) {
    switch (op) {
    case S7UnaryOp::LogicalNot: return "LogicalNot";
    case S7UnaryOp::BitNot: return "BitNot";
    case S7UnaryOp::Negate: return "Negate";
    case S7UnaryOp::Plus: return "Plus";
    }
    return "Unary";
}

std::string binaryName(S7BinaryOp op) {
    switch (op) {
    case S7BinaryOp::Add: return "Add";
    case S7BinaryOp::Sub: return "Sub";
    case S7BinaryOp::Mul: return "Mul";
    case S7BinaryOp::Div: return "Div";
    case S7BinaryOp::Mod: return "Mod";
    case S7BinaryOp::Shl: return "Shl";
    case S7BinaryOp::Shr: return "Shr";
    case S7BinaryOp::BitAnd: return "BitAnd";
    case S7BinaryOp::BitOr: return "BitOr";
    case S7BinaryOp::BitXor: return "BitXor";
    case S7BinaryOp::LogicalAnd: return "LogicalAnd";
    case S7BinaryOp::LogicalOr: return "LogicalOr";
    case S7BinaryOp::Eq: return "Eq";
    case S7BinaryOp::Ne: return "Ne";
    case S7BinaryOp::Lt: return "Lt";
    case S7BinaryOp::Le: return "Le";
    case S7BinaryOp::Gt: return "Gt";
    case S7BinaryOp::Ge: return "Ge";
    }
    return "Binary";
}

std::string hardwareName(S7HardwareOp op) {
    switch (op) {
    case S7HardwareOp::ZExt: return "ZExt";
    case S7HardwareOp::SExt: return "SExt";
    case S7HardwareOp::Trunc: return "Trunc";
    case S7HardwareOp::Slice: return "Slice";
    case S7HardwareOp::BitSelect: return "BitSelect";
    case S7HardwareOp::DynamicSlice: return "DynamicSlice";
    case S7HardwareOp::DynamicBitSelect: return "DynamicBitSelect";
    case S7HardwareOp::WriteSlice: return "WriteSlice";
    case S7HardwareOp::WriteBit: return "WriteBit";
    case S7HardwareOp::DynamicWriteSlice: return "DynamicWriteSlice";
    case S7HardwareOp::DynamicWriteBit: return "DynamicWriteBit";
    case S7HardwareOp::Concat: return "Concat";
    case S7HardwareOp::Repeat: return "Repeat";
    case S7HardwareOp::ReduceOr: return "ReduceOr";
    case S7HardwareOp::ReduceAnd: return "ReduceAnd";
    case S7HardwareOp::ReduceXor: return "ReduceXor";
    }
    return "Hardware";
}

std::string opText(const FlattenedCFG& fn, const S7Operation& op) {
    std::ostringstream os;
    if (op.kind == S7OpKind::Unary) os << unaryName(op.unary_op);
    else if (op.kind == S7OpKind::Binary) os << binaryName(op.binary_op);
    else if (op.kind == S7OpKind::Ternary) os << "Ternary";
    else if (op.kind == S7OpKind::Cast) os << "Cast";
    else os << hardwareName(op.hardware_op);
    os << "(";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        if (i) os << ", ";
        os << operandText(fn, op.operands[i]);
    }
    os << ")";
    return os.str();
}

std::string termKindName(S7TermKind kind) {
    switch (kind) {
    case S7TermKind::Jump: return "jump";
    case S7TermKind::Branch: return "branch";
    case S7TermKind::Switch: return "switch";
    case S7TermKind::Unreachable: return "unreachable";
    case S7TermKind::Exit: return "exit";
    }
    return "term";
}

std::string stmtText(const FlattenedCFG& fn, const S7Stmt& stmt) {
    std::ostringstream os;
    switch (stmt.kind) {
    case S7StmtKind::Assign:
        os << "assign " << symbolName(fn, stmt.target) << " = "
           << operandText(fn, stmt.value);
        break;
    case S7StmtKind::Op:
        os << "op " << symbolName(fn, stmt.target) << " = " << opText(fn, stmt.op);
        break;
    case S7StmtKind::Lookup:
        os << "lookup " << symbolName(fn, stmt.target) << " = lookup("
           << operandText(fn, stmt.lookup_index);
        for (const auto& elem : stmt.lookup_elements) {
            os << ", " << operandText(fn, elem);
        }
        os << ")";
        break;
    case S7StmtKind::LookupWrite:
        os << "lookupwrite [";
        for (std::size_t i = 0; i < stmt.lookup_write_targets.size(); ++i) {
            if (i) os << ", ";
            os << symbolName(fn, stmt.lookup_write_targets[i]);
        }
        os << "] = lookupwrite(" << operandText(fn, stmt.lookup_index)
           << ", " << operandText(fn, stmt.lookup_value);
        for (const auto& elem : stmt.lookup_elements) {
            os << ", " << operandText(fn, elem);
        }
        os << ")";
        break;
    }
    return os.str();
}

} // namespace

std::string debugPrint(const S7FlattenedProgram& program,
                       const std::vector<FlattenSummary>& summaries) {
    std::ostringstream os;
    os << "s7flatten\n";
    for (const auto& summary : summaries) {
        os << "summary function=" << summary.function_name
           << " aggregate_symbols=" << summary.aggregate_symbols
           << " leaf_symbols=" << summary.leaf_symbols
           << " dynamic_reads=" << summary.dynamic_reads
           << " dynamic_writes=" << summary.dynamic_writes << "\n";
    }
    const auto& fn = program.top;
    os << "top " << fn.name << " entry=bb" << fn.entry << " exit=bb" << fn.exit << "\n";
    os << "symbols\n";
    for (const auto& symbol : fn.symbols) {
        os << "  %" << symbol.id << " " << symbol.debug_name << "\n";
    }
    os << "ports\n";
    for (const auto& port : fn.ports) {
        os << "  " << symbolName(fn, port.symbol) << "\n";
    }
    for (const auto& block : fn.blocks) {
        os << "  bb" << block.id << "\n";
        for (const auto& stmt : block.stmts) {
            os << "    " << stmtText(fn, stmt) << "\n";
        }
        os << "    term " << termKindName(block.terminator.kind);
        if (block.terminator.kind == S7TermKind::Branch) {
            os << " " << operandText(fn, block.terminator.condition)
               << " ? bb" << block.terminator.true_target
               << " : bb" << block.terminator.false_target;
        } else if (block.terminator.kind == S7TermKind::Jump) {
            os << " bb" << block.terminator.jump_target;
        } else if (block.terminator.kind == S7TermKind::Switch) {
            os << " " << operandText(fn, block.terminator.switch_value);
        }
        os << "\n";
    }
    return os.str();
}

FlattenResult flattenProgram(const InlinedCFGProgram& program,
                             const FlattenOptions& options) {
    try {
        FlattenResult result;
        FlattenSummary summary;
        S7FlattenedProgram out;
        out.top = flattenFunction(program.top, program.struct_fields,
                                  program.struct_constructors, options, summary);
        result.summaries.push_back(summary);
        if (options.debug_print) result.debug_text = debugPrint(out, result.summaries);
        result.program = std::move(out);
        return result;
    } catch (const RTLZZException& ex) {
        FlattenResult result;
        FlattenError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        FlattenResult result;
        FlattenError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

S7FlattenedProgram flattenProgramOrThrow(const InlinedCFGProgram& program,
                                       const FlattenOptions& options) {
    auto result = flattenProgram(program, options);
    if (!result.ok()) {
        throw RTLZZException(result.error->context, result.error->message);
    }
    return std::move(result.program.value());
}

} // namespace pred::s7flatten
