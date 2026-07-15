#include "s7flatten/S7Flatten.h"

#include <algorithm>
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

FlatOperand unknownOperand(TypeInfo type = {}, DebugLoc loc = {}) {
    FlatOperand out;
    out.kind = FlatOperandKind::Unknown;
    out.type = std::move(type);
    out.debug_loc = std::move(loc);
    return out;
}

FlatOperand literalOperand(const Operand& operand) {
    FlatOperand out;
    out.kind = FlatOperandKind::Literal;
    out.type = operand.type;
    out.debug_loc = operand.debug_loc;
    out.literal_value = operand.literal_value;
    return out;
}

FlatOperand varOperand(const LeafInfo& leaf) {
    FlatOperand out;
    out.kind = FlatOperandKind::Var;
    out.type = leaf.type;
    out.debug_loc = leaf.debug_loc;
    out.var_name = leaf.name;
    out.var_symbol = leaf.id;
    return out;
}

FlatOperand varOperand(const SymbolInfo& symbol) {
    FlatOperand out;
    out.kind = FlatOperandKind::Var;
    out.type = symbol.type;
    out.var_name = symbol.name;
    out.var_symbol = symbol.id;
    return out;
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
    FlatOperand index;
    std::vector<Selection> choices;
};

struct Value {
    std::vector<FlatOperand> operands;
    std::shared_ptr<DynamicSelection> dynamic;
    TypeInfo type;
    DebugLoc debug_loc;
};

struct Context {
    const InlinedFunction& input;
    FlattenedFunction output;
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
        SymbolInfo symbol;
        symbol.id = static_cast<SymbolId>(ctx.output.symbols.size());
        symbol.name = one.path.empty() ? source.name : pathName(source.name, one.path);
        symbol.type = one.type;
        symbol.declaring_scope = -1;
        symbol.source_valid_scope_ids.clear();
        symbol.is_param = source.is_param;
        symbol.is_temp = source.is_temp;
        ctx.output.symbols.push_back(symbol);

        LeafInfo leaf;
        leaf.id = symbol.id;
        leaf.name = symbol.name;
        leaf.type = symbol.type;
        leaf.path = one.path;
        leaves.push_back(std::move(leaf));
    }
    return leaves;
}

const SymbolInfo& sourceSymbol(const InlinedFunction& fn, SymbolId id) {
    if (id < 0 || id >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("Invalid source symbol id in S7");
    }
    const auto& symbol = fn.symbols[static_cast<std::size_t>(id)];
    if (symbol.id != id) fail("Broken source symbol table invariant in S7");
    return symbol;
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

FlatOperand flattenOperand(Context& ctx,
                           const Operand& operand,
                           std::vector<FlattenedStmtPtr>& out);

Selection resolveAccesses(Context& ctx,
                          const std::vector<LeafInfo>& leaves,
                          const std::vector<LValueAccess>& accesses,
                          std::size_t access_i,
                          std::vector<std::string> prefix,
                          DebugLoc loc,
                          std::vector<FlattenedStmtPtr>& out) {
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

        FlatOperand index = flattenOperand(ctx, *access.index, out);
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
                        std::vector<FlattenedStmtPtr>& out) {
    const auto& map = leafMap(ctx, lv.root_symbol);
    auto selection = resolveAccesses(ctx, map.leaves, lv.accesses, 0, {},
                                     lv.debug_loc, out);
    (void)selectionWidth(selection);
    return selection;
}

FlattenedStmtPtr makeDecl(const LeafInfo& leaf, DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::Decl;
    stmt->debug_loc = std::move(loc);
    stmt->decl_name = leaf.name;
    stmt->decl_symbol = leaf.id;
    stmt->decl_type = leaf.type;
    return stmt;
}

FlattenedStmtPtr makeAssign(const LeafInfo& leaf, FlatOperand value, DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::Assign;
    stmt->debug_loc = std::move(loc);
    stmt->target_symbol = leaf.id;
    stmt->target_name = leaf.name;
    stmt->target_type = leaf.type;
    stmt->value = std::move(value);
    return stmt;
}

FlattenedStmtPtr makeAssign(SymbolId target, std::string name, TypeInfo type,
                            FlatOperand value, DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::Assign;
    stmt->debug_loc = std::move(loc);
    stmt->target_symbol = target;
    stmt->target_name = std::move(name);
    stmt->target_type = std::move(type);
    stmt->value = std::move(value);
    return stmt;
}

FlattenedStmtPtr makeOp(const LeafInfo& leaf, FlatOpExpr op, DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::Op;
    stmt->debug_loc = std::move(loc);
    stmt->target_symbol = leaf.id;
    stmt->target_name = leaf.name;
    stmt->target_type = leaf.type;
    stmt->op = std::move(op);
    return stmt;
}

LeafInfo createTemp(Context& ctx, TypeInfo type, const std::string& hint) {
    if (static_cast<int>(ctx.output.symbols.size() + 1) > ctx.options.max_leaf_symbols) {
        fail("S7 leaf symbol limit exceeded while creating temporary");
    }
    SymbolInfo symbol;
    symbol.id = static_cast<SymbolId>(ctx.output.symbols.size());
    symbol.name = "__s7_flatten_" + hint + "_" + std::to_string(ctx.temp_counter++);
    symbol.type = canonicalize_bool_type(std::move(type));
    symbol.is_temp = true;
    ctx.output.symbols.push_back(symbol);

    LeafInfo leaf;
    leaf.id = symbol.id;
    leaf.name = symbol.name;
    leaf.type = symbol.type;
    return leaf;
}

FlattenedStmtPtr makeLookup(const LeafInfo& target,
                            FlatOperand index,
                            std::vector<FlatOperand> elements,
                            DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::Lookup;
    stmt->debug_loc = std::move(loc);
    stmt->target_symbol = target.id;
    stmt->target_name = target.name;
    stmt->target_type = target.type;
    stmt->lookup_index = std::move(index);
    stmt->lookup_elements = std::move(elements);
    return stmt;
}

FlattenedStmtPtr makeLookupWrite(std::vector<LeafInfo> targets,
                                 FlatOperand index,
                                 FlatOperand value,
                                 std::vector<FlatOperand> old_values,
                                 DebugLoc loc = {}) {
    auto stmt = std::make_shared<FlattenedStmt>();
    stmt->kind = FlatStmtKind::LookupWrite;
    stmt->debug_loc = std::move(loc);
    stmt->lookup_index = std::move(index);
    stmt->lookup_value = std::move(value);
    stmt->lookup_elements = std::move(old_values);
    for (const auto& leaf : targets) {
        stmt->lookup_write_target_symbols.push_back(leaf.id);
        stmt->lookup_write_target_names.push_back(leaf.name);
    }
    return stmt;
}

FlatOperand materializeSelectionRead(Context& ctx,
                                     const Selection& selection,
                                     std::size_t leaf_index,
                                     std::vector<FlattenedStmtPtr>& out,
                                     DebugLoc loc);

FlatOperand materializeDynamicRead(Context& ctx,
                                   const DynamicSelection& dyn,
                                   std::size_t leaf_index,
                                   std::vector<FlattenedStmtPtr>& out,
                                   DebugLoc loc) {
    std::vector<FlatOperand> elements;
    elements.reserve(dyn.choices.size());
    TypeInfo type;
    for (const auto& choice : dyn.choices) {
        auto elem = materializeSelectionRead(ctx, choice, leaf_index, out, loc);
        type = elem.type;
        elements.push_back(std::move(elem));
    }
    LeafInfo temp = createTemp(ctx, type, "lookup");
    out.push_back(makeDecl(temp, loc));
    out.push_back(makeLookup(temp, dyn.index, std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
    return varOperand(temp);
}

void materializeDynamicReadInto(Context& ctx,
                                const DynamicSelection& dyn,
                                std::size_t leaf_index,
                                const LeafInfo& target,
                                std::vector<FlattenedStmtPtr>& out,
                                DebugLoc loc) {
    std::vector<FlatOperand> elements;
    elements.reserve(dyn.choices.size());
    for (const auto& choice : dyn.choices) {
        elements.push_back(materializeSelectionRead(ctx, choice, leaf_index, out, loc));
    }
    out.push_back(makeLookup(target, dyn.index, std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
}

FlatOperand materializeSelectionRead(Context& ctx,
                                     const Selection& selection,
                                     std::size_t leaf_index,
                                     std::vector<FlattenedStmtPtr>& out,
                                     DebugLoc loc) {
    if (selection.dynamic) {
        return materializeDynamicRead(ctx, *selection.dynamic, leaf_index, out, loc);
    }
    if (leaf_index >= selection.leaves.size()) {
        fail("Dynamic read leaf index out of range", loc);
    }
    return varOperand(selection.leaves[leaf_index]);
}

Value flattenValue(Context& ctx, const Operand& operand, std::vector<FlattenedStmtPtr>& out) {
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
        return value;
    }
    case OperandKind::LValueRead: {
        Selection selection = resolveLValue(ctx, operand.lvalue, out);
        if (selection.dynamic) {
            value.dynamic = std::move(selection.dynamic);
            return value;
        }
        for (const auto& leaf : selection.leaves) value.operands.push_back(varOperand(leaf));
        return value;
    }
    }
    return value;
}

FlatOperand flattenOperand(Context& ctx,
                           const Operand& operand,
                           std::vector<FlattenedStmtPtr>& out) {
    Value value = flattenValue(ctx, operand, out);
    if (value.dynamic) {
        return materializeDynamicRead(ctx, *value.dynamic, 0, out, operand.debug_loc);
    }
    if (value.operands.size() != 1) {
        fail("Expected scalar operand after flattening, got " +
             std::to_string(value.operands.size()) + " leaves", operand.debug_loc);
    }
    return value.operands.front();
}

std::vector<FlatOperand> flattenOpOperands(Context& ctx,
                                           const std::vector<Operand>& operands,
                                           std::vector<FlattenedStmtPtr>& out,
                                           DebugLoc loc) {
    std::vector<FlatOperand> flat;
    flat.reserve(operands.size());
    for (const auto& operand : operands) {
        Value value = flattenValue(ctx, operand, out);
        if (value.dynamic) {
            flat.push_back(materializeDynamicRead(ctx, *value.dynamic, 0, out, loc));
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
                                           std::vector<FlattenedStmtPtr>& out,
                                           DebugLoc loc) {
    std::vector<LeafInfo> temps;
    temps.reserve(source.size());
    for (const auto& leaf : source) {
        LeafInfo temp = createTemp(ctx, leaf.type, hint);
        out.push_back(makeDecl(temp, loc));
        temps.push_back(std::move(temp));
    }
    return temps;
}

FlatOperand makeLookupTemp(Context& ctx,
                           FlatOperand index,
                           std::vector<FlatOperand> elements,
                           DebugLoc loc,
                           std::vector<FlattenedStmtPtr>& out) {
    TypeInfo type = elements.empty() ? TypeInfo{} : elements.front().type;
    LeafInfo temp = createTemp(ctx, type, "lookup");
    out.push_back(makeDecl(temp, loc));
    out.push_back(makeLookup(temp, std::move(index), std::move(elements), loc));
    ++ctx.summary.dynamic_reads;
    return varOperand(temp);
}

std::vector<FlatOperand> applySelectionWrite(Context& ctx,
                                             const Selection& selection,
                                             const std::vector<FlatOperand>& values,
                                             bool actual_targets,
                                             std::vector<FlattenedStmtPtr>& out,
                                             DebugLoc loc) {
    if (!selection.dynamic) {
        if (selection.leaves.size() != values.size()) {
            fail("Dynamic write value leaf count does not match target shape", loc);
        }
        if (actual_targets) {
            for (std::size_t i = 0; i < selection.leaves.size(); ++i) {
                out.push_back(makeAssign(selection.leaves[i], values[i], loc));
            }
            std::vector<FlatOperand> result;
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
        std::vector<FlatOperand> result(choice_count * width);
        for (std::size_t leaf_i = 0; leaf_i < width; ++leaf_i) {
            std::vector<LeafInfo> old_targets;
            old_targets.reserve(choice_count);
            std::vector<FlatOperand> old_values;
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

    std::vector<std::vector<FlatOperand>> candidates;
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

    std::vector<FlatOperand> result(dyn.choices.size() * child_leaf_count);
    for (std::size_t leaf_i = 0; leaf_i < child_leaf_count; ++leaf_i) {
        std::vector<FlatOperand> candidate_values;
        candidate_values.reserve(candidates.size());
        std::vector<LeafInfo> old_targets;
        old_targets.reserve(old_leaf_groups.size());
        std::vector<FlatOperand> old_values;
        old_values.reserve(old_leaf_groups.size());
        for (std::size_t choice_i = 0; choice_i < candidates.size(); ++choice_i) {
            candidate_values.push_back(candidates[choice_i][leaf_i]);
            old_targets.push_back(old_leaf_groups[choice_i][leaf_i]);
            old_values.push_back(varOperand(old_leaf_groups[choice_i][leaf_i]));
        }
        FlatOperand selected_value = makeLookupTemp(ctx, dyn.index, std::move(candidate_values),
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
                 std::vector<FlattenedStmtPtr>& out,
                 DebugLoc loc) {
    Selection lhs = resolveLValue(ctx, target, out);
    Value rhs = flattenValue(ctx, value, out);
    if (lhs.dynamic) {
        std::size_t width = selectionWidth(lhs);
        std::vector<FlatOperand> values;
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
             std::vector<FlattenedStmtPtr>& out) {
    Selection target = resolveLValue(ctx, stmt.target, out);
    if (stmt.op.kind == OpExpr::Kind::Ternary && target.leaves.size() > 1) {
        if (stmt.op.operands.size() != 3) fail("Malformed ternary op", stmt.debug_loc);
        FlatOperand cond = flattenOperand(ctx, stmt.op.operands[0], out);
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
            FlatOpExpr op;
            op.kind = FlatOpExpr::Kind::Ternary;
            op.type = target.leaves[i].type;
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
    FlatOpExpr op;
    op.kind = static_cast<FlatOpExpr::Kind>(stmt.op.kind);
    op.type = stmt.op.type;
    op.debug_loc = stmt.op.debug_loc;
    op.unary_op = stmt.op.unary_op;
    op.binary_op = stmt.op.binary_op;
    op.hardware_op = stmt.op.hardware_op;
    op.cast_type = stmt.op.cast_type;
    op.hi = stmt.op.hi;
    op.lo = stmt.op.lo;
    op.bit = stmt.op.bit;
    op.times = stmt.op.times;
    op.to_width = stmt.op.to_width;
    op.operands = flattenOpOperands(ctx, stmt.op.operands, out, stmt.debug_loc);
    out.push_back(makeOp(target.leaves.front(), std::move(op), stmt.debug_loc));
}

std::optional<std::vector<FlatOperand>> argsByConstructorMetadata(
    Context& ctx,
    const S3Stmt& stmt,
    const std::vector<LeafInfo>& target_leaves,
    std::vector<FlattenedStmtPtr>& out) {
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
    std::unordered_map<std::string, FlatOperand> by_param;
    for (std::size_t i = 0; i < matched->param_names.size(); ++i) {
        by_param[matched->param_names[i]] = flattenOperand(ctx, stmt.args[i], out);
    }
    std::vector<FlatOperand> values;
    values.reserve(target_leaves.size());
    for (const auto& leaf : target_leaves) {
        if (leaf.path.empty()) return std::nullopt;
        const std::string& field = leaf.path.front();
        auto map_it = matched->field_to_param.find(field);
        if (map_it == matched->field_to_param.end()) {
            values.push_back(unknownOperand(leaf.type, stmt.debug_loc));
            continue;
        }
        auto param_it = by_param.find(map_it->second);
        if (param_it == by_param.end()) {
            values.push_back(unknownOperand(leaf.type, stmt.debug_loc));
            continue;
        }
        values.push_back(param_it->second);
    }
    return values;
}

void lowerConstruct(Context& ctx,
                    const S3Stmt& stmt,
                    std::vector<FlattenedStmtPtr>& out) {
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
    std::vector<FlatOperand> values;
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
               std::vector<FlattenedStmtPtr>& out) {
    if (!cfg_stmt.stmt) return;
    const S3Stmt& stmt = *cfg_stmt.stmt;
    switch (stmt.kind) {
    case S3StmtKind::Decl: {
        const auto& map = leafMap(ctx, stmt.decl_symbol);
        for (const auto& leaf : map.leaves) out.push_back(makeDecl(leaf, stmt.debug_loc));
        return;
    }
    case S3StmtKind::Assign:
        lowerAssign(ctx, stmt.target, stmt.value, out, stmt.debug_loc);
        return;
    case S3StmtKind::Op:
        lowerOp(ctx, stmt, out);
        return;
    case S3StmtKind::Construct:
        lowerConstruct(ctx, stmt, out);
        return;
    case S3StmtKind::Eval: {
        auto value = flattenOperand(ctx, stmt.value, out);
        auto flat = std::make_shared<FlattenedStmt>();
        flat->kind = FlatStmtKind::Eval;
        flat->debug_loc = stmt.debug_loc;
        flat->value = std::move(value);
        out.push_back(flat);
        return;
    }
    case S3StmtKind::Call:
        fail("S7 expects all calls to be inlined before flatten", stmt.debug_loc);
    default:
        fail("Control statement reached flattened basic block body", stmt.debug_loc);
    }
}

FlattenedEdge flattenEdge(Context& ctx,
                          const CFGEdge& edge,
                          std::vector<FlattenedStmtPtr>& prelude) {
    FlattenedEdge out;
    out.from = edge.from;
    out.to = edge.to;
    out.kind = edge.kind;
    out.label = edge.label;
    if (edge.case_value) out.case_value = flattenOperand(ctx, edge.case_value.value(), prelude);
    return out;
}

FlattenedTerminator flattenTerminator(Context& ctx,
                                      const Terminator& term,
                                      std::vector<FlattenedStmtPtr>& prelude) {
    FlattenedTerminator out;
    out.kind = term.kind;
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
            FlattenedSwitchTarget ft;
            ft.target = target.target;
            if (target.value) ft.value = flattenOperand(ctx, target.value.value(), prelude);
            out.switch_targets.push_back(std::move(ft));
        }
        break;
    case TermKind::Return:
        if (term.return_value) {
            Value value = flattenValue(ctx, term.return_value.value(), prelude);
            if (value.dynamic) fail("Return value may not be dynamic aggregate read");
            if (value.operands.size() != 1) fail("Aggregate return reached S7");
            out.return_value = value.operands.front();
        }
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
    ctx.output.return_type = ctx.input.return_type;
    ctx.output.entry = ctx.input.entry;
    ctx.output.exit = ctx.input.exit;
    ctx.output.params = ctx.input.params;

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
            FlattenedPort port;
            port.source_name = symbol.name;
            port.source_type = symbol.type;
            port.leaves = map.leaves;
            for (const auto& param : ctx.input.params) {
                if (param.name == symbol.name) {
                    port.direction = param.direction;
                    port.passing = param.passing;
                    break;
                }
            }
            ctx.output.ports.push_back(std::move(port));
        }
        ctx.output.symbol_leaf_maps.push_back(map);
        ctx.maps[symbol.id] = std::move(map);
    }
}

FlattenedFunction flattenFunction(const InlinedFunction& fn,
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
        auto out_block = std::make_unique<FlattenedBasicBlock>();
        out_block->id = block->id;
        for (const auto& stmt : block->stmts) lowerStmt(ctx, stmt, out_block->stmts);
        out_block->terminator = flattenTerminator(ctx, block->terminator, out_block->stmts);
        for (const auto& edge : block->successors) {
            out_block->successors.push_back(flattenEdge(ctx, edge, out_block->stmts));
        }
        for (const auto& edge : block->predecessors) {
            out_block->predecessors.push_back(flattenEdge(ctx, edge, out_block->stmts));
        }
        ctx.output.blocks.push_back(std::move(out_block));
    }
    summary = ctx.summary;
    return std::move(ctx.output);
}

std::string operandText(const FlatOperand& operand) {
    switch (operand.kind) {
    case FlatOperandKind::Literal: return operand.literal_value;
    case FlatOperandKind::Var: return operand.var_name;
    case FlatOperandKind::Unknown: return "unknown";
    }
    return "<operand>";
}

std::string unaryName(UnaryOp op) {
    switch (op) {
    case UnaryOp::LogicalNot: return "LogicalNot";
    case UnaryOp::BitNot: return "BitNot";
    case UnaryOp::Negate: return "Negate";
    case UnaryOp::Plus: return "Plus";
    }
    return "Unary";
}

std::string binaryName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "Add";
    case BinaryOp::Sub: return "Sub";
    case BinaryOp::Mul: return "Mul";
    case BinaryOp::Div: return "Div";
    case BinaryOp::Mod: return "Mod";
    case BinaryOp::Shl: return "Shl";
    case BinaryOp::Shr: return "Shr";
    case BinaryOp::BitAnd: return "BitAnd";
    case BinaryOp::BitOr: return "BitOr";
    case BinaryOp::BitXor: return "BitXor";
    case BinaryOp::LogicalAnd: return "LogicalAnd";
    case BinaryOp::LogicalOr: return "LogicalOr";
    case BinaryOp::Eq: return "Eq";
    case BinaryOp::Ne: return "Ne";
    case BinaryOp::Lt: return "Lt";
    case BinaryOp::Le: return "Le";
    case BinaryOp::Gt: return "Gt";
    case BinaryOp::Ge: return "Ge";
    }
    return "Binary";
}

std::string hardwareName(HardwareOp op) {
    switch (op) {
    case HardwareOp::ZExt: return "ZExt";
    case HardwareOp::SExt: return "SExt";
    case HardwareOp::Trunc: return "Trunc";
    case HardwareOp::Slice: return "Slice";
    case HardwareOp::BitSelect: return "BitSelect";
    case HardwareOp::DynamicSlice: return "DynamicSlice";
    case HardwareOp::DynamicBitSelect: return "DynamicBitSelect";
    case HardwareOp::WriteSlice: return "WriteSlice";
    case HardwareOp::WriteBit: return "WriteBit";
    case HardwareOp::DynamicWriteSlice: return "DynamicWriteSlice";
    case HardwareOp::DynamicWriteBit: return "DynamicWriteBit";
    case HardwareOp::Concat: return "Concat";
    case HardwareOp::Repeat: return "Repeat";
    case HardwareOp::ReduceOr: return "ReduceOr";
    case HardwareOp::ReduceAnd: return "ReduceAnd";
    case HardwareOp::ReduceXor: return "ReduceXor";
    }
    return "Hardware";
}

std::string opText(const FlatOpExpr& op) {
    std::ostringstream os;
    if (op.kind == FlatOpExpr::Kind::Unary) os << unaryName(op.unary_op);
    else if (op.kind == FlatOpExpr::Kind::Binary) os << binaryName(op.binary_op);
    else if (op.kind == FlatOpExpr::Kind::Ternary) os << "Ternary";
    else if (op.kind == FlatOpExpr::Kind::Cast) os << "Cast";
    else os << hardwareName(op.hardware_op);
    os << "(";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        if (i) os << ", ";
        os << operandText(op.operands[i]);
    }
    os << ")";
    return os.str();
}

std::string termKindName(TermKind kind) {
    switch (kind) {
    case TermKind::Jump: return "jump";
    case TermKind::Branch: return "branch";
    case TermKind::Switch: return "switch";
    case TermKind::Return: return "return";
    case TermKind::Unreachable: return "unreachable";
    case TermKind::Exit: return "exit";
    }
    return "term";
}

std::string stmtText(const FlattenedStmt& stmt) {
    std::ostringstream os;
    switch (stmt.kind) {
    case FlatStmtKind::Decl:
        os << "decl " << stmt.decl_name;
        break;
    case FlatStmtKind::Assign:
        os << "assign " << stmt.target_name << " = " << operandText(stmt.value);
        break;
    case FlatStmtKind::Op:
        os << "op " << stmt.target_name << " = " << opText(stmt.op);
        break;
    case FlatStmtKind::Lookup:
        os << "lookup " << stmt.target_name << " = lookup("
           << operandText(stmt.lookup_index);
        for (const auto& elem : stmt.lookup_elements) os << ", " << operandText(elem);
        os << ")";
        break;
    case FlatStmtKind::LookupWrite:
        os << "lookupwrite [";
        for (std::size_t i = 0; i < stmt.lookup_write_target_names.size(); ++i) {
            if (i) os << ", ";
            os << stmt.lookup_write_target_names[i];
        }
        os << "] = lookupwrite(" << operandText(stmt.lookup_index)
           << ", " << operandText(stmt.lookup_value);
        for (const auto& elem : stmt.lookup_elements) os << ", " << operandText(elem);
        os << ")";
        break;
    case FlatStmtKind::Eval:
        os << "eval " << operandText(stmt.value);
        break;
    }
    return os.str();
}

} // namespace

std::string debugPrint(const FlattenedProgram& program,
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
    os << "ports\n";
    for (const auto& port : fn.ports) {
        os << "  " << port.source_name << " ->";
        for (const auto& leaf : port.leaves) os << " " << leaf.name;
        os << "\n";
    }
    os << "leaf_maps\n";
    for (const auto& map : fn.symbol_leaf_maps) {
        os << "  " << map.source_name << " ->";
        for (const auto& leaf : map.leaves) os << " " << leaf.name;
        os << "\n";
    }
    for (const auto& block : fn.blocks) {
        if (!block) continue;
        os << "  bb" << block->id << "\n";
        for (const auto& stmt : block->stmts) {
            if (stmt) os << "    " << stmtText(*stmt) << "\n";
        }
        os << "    term " << termKindName(block->terminator.kind);
        if (block->terminator.kind == TermKind::Branch) {
            os << " " << operandText(block->terminator.condition)
               << " ? bb" << block->terminator.true_target
               << " : bb" << block->terminator.false_target;
        } else if (block->terminator.kind == TermKind::Jump) {
            os << " bb" << block->terminator.jump_target;
        } else if (block->terminator.kind == TermKind::Switch) {
            os << " " << operandText(block->terminator.switch_value);
        } else if (block->terminator.kind == TermKind::Return &&
                   block->terminator.return_value) {
            os << " " << operandText(block->terminator.return_value.value());
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
        FlattenedProgram out;
        out.struct_fields = program.struct_fields;
        out.struct_constructors = program.struct_constructors;
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

FlattenedProgram flattenProgramOrThrow(const InlinedCFGProgram& program,
                                       const FlattenOptions& options) {
    auto result = flattenProgram(program, options);
    if (!result.ok()) {
        throw RTLZZException(result.error->context, result.error->message);
    }
    return std::move(result.program.value());
}

} // namespace pred::s7flatten
