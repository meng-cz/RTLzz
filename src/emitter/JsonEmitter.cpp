#include "emitter/JsonEmitter.h"
#include "emitter/TextEmitter.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace pred {

// Minimal JSON emitter without external dependency.
// For production use, replace with nlohmann/json.

namespace {

constexpr const char* kSchemaVersion = "gpef-predicate-json-v1";
constexpr const char* kToolVersion = "predicate-expand-0.1";

#ifdef PREDICATE_EXPAND_BUILD_COMMIT
constexpr const char* kBuildCommit = PREDICATE_EXPAND_BUILD_COMMIT;
#else
constexpr const char* kBuildCommit = "unknown";
#endif

} // namespace

static std::string jsonEscape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c;
        }
    }
    return result;
}

static std::string exprToJson(const ExprPtr& e, int indent);

static std::string ind(int level) {
    return std::string(level * 2, ' ');
}

template <typename Map>
static std::vector<std::string> sortedKeys(const Map& map) {
    std::vector<std::string> keys;
    keys.reserve(map.size());
    for (const auto& [key, value] : map) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

static std::string exprToJson(const ExprPtr& e, int indent) {
    if (!e) return "null";
    static thread_local int node_budget = 0;
    static thread_local std::unordered_set<const Expr*> visiting;
    bool root = visiting.empty();
    if (root) node_budget = 0;

    if (indent > 8192 || node_budget++ > 1000000) {
        if (root) {
            visiting.clear();
            node_budget = 0;
        }
        throw std::runtime_error("JsonEmitter: expression too large to serialize without elision");
    }
    if (visiting.count(e.get())) {
        if (root) {
            visiting.clear();
            node_budget = 0;
        }
        throw std::runtime_error("JsonEmitter: expression cycle detected during serialization");
    }
    visiting.insert(e.get());

    std::ostringstream os;
    os << "{\n";

    switch (e->kind) {
    case ExprKind::Literal:
        os << ind(indent+1) << "\"kind\": \"literal\",\n";
        os << ind(indent+1) << "\"value\": \"" << jsonEscape(e->literal_value) << "\"";
        break;

    case ExprKind::VarRef:
        os << ind(indent+1) << "\"kind\": \"var\",\n";
        os << ind(indent+1) << "\"name\": \"" << jsonEscape(e->var_name) << "\"";
        break;

    case ExprKind::BinaryOp:
        os << ind(indent+1) << "\"kind\": \"binary\",\n";
        os << ind(indent+1) << "\"op\": \"" << jsonEscape(e->op) << "\",\n";
        os << ind(indent+1) << "\"left\": " << exprToJson(e->left, indent+1) << ",\n";
        os << ind(indent+1) << "\"right\": " << exprToJson(e->right, indent+1);
        break;

    case ExprKind::UnaryOp:
        os << ind(indent+1) << "\"kind\": \"unary\",\n";
        os << ind(indent+1) << "\"op\": \"" << jsonEscape(e->op) << "\",\n";
        os << ind(indent+1) << "\"operand\": " << exprToJson(e->operand, indent+1);
        break;

    case ExprKind::ArrayAccess:
        os << ind(indent+1) << "\"kind\": \"array_access\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->array_base, indent+1) << ",\n";
        os << ind(indent+1) << "\"index\": " << exprToJson(e->index, indent+1);
        break;

    case ExprKind::FieldAccess:
        os << ind(indent+1) << "\"kind\": \"field_access\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->struct_base, indent+1) << ",\n";
        os << ind(indent+1) << "\"field\": \"" << jsonEscape(e->field_name) << "\"";
        break;

    case ExprKind::Call:
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->intrinsic == IntrinsicKind::DynamicBitAt) {
            os << ind(indent+1) << "\"kind\": \""
               << (e->intrinsic == IntrinsicKind::DynamicBitAt ? "dynamic_bit_select" : "dynamic_slice")
               << "\",\n";
        } else {
            os << ind(indent+1) << "\"kind\": \"call\",\n";
        }
        os << ind(indent+1) << "\"callee\": \"" << jsonEscape(e->callee) << "\",\n";
        os << ind(indent+1) << "\"args\": [";
        for (size_t i = 0; i < e->args.size(); ++i) {
            if (i > 0) os << ", ";
            os << exprToJson(e->args[i], indent+2);
        }
        os << "]";
        break;

    case ExprKind::Cast:
        os << ind(indent+1) << "\"kind\": \"cast\",\n";
        os << ind(indent+1) << "\"cast_type\": \"" << jsonEscape(e->cast_type.name) << "\",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->cast_expr, indent+1);
        break;

    case ExprKind::Ternary:
        os << ind(indent+1) << "\"kind\": \"ite\",\n";
        os << ind(indent+1) << "\"cond\": " << exprToJson(e->cond, indent+1) << ",\n";
        os << ind(indent+1) << "\"then\": " << exprToJson(e->then_expr, indent+1) << ",\n";
        os << ind(indent+1) << "\"else\": " << exprToJson(e->else_expr, indent+1);
        break;

    case ExprKind::ZExt:
        os << ind(indent+1) << "\"kind\": \"zext\",\n";
        os << ind(indent+1) << "\"to_width\": " << e->to_width << ",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->cast_expr, indent+1);
        break;

    case ExprKind::SExt:
        os << ind(indent+1) << "\"kind\": \"sext\",\n";
        os << ind(indent+1) << "\"to_width\": " << e->to_width << ",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->cast_expr, indent+1);
        break;

    case ExprKind::Trunc:
        os << ind(indent+1) << "\"kind\": \"trunc\",\n";
        os << ind(indent+1) << "\"to_width\": " << e->to_width << ",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->cast_expr, indent+1);
        break;

    case ExprKind::Slice:
        os << ind(indent+1) << "\"kind\": \"slice\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->base, indent+1) << ",\n";
        os << ind(indent+1) << "\"hi\": " << e->hi << ",\n";
        os << ind(indent+1) << "\"lo\": " << e->lo;
        break;

    case ExprKind::BitSelect:
        os << ind(indent+1) << "\"kind\": \"bit_select\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->base, indent+1) << ",\n";
        os << ind(indent+1) << "\"bit\": " << e->bit;
        break;

    case ExprKind::WriteSlice:
        os << ind(indent+1) << "\"kind\": \"write_slice\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->base, indent+1) << ",\n";
        os << ind(indent+1) << "\"hi\": " << e->hi << ",\n";
        os << ind(indent+1) << "\"lo\": " << e->lo << ",\n";
        os << ind(indent+1) << "\"value\": " << exprToJson(e->value, indent+1);
        break;

    case ExprKind::WriteBit:
        os << ind(indent+1) << "\"kind\": \"write_bit\",\n";
        os << ind(indent+1) << "\"base\": " << exprToJson(e->base, indent+1) << ",\n";
        os << ind(indent+1) << "\"bit\": " << e->bit << ",\n";
        os << ind(indent+1) << "\"value\": " << exprToJson(e->value, indent+1);
        break;

    case ExprKind::Concat:
        os << ind(indent+1) << "\"kind\": \"concat\",\n";
        os << ind(indent+1) << "\"parts\": [";
        for (size_t i = 0; i < e->parts.size(); ++i) {
            if (i > 0) os << ", ";
            os << exprToJson(e->parts[i], indent+2);
        }
        os << "]";
        break;

    case ExprKind::Repeat:
        os << ind(indent+1) << "\"kind\": \"repeat\",\n";
        os << ind(indent+1) << "\"times\": " << e->times << ",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->operand, indent+1);
        break;

    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        os << ind(indent+1) << "\"kind\": \""
           << (e->kind == ExprKind::ReduceOr ? "reduce_or" :
               e->kind == ExprKind::ReduceAnd ? "reduce_and" : "reduce_xor")
           << "\",\n";
        os << ind(indent+1) << "\"expr\": " << exprToJson(e->operand, indent+1);
        break;
    }

    os << ",\n" << ind(indent+1) << "\"width\": " << e->type.width;
    os << ",\n" << ind(indent+1) << "\"signed\": " << (e->type.is_signed ? "true" : "false");
    os << ",\n" << ind(indent+1) << "\"type\": \"" << jsonEscape(e->type.name) << "\"";
    os << ",\n" << ind(indent+1) << "\"hw_kind\": \"" << jsonEscape(e->type.hw_kind) << "\"";

    os << "\n" << ind(indent) << "}";
    visiting.erase(e.get());
    if (root) {
        visiting.clear();
        node_budget = 0;
    }
    return os.str();
}

std::string emitJson(const PredicateProgram& prog) {
    std::ostringstream os;

    os << "{\n";
    os << "  \"schema_version\": \"" << jsonEscape(kSchemaVersion) << "\",\n";
    os << "  \"tool_version\": \"" << jsonEscape(kToolVersion) << "\",\n";
    os << "  \"build_commit\": \"" << jsonEscape(kBuildCommit) << "\",\n";
    os << "  \"function\": \"" << jsonEscape(prog.function_name) << "\",\n";

    os << "  \"inputs\": [";
    for (size_t i = 0; i < prog.inputs.size(); ++i) {
        if (i > 0) os << ", ";
        os << "\"" << jsonEscape(prog.inputs[i]) << "\"";
    }
    os << "],\n";

    os << "  \"outputs\": [";
    for (size_t i = 0; i < prog.outputs.size(); ++i) {
        if (i > 0) os << ", ";
        os << "\"" << jsonEscape(prog.outputs[i]) << "\"";
    }
    os << "],\n";

    os << "  \"param_directions\": {";
    bool first_dir = true;
    for (const auto& name : sortedKeys(prog.param_directions)) {
        const auto& direction = prog.param_directions.at(name);
        if (!first_dir) os << ", ";
        first_dir = false;
        os << "\"" << jsonEscape(name) << "\": \"" << jsonEscape(direction) << "\"";
    }
    os << "},\n";

    os << "  \"output_default_reasons\": {";
    bool first_default_reason = true;
    for (const auto& name : sortedKeys(prog.output_default_reasons)) {
        if (!first_default_reason) os << ", ";
        first_default_reason = false;
        os << "\"" << jsonEscape(name) << "\": \""
           << jsonEscape(prog.output_default_reasons.at(name)) << "\"";
    }
    os << "},\n";

    os << "  \"output_paired_controls\": {";
    bool first_paired_control = true;
    for (const auto& name : sortedKeys(prog.output_paired_controls)) {
        if (!first_paired_control) os << ", ";
        first_paired_control = false;
        os << "\"" << jsonEscape(name) << "\": \""
           << jsonEscape(prog.output_paired_controls.at(name)) << "\"";
    }
    os << "},\n";

    // Symbols
    os << "  \"symbols\": {\n";
    bool first_sym = true;
    for (const auto& name : sortedKeys(prog.symbols)) {
        const auto& ti = prog.symbols.at(name);
        if (!first_sym) os << ",\n";
        first_sym = false;
        os << "    \"" << jsonEscape(name) << "\": {"
           << "\"name\": \"" << jsonEscape(name) << "\", "
           << "\"type\": \"" << jsonEscape(ti.name) << "\", "
           << "\"width\": " << ti.width << ", "
           << "\"signed\": " << (ti.is_signed ? "true" : "false") << ", "
           << "\"is_hw_int\": " << (ti.is_hw_int ? "true" : "false") << ", "
           << "\"hw_kind\": \"" << jsonEscape(ti.hw_kind) << "\", "
           << "\"is_array\": " << (ti.is_array ? "true" : "false") << ", "
           << "\"array_dims\": [";
        for (size_t i = 0; i < ti.array_dims.size(); ++i) {
            if (i > 0) os << ", ";
            os << ti.array_dims[i];
        }
        os << "]";
        if (ti.is_array) os << ", \"array_size\": " << ti.array_size;
        auto dir_it = prog.param_directions.find(name);
        if (dir_it != prog.param_directions.end()) {
            os << ", \"direction\": \"" << jsonEscape(dir_it->second) << "\"";
        }
        os << "}";
    }
    os << "\n  },\n";

    os << "  \"lookup_tables\": {\n";
    bool first_table = true;
    for (const auto& name : sortedKeys(prog.lookup_tables)) {
        const auto& values = prog.lookup_tables.at(name);
        if (!first_table) os << ",\n";
        first_table = false;
        os << "    \"" << jsonEscape(name) << "\": [";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) os << ", ";
            os << "\"" << jsonEscape(values[i]) << "\"";
        }
        os << "]";
    }
    os << "\n  },\n";

    // Assignments
    os << "  \"assignments\": [\n";
    for (size_t i = 0; i < prog.assignments.size(); ++i) {
        auto& ga = prog.assignments[i];
        os << "    {\n";
        os << "      \"guard\": \"" << jsonEscape(exprToString(ga.guard)) << "\",\n";
        os << "      \"target\": \"" << jsonEscape(exprToString(ga.target)) << "\",\n";
        os << "      \"value\": \"" << jsonEscape(exprToString(ga.value)) << "\",\n";
        os << "      \"guard_expr\": " << exprToJson(ga.guard, 3) << ",\n";
        os << "      \"target_expr\": " << exprToJson(ga.target, 3) << ",\n";
        os << "      \"value_expr\": " << exprToJson(ga.value, 3) << "\n";
        os << "    }";
        if (i + 1 < prog.assignments.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"output_expressions\": {\n";
    for (size_t i = 0; i < prog.output_expressions.size(); ++i) {
        const auto& out = prog.output_expressions[i];
        os << "    \"" << jsonEscape(out.name) << "\": {\n";
        os << "      \"name\": \"" << jsonEscape(out.name) << "\",\n";
        os << "      \"expr\": \"" << jsonEscape(exprToString(out.expr)) << "\",\n";
        os << "      \"expr_tree\": " << exprToJson(out.expr, 3) << ",\n";
        os << "      \"width\": " << out.type.width << ",\n";
        os << "      \"signed\": " << (out.type.is_signed ? "true" : "false") << ",\n";
        os << "      \"hw_kind\": \"" << jsonEscape(out.type.hw_kind) << "\",\n";
        os << "      \"has_default_policy\": " << (!out.default_policy.empty() ? "true" : "false") << ",\n";
        os << "      \"default_applied\": " << (out.default_applied ? "true" : "false") << ",\n";
        os << "      \"default_value\": \"" << jsonEscape(out.default_value) << "\",\n";
        os << "      \"default_reason\": \"" << jsonEscape(out.default_reason) << "\",\n";
        os << "      \"default_policy\": \"" << jsonEscape(out.default_policy) << "\",\n";
        os << "      \"assignment_coverage\": \"" << jsonEscape(out.assignment_coverage) << "\",\n";
        os << "      \"default_guard_relation\": \"" << jsonEscape(out.default_guard_relation) << "\",\n";
        os << "      \"paired_control\": \"" << jsonEscape(out.paired_control) << "\",\n";
        os << "      \"valid_when\": \"" << jsonEscape(out.valid_when) << "\",\n";
        os << "      \"inactive_value\": \"" << jsonEscape(out.inactive_value) << "\",\n";
        os << "      \"inactive_semantics\": \"" << jsonEscape(out.inactive_semantics) << "\",\n";
        os << "      \"guard_relation\": \"" << jsonEscape(out.guard_relation) << "\"\n";
        os << "    }";
        if (i + 1 < prog.output_expressions.size()) os << ",";
        os << "\n";
    }
    os << "  },\n";

    os << "  \"diagnostics\": [";
    for (size_t i = 0; i < prog.diagnostics.size(); ++i) {
        if (i > 0) os << ", ";
        os << "\"" << jsonEscape(prog.diagnostics[i]) << "\"";
    }
    os << "]\n";
    os << "}\n";

    return os.str();
}

} // namespace pred
