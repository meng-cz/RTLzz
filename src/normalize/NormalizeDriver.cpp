#include "transform/Normalize.h"
#include "predicate/PredicateIR.h"
#include "semantics/AliasGraph.h"
#include "semantics/IntSemantics.h"
#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace pred {

namespace {

static constexpr bool allowUnsafeTextFallback = false;

void addOutputParam(std::vector<std::string>& outputs, const std::string& name) {
    if (name.empty()) return;
    if (std::find(outputs.begin(), outputs.end(), name) == outputs.end()) {
        outputs.push_back(name);
    }
}

struct Env;
ExprPtr rewriteExpr(const ExprPtr& e, Env& env);
std::vector<StmtPtr> rewriteStmts(const std::vector<StmtPtr>& stmts, Env& env);
const std::vector<StructFieldInfo>* structFieldsForType(const TypeInfo& type, const Env& env);
const std::vector<StructFieldInfo>* findStructFields(const TypeInfo& type, Env& env);
ExprPtr buildPackedStructValue(const ExprPtr& value, TypeInfo target_type, Env& env);
TypeInfo normalizeArrayType(TypeInfo type);
int flattenedTypeWidth(const TypeInfo& type, Env& env);
bool appendStructUnpackDecls(const StmtPtr& block,
                             const std::string& prefix,
                             const TypeInfo& type,
                             const ExprPtr& packed,
                             int& offset,
                             Env& env);

struct Env {
    std::unordered_map<std::string, TypeInfo> symbols;
    std::unordered_map<std::string, TypeInfo> ssa_seed_symbols;
    std::unordered_set<std::string> initialized;
    std::unordered_map<std::string, std::vector<bool>> bit_initialized;
    std::unordered_map<std::string, FunctionAST> helpers;
    std::unordered_map<std::string, FunctionAST> lambdas;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
    std::unordered_map<std::string, std::vector<std::string>> lookup_tables;
    std::unordered_map<std::string, std::string> param_directions;
    std::unordered_map<std::string, std::string> output_default_reasons;
    std::unordered_map<std::string, std::string> output_paired_controls;
    struct RegProxyAlias {
        std::string rdata;
        std::string wen;
        std::string wdata;
    };
    struct ReqHelperAlias {
        std::string vld;
        std::string rdy;
        std::string arg_data;
        std::string ret_data;
        std::vector<std::pair<std::string, std::string>> payloads;
        std::vector<std::pair<std::string, std::string>> ret_payloads;
    };
    std::unordered_map<std::string, RegProxyAlias> regproxy_aliases;
    std::unordered_map<std::string, ReqHelperAlias> reqhelper_aliases;
    AliasGraph alias_graph;
    std::unordered_set<std::string> input_arrays;
    std::vector<std::string> output_params;
    std::unordered_set<std::string> formal_write_reset_done;
    std::shared_ptr<int> inline_counter = std::make_shared<int>(0);
    std::shared_ptr<std::unordered_set<std::string>> formal_reads =
        std::make_shared<std::unordered_set<std::string>>();
    std::shared_ptr<std::unordered_set<std::string>> formal_writes =
        std::make_shared<std::unordered_set<std::string>>();
    std::string error;
};

void markFormalWrite(Env& env, const std::string& name) {
    if (name.empty()) return;
    env.formal_writes->insert(name);
    auto dir = env.param_directions.find(name);
    const bool is_output_formal = dir != env.param_directions.end() && dir->second == "Output";
    if (is_output_formal) addOutputParam(env.output_params, name);
    auto sym = env.symbols.find(name);
    const bool reads_input_value = env.formal_reads->count(name) != 0;
    if (sym != env.symbols.end() && reads_input_value && dir != env.param_directions.end() &&
        (dir->second == "Input" || dir->second == "Output")) {
        env.ssa_seed_symbols[name] = sym->second;
    } else {
        env.ssa_seed_symbols.erase(name);
        if (!env.formal_write_reset_done.count(name)) {
            env.initialized.erase(name);
            env.bit_initialized.erase(name);
            env.formal_write_reset_done.insert(name);
        }
    }
}

bool isFormalOutput(const Env& env, const std::string& name) {
    auto it = env.param_directions.find(name);
    return it != env.param_directions.end() && it->second == "Output";
}

std::string handshakeDataPrefix(const std::string& name) {
    const std::string suffix = "_data__";
    if (name.size() <= suffix.size()) return {};
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return name.substr(0, name.size() - suffix.size());
}

void registerHandshakePayloadDefaults(Env& env) {
    for (const auto& item : env.param_directions) {
        const std::string& data = item.first;
        if (item.second != "Output") continue;
        std::string prefix = handshakeDataPrefix(data);
        if (prefix.empty()) continue;
        std::string ready = prefix + "__rdy__";
        std::string valid = prefix + "__vld__";
        auto ready_it = env.param_directions.find(ready);
        auto valid_it = env.param_directions.find(valid);
        if (ready_it == env.param_directions.end() ||
            valid_it == env.param_directions.end()) {
            continue;
        }
        bool ready_is_output = ready_it->second == "Output";
        bool valid_is_input = valid_it->second == "Input";
        if (!ready_is_output || !valid_is_input) continue;
        env.output_default_reasons[data] = "handshake_payload_default_zero_when_not_ready_valid";
        env.output_paired_controls[data] = ready;
    }
}

std::string childServiceResponsePrefix(const std::string& name) {
    const std::string suffix = "_out__";
    if (name.size() <= suffix.size()) return {};
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return name.substr(0, name.size() - suffix.size());
}

void registerChildServiceResponseDefaults(Env& env) {
    for (const auto& item : env.param_directions) {
        const std::string& payload = item.first;
        if (item.second != "Output") continue;
        if (env.output_paired_controls.count(payload) != 0) continue;

        std::string prefix = childServiceResponsePrefix(payload);
        if (prefix.empty()) continue;
        std::string valid = prefix + "__vld__";
        auto valid_it = env.param_directions.find(valid);
        if (valid_it == env.param_directions.end()) continue;
        bool valid_is_input = valid_it->second == "Input";
        if (!valid_is_input) continue;

        env.output_default_reasons[payload] = "payload_default_zero_when_valid_false";
        env.output_paired_controls[payload] = valid;
    }
}

std::string requestOutputPayloadPrefix(const std::string& name) {
    const std::string suffix = "__";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    std::string stem = name.substr(0, name.size() - suffix.size());
    auto sep = stem.rfind('_');
    if (sep == std::string::npos || sep == 0) return {};
    return stem.substr(0, sep);
}

void registerRequestOutputPayloadDefaults(Env& env) {
    for (const auto& item : env.param_directions) {
        const std::string& payload = item.first;
        if (item.second != "Output") continue;
        if (env.output_paired_controls.count(payload) != 0) continue;

        std::string prefix = requestOutputPayloadPrefix(payload);
        if (prefix.empty()) continue;
        std::string valid = prefix + "__vld__";
        auto valid_it = env.param_directions.find(valid);
        if (valid_it == env.param_directions.end() || valid_it->second != "Output") {
            continue;
        }

        env.output_default_reasons[payload] = "payload_default_zero_when_valid_false";
        env.output_paired_controls[payload] = valid;
    }
}

ExprPtr zeroValueForType(const TypeInfo& type) {
    if (type.width == 1 || type.hw_kind == "bool" || type.name == "bool") {
        return make_literal("false", TypeInfo{"bool", 1, false, true, "bool"});
    }
    TypeInfo t = type;
    if (t.width <= 0) t = make_hw_type("UInt", 1, false);
    return make_literal("0", t);
}

void addSeedSymbol(Env& env, const std::string& name, const TypeInfo& type) {
    if (!name.empty()) env.ssa_seed_symbols[name] = type;
}

void addSeedSymbolsWithPrefix(Env& env, const std::string& prefix) {
    for (auto& [name, type] : env.symbols) {
        if (name == prefix || name.rfind(prefix + "_", 0) == 0) {
            env.ssa_seed_symbols[name] = type;
        }
    }
}

std::string semanticDefaultReasonFor(const Env& env, const std::string& name) {
    auto exact = env.output_default_reasons.find(name);
    if (exact != env.output_default_reasons.end()) return exact->second;
    for (const auto& item : env.output_default_reasons) {
        const std::string prefix = item.first + "_";
        if (name.rfind(prefix, 0) == 0) return item.second;
    }
    return {};
}

bool hasSemanticDefaultFor(const Env& env, const std::string& name) {
    return !semanticDefaultReasonFor(env, name).empty();
}

bool allFlattenedElementsInitialized(const std::string& name,
                                     const TypeInfo& arr_type,
                                     Env& env,
                                     std::vector<int>& prefix,
                                     size_t depth);
std::string firstUninitializedFlattenedElement(const std::string& name,
                                               const TypeInfo& arr_type,
                                               Env& env,
                                               std::vector<int>& prefix,
                                               size_t depth);

bool isRegProxyType(const TypeInfo& type) {
    return type.struct_name.find("__RegProxy") != std::string::npos ||
           type.name.find("__RegProxy") != std::string::npos;
}

bool isReqHelperType(const TypeInfo& type) {
    return type.struct_name.find("__ReqHelper") != std::string::npos ||
           type.struct_name.find("__ChildServiceHelper") != std::string::npos ||
           type.struct_name.find("__ChildQueryHelper") != std::string::npos ||
           type.name.find("__ReqHelper") != std::string::npos ||
           type.name.find("__ChildServiceHelper") != std::string::npos ||
           type.name.find("__ChildQueryHelper") != std::string::npos;
}

bool isProxyCarrierType(const TypeInfo& type) {
    return isRegProxyType(type) || isReqHelperType(type) ||
           type.struct_name.find("__BRAMProxy") != std::string::npos ||
           type.name.find("__BRAMProxy") != std::string::npos ||
           type.struct_name.find("QueueProxy") != std::string::npos ||
           type.name.find("QueueProxy") != std::string::npos;
}

bool isBuiltinScalarType(const TypeInfo& type) {
    if (type.is_array || type.is_pointer || type.is_reference ||
        !type.struct_name.empty()) {
        return false;
    }
    if (type.hw_kind == "builtin") return true;
    return type.name == "char" || type.name == "signed char" ||
           type.name == "unsigned char" || type.name == "short" ||
           type.name == "unsigned short" || type.name == "int" ||
           type.name == "unsigned int" || type.name == "long" ||
           type.name == "unsigned long" || type.name == "long long" ||
           type.name == "unsigned long long" || type.name == "uint8_t" ||
           type.name == "uint16_t" || type.name == "uint32_t" ||
           type.name == "uint64_t" || type.name == "int8_t" ||
           type.name == "int16_t" || type.name == "int32_t" ||
           type.name == "int64_t";
}

bool isBuiltinExpressionType(const TypeInfo& type) {
    return isBuiltinScalarType(type) ||
           is_bool_type_info(type);
}

bool isBuiltinBinaryExpression(const ExprPtr& original,
                               const ExprPtr& lhs,
                               const ExprPtr& rhs) {
    return original && lhs && rhs &&
           isBuiltinExpressionType(original->type) &&
           isBuiltinExpressionType(lhs->type) &&
           isBuiltinExpressionType(rhs->type);
}

bool builtinOperandsHaveCanonicalCommonType(const ExprPtr& lhs,
                                            const ExprPtr& rhs) {
    return lhs && rhs &&
           lhs->type.width == rhs->type.width &&
           lhs->type.is_signed == rhs->type.is_signed &&
           is_bool_type_info(lhs->type) == is_bool_type_info(rhs->type);
}

TypeInfo promotedBuiltinType(const TypeInfo& type) {
    if (!isBuiltinExpressionType(type)) return type;
    if (is_bool_type_info(type) || type.width < 32) {
        return TypeInfo{"int", 32, true, true, "builtin"};
    }
    return type;
}

TypeInfo builtinTypeForWidth(int width, bool is_signed) {
    if (width <= 32) {
        return TypeInfo{is_signed ? "int" : "unsigned int",
                        32, is_signed, true, "builtin"};
    }
    return TypeInfo{is_signed ? "long long" : "unsigned long long",
                    width, is_signed, true, "builtin"};
}

TypeInfo usualArithmeticBuiltinType(const TypeInfo& lhs,
                                    const TypeInfo& rhs) {
    TypeInfo l = promotedBuiltinType(lhs);
    TypeInfo r = promotedBuiltinType(rhs);
    if (l.width <= 0 || r.width <= 0) return {};
    if (l.is_signed == r.is_signed) {
        return l.width >= r.width ? l : r;
    }

    const TypeInfo& unsigned_type = l.is_signed ? r : l;
    const TypeInfo& signed_type = l.is_signed ? l : r;
    if (unsigned_type.width >= signed_type.width) {
        return unsigned_type;
    }
    if (signed_type.width > unsigned_type.width) {
        return signed_type;
    }
    return builtinTypeForWidth(signed_type.width, false);
}

void canonicalizeBuiltinBinaryOperands(ExprPtr& lhs, ExprPtr& rhs) {
    if (!lhs || !rhs) return;
    lhs->type = canonicalize_bool_type(std::move(lhs->type));
    rhs->type = canonicalize_bool_type(std::move(rhs->type));
    if (is_bool_type_info(lhs->type) && is_bool_type_info(rhs->type)) {
        return;
    }
    TypeInfo common = usualArithmeticBuiltinType(lhs->type, rhs->type);
    if (common.width <= 0) return;
    lhs = castIfWidthChanges(lhs, common);
    rhs = castIfWidthChanges(rhs, common);
}

bool widenBuiltinBitwiseOperands(ExprPtr& lhs, ExprPtr& rhs) {
    if (!lhs || !rhs || lhs->type.width <= 0 || rhs->type.width <= 0 ||
        lhs->type.width == rhs->type.width) {
        return true;
    }
    if (!isBuiltinScalarType(lhs->type) && !isBuiltinScalarType(rhs->type)) {
        return false;
    }
    if (lhs->type.width > rhs->type.width) {
        rhs = castIfWidthChanges(rhs, lhs->type);
    } else {
        lhs = castIfWidthChanges(lhs, rhs->type);
    }
    return true;
}

bool isVulFixedBitVectorType(const TypeInfo& type) {
    if (type.hw_kind == "builtin") return false;
    return (type.is_hw_int && type.hw_kind != "builtin") ||
           type.hw_kind == "Int" || type.hw_kind == "UInt" ||
           type.hw_kind == "signed_view" ||
           type.name.rfind("Int<", 0) == 0 ||
           type.name.rfind("UInt<", 0) == 0;
}

bool isBuiltinDivRemType(const TypeInfo& type) {
    return isBuiltinScalarType(type) && !isVulFixedBitVectorType(type) &&
           type.width > 0 && type.hw_kind != "bool" && type.name != "bool";
}

bool unifyBuiltinDivRemOperands(ExprPtr& lhs, ExprPtr& rhs, TypeInfo& result, Env& env) {
    if (!lhs || !rhs || !isBuiltinDivRemType(lhs->type) || !isBuiltinDivRemType(rhs->type)) {
        return false;
    }
    if (lhs->type.is_signed != rhs->type.is_signed) {
        env.error = "Unsupported mixed signed/unsigned builtin division/modulo";
        return true;
    }
    result = lhs->type.width >= rhs->type.width ? lhs->type : rhs->type;
    result.is_signed = lhs->type.is_signed;
    if (lhs->type.width != result.width) lhs = castIfWidthChanges(lhs, result);
    if (rhs->type.width != result.width) rhs = castIfWidthChanges(rhs, result);
    return true;
}

TypeInfo symbolType(Env& env, const std::string& name, TypeInfo fallback = {}) {
    auto it = env.symbols.find(name);
    return it == env.symbols.end() ? fallback : it->second;
}

ExprPtr readRegProxy(const std::string& proxy, TypeInfo desired_type, Env& env) {
    auto alias_it = env.regproxy_aliases.find(proxy);
    if (alias_it == env.regproxy_aliases.end()) return nullptr;
    const auto& alias = alias_it->second;
    TypeInfo rdata_type = symbolType(env, alias.rdata);
    if (rdata_type.width <= 0) {
        env.error = "Unsupported RegProxy rdata alias without known width for '" + proxy + "'";
        return nullptr;
    }
    int width = desired_type.width > 0 ? desired_type.width : rdata_type.width;
    int hi = std::min(width, rdata_type.width) - 1;
    if (hi < 0) {
        env.error = "Unsupported zero-width RegProxy read for '" + proxy + "'";
        return nullptr;
    }
    bool is_signed = desired_type.width > 0 ? desired_type.is_signed : rdata_type.is_signed;
    return make_slice(make_var(alias.rdata, rdata_type),
                      hi,
                      0,
                      make_hw_type(is_signed ? "Int" : "UInt", hi + 1, is_signed));
}

std::optional<Env::ReqHelperAlias> resolveReqHelperAlias(const ExprPtr& maybe_receiver, Env& env) {
    std::string receiver = baseName(maybe_receiver);
    if (!receiver.empty()) {
        auto it = env.reqhelper_aliases.find(receiver);
        if (it != env.reqhelper_aliases.end()) return it->second;
        env.error = "Unsupported output call with unknown ReqHelper alias '" + receiver + "'";
        return std::nullopt;
    }
    if (env.reqhelper_aliases.size() == 1) return env.reqhelper_aliases.begin()->second;
    if (env.reqhelper_aliases.empty()) {
        env.error = "Unsupported output call without ReqHelper constructor alias";
    } else {
        env.error = "Unsupported output call with ambiguous ReqHelper alias";
    }
    return std::nullopt;
}

bool isRegProxyWdataAliasTarget(const std::string& name, const Env& env) {
    for (const auto& item : env.regproxy_aliases) {
        const auto& wdata = item.second.wdata;
        if (name == wdata || name.rfind(wdata + "_", 0) == 0) return true;
    }
    return false;
}

std::string canonicalOutputNameFromReqHelperAlias(const std::string& name, const Env& env) {
    for (const auto& item : env.reqhelper_aliases) {
        const auto& alias = item.second;
        if (!alias.arg_data.empty() && name == alias.arg_data) return alias.arg_data;
    }
    return name;
}

void replaceOutputParamName(Env& env, const std::string& old_name, const std::string& new_name) {
    if (old_name == new_name) return;
    for (auto& name : env.output_params) {
        if (name == old_name) name = new_name;
    }
}

void markScalarFullyInitialized(Env& env, const std::string& name, const TypeInfo& type) {
    env.initialized.insert(name);
    if (const auto* fields = findStructFields(type, env)) {
        for (const auto& field : *fields) {
            markScalarFullyInitialized(env, name + "_" + field.name, field.type);
        }
    }
    if (type.width > 0) {
        env.bit_initialized[name] = std::vector<bool>(static_cast<size_t>(type.width), true);
    }
}

void markBitRangeInitialized(Env& env,
                             const std::string& name,
                             const TypeInfo& type,
                             int hi,
                             int lo) {
    if (type.width <= 0) {
        env.initialized.insert(name);
        return;
    }
    if (hi < lo) std::swap(hi, lo);
    if (hi < 0 || lo < 0 || hi >= type.width) {
        env.error = "Bit/slice assignment out of bounds for '" + name + "'";
        return;
    }
    auto& bits = env.bit_initialized[name];
    if (bits.size() != static_cast<size_t>(type.width)) {
        bits.assign(static_cast<size_t>(type.width), false);
    }
    for (int i = lo; i <= hi; ++i) bits[static_cast<size_t>(i)] = true;
    if (std::all_of(bits.begin(), bits.end(), [](bool v) { return v; })) {
        env.initialized.insert(name);
    }
}

bool hasAnyBitInitialized(Env& env, const std::string& name) {
    auto it = env.bit_initialized.find(name);
    if (it == env.bit_initialized.end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [](bool v) { return v; });
}

ExprPtr lowerPowerOfTwoDivRem(const std::string& op, ExprPtr lhs, ExprPtr rhs, Env& env) {
    int divisor = literalIntValue(rhs, -1);
    bool is_power_of_two = divisor > 0 && (divisor & (divisor - 1)) == 0;
    if (!lhs || !rhs || lhs->type.width <= 0 || !is_power_of_two ||
        lhs->type.is_signed || lhs->type.hw_kind == "signed_view") {
        env.error = IntSemantics::binaryResultType(op, lhs ? lhs->type : TypeInfo{},
                                                   rhs ? rhs->type : TypeInfo{}).error;
        if (lhs && rhs) {
            env.error += " lhs_type='" + lhs->type.name + "/" + lhs->type.hw_kind +
                         "' lhs_width=" + std::to_string(lhs->type.width) +
                         " lhs_signed=" + (lhs->type.is_signed ? std::string("true") : std::string("false")) +
                         " rhs_type='" + rhs->type.name + "/" + rhs->type.hw_kind +
                         "' rhs_width=" + std::to_string(rhs->type.width) +
                         " rhs_signed=" + (rhs->type.is_signed ? std::string("true") : std::string("false")) +
                         " divisor_literal=" + std::to_string(divisor);
        }
        return nullptr;
    }

    if (op == "%") {
        auto mask = make_literal(std::to_string(divisor - 1), lhs->type);
        return make_binary("&", lhs, mask, lhs->type);
    }

    int shift = 0;
    for (int value = divisor; value > 1; value >>= 1) ++shift;
    auto shift_lit = make_literal(std::to_string(shift), make_hw_type("UInt", 32, false));
    return make_binary(">>", lhs, shift_lit, lhs->type);
}

std::string compactToken(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '_';
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

const StructFieldInfo* findStructFieldByToken(const TypeInfo& type,
                                              const Env& env,
                                              const std::string& token) {
    const auto* fields = structFieldsForType(type, env);
    if (!fields) return nullptr;
    std::string wanted = compactToken(token);
    for (const auto& field : *fields) {
        if (compactToken(field.name).find(wanted) != std::string::npos) {
            return &field;
        }
    }
    return nullptr;
}

bool memberCallReceiver(const ExprPtr& call, ExprPtr& receiver) {
    if (!call || call->args.empty()) return false;
    auto head = call->args.front();
    if (!head) return false;
    if (head->kind == ExprKind::FieldAccess && head->struct_base) {
        if (!call->callee.empty() && head->field_name != call->callee) return false;
        receiver = head->struct_base;
        return receiver != nullptr;
    }
    if (head->kind == ExprKind::VarRef || head->kind == ExprKind::ArrayAccess ||
        head->kind == ExprKind::FieldAccess) {
        receiver = head;
        return receiver != nullptr;
    }
    return false;
}

bool typeMatchesForProxyReturn(TypeInfo candidate, const TypeInfo& wanted) {
    if (candidate.is_array) candidate = scalarTypeFromArray(candidate);
    if (!wanted.struct_name.empty()) {
        return candidate.struct_name == wanted.struct_name ||
               candidate.name == wanted.struct_name ||
               candidate.name == wanted.name;
    }
    if (wanted.width > 0 && candidate.width > 0) return candidate.width == wanted.width;
    return false;
}

ExprPtr recoverProxyReceiverByReturnType(const ExprPtr& call, Env& env) {
    if (!call || call->callee != "readdata") return nullptr;
    std::string match;
    std::string candidates;
    for (const auto& [name, type] : env.symbols) {
        const StructFieldInfo* value_field = findStructFieldByToken(type, env, "readdata_buf");
        if (!value_field) value_field = findStructFieldByToken(type, env, "readdata");
        if (value_field) {
            if (!candidates.empty()) candidates += ",";
            candidates += name + ":" + type.name + "/" + type.struct_name +
                ":field_width=" + std::to_string(value_field->type.width) +
                ":field_struct=" + value_field->type.struct_name;
        }
        if (!value_field || !typeMatchesForProxyReturn(value_field->type, call->type)) continue;
        if (!match.empty()) return nullptr;
        match = name;
    }
    if (match.empty() && !candidates.empty()) {
        env.error = "Unable to recover readdata receiver for return type '" +
                    call->type.name + "/" + call->type.struct_name +
                    "' candidates=" + candidates;
    }
    if (match.empty()) return nullptr;
    return make_var(match, env.symbols[match]);
}

ExprPtr readStructFieldToken(const ExprPtr& receiver,
                             const std::string& token,
                             Env& env,
                             TypeInfo fallback = {}) {
    if (!receiver) return nullptr;
    const auto* field = findStructFieldByToken(receiver->type, env, token);
    if (!field) return nullptr;
    std::string object = baseName(receiver);
    TypeInfo field_type = field->type.width > 0 ? field->type : fallback;
    if (!object.empty()) {
        if (auto alias = env.alias_graph.resolvePath(object, {field->name})) {
            TypeInfo alias_type = symbolType(env, alias->canonical_name, field_type);
            return rewriteExpr(make_var(alias->canonical_name, alias_type), env);
        }
    }
    if (env.symbols.count(field->name)) {
        TypeInfo direct_type = symbolType(env, field->name, field_type);
        return rewriteExpr(make_var(field->name, direct_type), env);
    }
    auto access = make_field_access(cloneExpr(receiver), field->name, field_type);
    return rewriteExpr(access, env);
}

ExprPtr portElementForFieldToken(const ExprPtr& receiver,
                                 const std::string& token,
                                 int port,
                                 Env& env) {
    if (!receiver) return nullptr;
    const auto* field = findStructFieldByToken(receiver->type, env, token);
    if (!field) return nullptr;
    std::string object = baseName(receiver);
    std::string port_name;
    if (auto alias = env.alias_graph.resolvePath(object, {field->name})) {
        port_name = alias->canonical_name;
    }
    if (port_name.empty()) port_name = object + "_" + field->name;
    TypeInfo arr_type = symbolType(env, port_name, field->type);
    if (arr_type.is_array && arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    TypeInfo scalar = scalarTypeFromArray(arr_type);
    return make_array_access(make_var(port_name, arr_type),
                             make_literal(std::to_string(port), TypeInfo{"int", 32, true}),
                             scalar);
}

ExprPtr lowerProxyMethodExpr(const ExprPtr& call, Env& env) {
    ExprPtr receiver;
    if (!memberCallReceiver(call, receiver)) {
        receiver = recoverProxyReceiverByReturnType(call, env);
        if (!receiver) return nullptr;
    }
    if (call->callee == "enqready") {
        return readStructFieldToken(receiver, "enqready", env, make_hw_type("bool", 1, false));
    }
    if (call->callee == "deqvalid") {
        return readStructFieldToken(receiver, "deqvalid", env, make_hw_type("bool", 1, false));
    }
    if (call->callee == "front") {
        auto value = readStructFieldToken(receiver, "deqdata", env, call->type);
        if (!value || !env.error.empty()) return value;
        return castIfWidthChanges(value, call->type);
    }
    if (call->callee == "readdata") {
        auto value = portElementForFieldToken(receiver, "readdata", 0, env);
        if (!value) return nullptr;
        return rewriteExpr(value, env);
    }
    return nullptr;
}

std::vector<StmtPtr> lowerProxyProcedureCall(const ExprPtr& call, Env& env) {
    std::vector<StmtPtr> out;
    ExprPtr receiver;
    if (!memberCallReceiver(call, receiver)) return out;

    auto assign_field = [&](const std::string& token, ExprPtr value) -> bool {
        const auto* field = findStructFieldByToken(receiver->type, env, token);
        if (!field) {
            env.error = "Unsupported proxy method '" + call->callee +
                        "': missing field token '" + token + "'";
            return false;
        }
        auto target = make_field_access(cloneExpr(receiver), field->name, field->type);
        out.push_back(makeAssignStmt(target, std::move(value)));
        return true;
    };

    if (call->callee == "deqnext") {
        assign_field("deqready", make_literal("true", make_hw_type("bool", 1, false)));
    } else if (call->callee == "clrnext") {
        assign_field("clrnext", make_literal("true", make_hw_type("bool", 1, false)));
    } else if (call->callee == "enqnext") {
        if (call->args.size() < 2) {
            env.error = "Unsupported proxy method 'enqnext' without data argument";
            return out;
        }
        if (!assign_field("enqdata", cloneExpr(call->args[1]))) return out;
        assign_field("enqvalid", make_literal("true", make_hw_type("bool", 1, false)));
    } else if (call->callee == "readreq") {
        if (call->args.size() < 2) {
            env.error = "Unsupported BRAM readreq without address argument";
            return out;
        }
        auto req = portElementForFieldToken(receiver, "readreq", 0, env);
        auto addr = portElementForFieldToken(receiver, "readaddr", 0, env);
        if (!req || !addr) {
            env.error = "Unsupported BRAM readreq without structured ports";
            return out;
        }
        out.push_back(makeAssignStmt(req, make_literal("true", make_hw_type("bool", 1, false))));
        out.push_back(makeAssignStmt(addr, cloneExpr(call->args[1])));
    } else if (call->callee == "write") {
        if (call->args.size() < 3) {
            env.error = "Unsupported BRAM write without address/data arguments";
            return out;
        }
        auto write = portElementForFieldToken(receiver, "write", 0, env);
        auto addr = portElementForFieldToken(receiver, "writeaddr", 0, env);
        auto data = portElementForFieldToken(receiver, "writedata", 0, env);
        if (!write || !addr || !data) {
            env.error = "Unsupported BRAM write without structured ports";
            return out;
        }
        ExprPtr payload = cloneExpr(call->args[2]);
        if (auto packed = buildPackedStructValue(payload, data->type, env)) {
            payload = packed;
        }
        out.push_back(makeAssignStmt(write, make_literal("true", make_hw_type("bool", 1, false))));
        out.push_back(makeAssignStmt(addr, cloneExpr(call->args[1])));
        out.push_back(makeAssignStmt(data, payload));
    }
    return out;
}

struct InlineExpressionFlow {
    std::unordered_map<std::string, ExprPtr> values;
    std::unordered_map<std::string, TypeInfo> types;
    ExprPtr active = make_literal("true", make_hw_type("bool", 1, false));
    ExprPtr return_value;
    ExprPtr return_guard;
};

std::optional<std::pair<std::string, TypeInfo>>
inlineLocalFieldTarget(const ExprPtr& target,
                       const InlineExpressionFlow& flow,
                       Env& env) {
    if (!target || target->kind != ExprKind::FieldAccess ||
        !target->struct_base || target->struct_base->kind != ExprKind::VarRef) {
        return std::nullopt;
    }
    const std::string base = target->struct_base->var_name;
    auto type_it = flow.types.find(base);
    if (type_it == flow.types.end()) return std::nullopt;
    const auto* fields = findStructFields(type_it->second, env);
    if (!fields) return std::nullopt;
    for (const auto& field : *fields) {
        if (field.name == target->field_name) {
            return std::make_pair(base + "_" + field.name, field.type);
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, TypeInfo>>
inlineLocalArrayTarget(const ExprPtr& target,
                       const InlineExpressionFlow& flow) {
    ExprPtr base;
    std::vector<ExprPtr> indices;
    if (!collectArrayAccess(target, base, indices) || !base) return std::nullopt;
    const std::string name = baseName(base);
    if (name.empty()) return std::nullopt;
    auto type_it = flow.types.find(name);
    if (type_it == flow.types.end()) return std::nullopt;
    TypeInfo arr_type = normalizeArrayType(type_it->second);
    if (!arr_type.is_array || arr_type.array_dims.empty()) return std::nullopt;
    if (indices.size() != arr_type.array_dims.size()) return std::nullopt;
    std::vector<int> literal_prefix;
    for (const auto& idx : indices) {
        auto lit = literalIndex(idx);
        if (!lit.has_value()) return std::nullopt;
        literal_prefix.push_back(*lit);
    }
    return std::make_pair(joinIndexName(name, literal_prefix),
                          scalarTypeFromArray(arr_type));
}

ExprPtr packInlineStructLocal(const std::string& name,
                              const TypeInfo& type,
                              const InlineExpressionFlow& flow,
                              Env& env,
                              const std::string& callee) {
    const auto* fields = findStructFields(type, env);
    if (!fields || fields->empty()) return nullptr;
    std::vector<ExprPtr> parts;
    for (const auto& field : *fields) {
        const std::string flat = name + "_" + field.name;
        ExprPtr part;
        if (findStructFields(field.type, env)) {
            part = packInlineStructLocal(flat, field.type, flow, env, callee);
        } else {
            auto value_it = flow.values.find(flat);
            if (value_it == flow.values.end()) {
                env.error = "Helper function '" + callee +
                            "' returns struct local '" + name +
                            "' with uninitialized field '" + field.name + "'";
                return nullptr;
            }
            part = castIfWidthChanges(cloneExpr(value_it->second), field.type);
        }
        if (!part || !env.error.empty()) return nullptr;
        parts.push_back(std::move(part));
    }
    std::reverse(parts.begin(), parts.end());
    auto packed = make_concat(std::move(parts));
    TypeInfo packed_type = type;
    packed_type.width = packed->type.width;
    packed->type = packed_type;
    return packed;
}

ExprPtr packInlineArrayLocal(const std::string& name,
                             const TypeInfo& type,
                             const InlineExpressionFlow& flow,
                             Env& env,
                             const std::string& callee) {
    TypeInfo arr_type = normalizeArrayType(type);
    if (!arr_type.is_array || arr_type.array_dims.empty()) return nullptr;
    TypeInfo elem_type = scalarTypeFromArray(arr_type);
    std::vector<ExprPtr> parts;
    std::vector<int> prefix;
    auto collect = [&](auto&& self, std::size_t depth) -> bool {
        if (depth >= arr_type.array_dims.size()) {
            const std::string flat = joinIndexName(name, prefix);
            ExprPtr part;
            if (findStructFields(elem_type, env)) {
                part = packInlineStructLocal(flat, elem_type, flow, env, callee);
            } else {
                auto value_it = flow.values.find(flat);
                if (value_it == flow.values.end()) {
                    env.error = "Helper function '" + callee +
                                "' returns array local '" + name +
                                "' with uninitialized element '" + flat + "'";
                    return false;
                }
                part = castIfWidthChanges(cloneExpr(value_it->second), elem_type);
            }
            if (!part || !env.error.empty()) return false;
            parts.push_back(std::move(part));
            return true;
        }
        for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
            prefix.push_back(i);
            if (!self(self, depth + 1)) return false;
            prefix.pop_back();
        }
        return true;
    };
    if (!collect(collect, 0)) return nullptr;
    std::reverse(parts.begin(), parts.end());
    auto packed = make_concat(std::move(parts));
    packed->type = make_hw_type("UInt", packed->type.width, false);
    return packed;
}

bool storeInlineStructLocal(const std::string& name,
                            const TypeInfo& type,
                            const ExprPtr& packed,
                            InlineExpressionFlow& flow,
                            Env& env,
                            const std::string& callee,
                            int& offset) {
    const auto* fields = findStructFields(type, env);
    if (!fields || fields->empty()) return false;
    for (const auto& field : *fields) {
        const std::string flat = name + "_" + field.name;
        if (findStructFields(field.type, env)) {
            flow.types[flat] = field.type;
            if (!storeInlineStructLocal(flat, field.type, packed, flow, env, callee, offset)) {
                return false;
            }
            continue;
        }
        int field_width = flattenedTypeWidth(field.type, env);
        if (field_width <= 0) {
            env.error = "Helper function '" + callee +
                        "' assigns struct field with unknown width '" + flat + "'";
            return false;
        }
        TypeInfo leaf_type = field.type;
        leaf_type.width = field_width;
        flow.types[flat] = leaf_type;
        flow.values[flat] = make_slice(cloneExpr(packed),
                                       offset + field_width - 1,
                                       offset,
                                       leaf_type);
        offset += field_width;
    }
    return true;
}

bool storeInlineArrayLocal(const std::string& name,
                           const TypeInfo& type,
                           const ExprPtr& packed,
                           InlineExpressionFlow& flow,
                           Env& env,
                           const std::string& callee,
                           int& offset) {
    TypeInfo arr_type = normalizeArrayType(type);
    if (!arr_type.is_array || arr_type.array_dims.empty()) return false;
    TypeInfo elem_type = scalarTypeFromArray(arr_type);
    std::vector<int> prefix;
    auto store = [&](auto&& self, std::size_t depth) -> bool {
        if (depth >= arr_type.array_dims.size()) {
            const std::string flat = joinIndexName(name, prefix);
            flow.types[flat] = elem_type;
            if (findStructFields(elem_type, env)) {
                return storeInlineStructLocal(flat, elem_type, packed,
                                              flow, env, callee, offset);
            }
            int elem_width = flattenedTypeWidth(elem_type, env);
            if (elem_width <= 0) {
                env.error = "Helper function '" + callee +
                            "' assigns array element with unknown width '" +
                            flat + "'";
                return false;
            }
            TypeInfo leaf_type = elem_type;
            leaf_type.width = elem_width;
            flow.types[flat] = leaf_type;
            flow.values[flat] = make_slice(cloneExpr(packed),
                                           offset + elem_width - 1,
                                           offset,
                                           leaf_type);
            offset += elem_width;
            return true;
        }
        for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
            prefix.push_back(i);
            if (!self(self, depth + 1)) return false;
            prefix.pop_back();
        }
        return true;
    };
    return store(store, 0);
}

ExprPtr orExpr(ExprPtr lhs, ExprPtr rhs) {
    if (!lhs) return rhs;
    if (!rhs) return lhs;
    if (isTrueLiteral(lhs) || isTrueLiteral(rhs)) {
        return make_literal("true", make_hw_type("bool", 1, false));
    }
    if (isFalseLiteral(lhs)) return rhs;
    if (isFalseLiteral(rhs)) return lhs;
    return make_binary("||", std::move(lhs), std::move(rhs),
                       make_hw_type("bool", 1, false));
}

ExprPtr rewriteInlineExpression(const ExprPtr& expr,
                                const InlineExpressionFlow& flow,
                                Env& env) {
    auto substituted = substituteInlineExpr(expr, flow.values);
    return rewriteExpr(substituted, env);
}

void appendInlineReturn(InlineExpressionFlow& flow, ExprPtr guard, ExprPtr value) {
    if (!flow.return_value) {
        flow.return_value = std::move(value);
        flow.return_guard = std::move(guard);
        return;
    }
    TypeInfo result_type = flow.return_value->type.width > 0
        ? flow.return_value->type
        : value->type;
    flow.return_value = make_ternary(cloneExpr(guard), std::move(value),
                                     std::move(flow.return_value), result_type);
    flow.return_guard = orExpr(std::move(flow.return_guard), std::move(guard));
}

bool lowerInlineExpressionSequence(const std::vector<StmtPtr>& statements,
                                   const std::string& callee,
                                   InlineExpressionFlow& flow,
                                   Env& env);

bool lowerInlineExpressionIf(const StmtPtr& stmt,
                             const std::string& callee,
                             InlineExpressionFlow& flow,
                             Env& env) {
    auto condition = rewriteInlineExpression(stmt->if_cond, flow, env);
    if (!condition || !env.error.empty()) return false;

    InlineExpressionFlow then_flow;
    then_flow.values = flow.values;
    then_flow.types = flow.types;
    then_flow.active = andExpr(cloneExpr(flow.active), cloneExpr(condition));

    InlineExpressionFlow else_flow;
    else_flow.values = flow.values;
    else_flow.types = flow.types;
    else_flow.active = andExpr(cloneExpr(flow.active), notExpr(cloneExpr(condition)));

    if (!lowerInlineExpressionSequence(stmt->if_then, callee, then_flow, env)) return false;
    if (!lowerInlineExpressionSequence(stmt->if_else, callee, else_flow, env)) return false;

    // Only names visible before the branch escape its local scopes. Branch-local
    // declarations remain local, while assignments to parameters/outer locals merge.
    for (auto& [name, old_value] : flow.values) {
        auto then_it = then_flow.values.find(name);
        auto else_it = else_flow.values.find(name);
        if (then_it == then_flow.values.end() || else_it == else_flow.values.end()) {
            env.error = "Helper function '" + callee +
                        "' has an uninitialized local after conditional assignment: '" +
                        name + "'";
            return false;
        }
        TypeInfo type = old_value && old_value->type.width > 0
            ? old_value->type
            : flow.types[name];
        flow.values[name] = make_ternary(cloneExpr(condition),
                                         cloneExpr(then_it->second),
                                         cloneExpr(else_it->second), type);
    }

    if (then_flow.return_value) {
        appendInlineReturn(flow, cloneExpr(then_flow.return_guard),
                           cloneExpr(then_flow.return_value));
    }
    if (else_flow.return_value) {
        appendInlineReturn(flow, cloneExpr(else_flow.return_guard),
                           cloneExpr(else_flow.return_value));
    }
    flow.active = orExpr(std::move(then_flow.active), std::move(else_flow.active));
    return true;
}

bool lowerInlineExpressionSequence(const std::vector<StmtPtr>& statements,
                                   const std::string& callee,
                                   InlineExpressionFlow& flow,
                                   Env& env) {
    for (const auto& stmt : statements) {
        if (!stmt || isFalseLiteral(flow.active)) continue;
        switch (stmt->kind) {
        case StmtKind::Decl: {
            flow.types[stmt->decl_name] = stmt->decl_type;
            TypeInfo decl_array_type = normalizeArrayType(stmt->decl_type);
            const std::string decl_struct_name = canonicalStructName(stmt->decl_type.name);
            std::string init_struct_name;
            if (stmt->decl_init.has_value() && stmt->decl_init.value() &&
                stmt->decl_init.value()->kind == ExprKind::Call &&
                stmt->decl_init.value()->args.empty()) {
                init_struct_name = canonicalStructName(stmt->decl_init.value()->callee);
            }
            const bool known_struct_type =
                !stmt->decl_type.struct_name.empty() ||
                env.struct_fields.count(stmt->decl_type.name) ||
                env.struct_fields.count(decl_struct_name) ||
                (!init_struct_name.empty() &&
                    (env.struct_fields.count(init_struct_name) ||
                     env.struct_fields.count("struct " + init_struct_name)));
            const bool empty_struct_constructor = stmt->decl_init.has_value() &&
                stmt->decl_init.value() &&
                stmt->decl_init.value()->kind == ExprKind::Call &&
                stmt->decl_init.value()->args.empty() &&
                known_struct_type;
            const bool empty_array_constructor = stmt->decl_init.has_value() &&
                stmt->decl_init.value() &&
                stmt->decl_init.value()->kind == ExprKind::Call &&
                stmt->decl_init.value()->args.empty() &&
                decl_array_type.is_array && !decl_array_type.array_dims.empty();
            if (empty_struct_constructor && !findStructFields(stmt->decl_type, env)) {
                break;
            }
            if (!stmt->decl_init.has_value()) {
                if (findStructFields(stmt->decl_type, env)) {
                    break;
                }
                if (decl_array_type.is_array && !decl_array_type.array_dims.empty()) {
                    std::vector<int> prefix;
                    auto add_types = [&](auto&& self, std::size_t depth) -> void {
                        if (depth >= decl_array_type.array_dims.size()) {
                            flow.types[joinIndexName(stmt->decl_name, prefix)] =
                                scalarTypeFromArray(decl_array_type);
                            return;
                        }
                        for (int i = 0; i < decl_array_type.array_dims[depth]; ++i) {
                            prefix.push_back(i);
                            self(self, depth + 1);
                            prefix.pop_back();
                        }
                    };
                    add_types(add_types, 0);
                    break;
                }
                env.error = "Helper function '" + callee +
                            "' declares uninitialized local '" + stmt->decl_name + "'";
                return false;
            }
            ExprPtr value;
            if (empty_array_constructor || empty_struct_constructor) {
                int width = flattenedTypeWidth(stmt->decl_type, env);
                if (width <= 0) {
                    env.error = "Helper function '" + callee +
                                "' default-initializes aggregate with unknown flattened width";
                    return false;
                }
                value = make_literal("0", make_hw_type("UInt", width, false));
            } else {
                value = rewriteInlineExpression(stmt->decl_init.value(), flow, env);
            }
            if (!value || !env.error.empty()) return false;
            if (findStructFields(stmt->decl_type, env)) {
                int packed_width = flattenedTypeWidth(stmt->decl_type, env);
                if (packed_width <= 0) {
                    env.error = "Helper function '" + callee +
                                "' declares struct local with unknown flattened width '" +
                                stmt->decl_name + "'";
                    return false;
                }
                ExprPtr packed = value;
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed,
                                                make_hw_type("UInt", packed_width, false));
                }
                int offset = 0;
                if (!storeInlineStructLocal(stmt->decl_name, stmt->decl_type,
                                            packed, flow, env, callee, offset)) {
                    return false;
                }
                break;
            }
            if (decl_array_type.is_array && !decl_array_type.array_dims.empty()) {
                int packed_width = flattenedTypeWidth(decl_array_type, env);
                if (packed_width <= 0) {
                    env.error = "Helper function '" + callee +
                                "' declares array local with unknown flattened width '" +
                                stmt->decl_name + "'";
                    return false;
                }
                ExprPtr packed = value;
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed,
                                                make_hw_type("UInt", packed_width, false));
                }
                int offset = 0;
                if (!storeInlineArrayLocal(stmt->decl_name, decl_array_type,
                                           packed, flow, env, callee, offset)) {
                    return false;
                }
                break;
            }
            flow.values[stmt->decl_name] = castIfWidthChanges(value, stmt->decl_type);
            break;
        }
        case StmtKind::Assign: {
            std::string name;
            TypeInfo type;
            bool require_existing = true;
            if (stmt->assign_target && stmt->assign_target->kind == ExprKind::VarRef) {
                name = stmt->assign_target->var_name;
                auto type_it = flow.types.find(name);
                if (type_it != flow.types.end()) type = type_it->second;
            } else if (auto field = inlineLocalFieldTarget(stmt->assign_target, flow, env)) {
                name = field->first;
                type = field->second;
                flow.types[name] = type;
                require_existing = false;
            } else if (auto element = inlineLocalArrayTarget(stmt->assign_target, flow)) {
                name = element->first;
                type = element->second;
                flow.types[name] = type;
                require_existing = false;
            } else {
                env.error = "Helper function '" + callee +
                            "' expression inline supports assignments only to local/value variables";
                return false;
            }
            auto old_it = flow.values.find(name);
            if (require_existing && old_it == flow.values.end()) {
                env.error = "Helper function '" + callee +
                            "' assigns unknown local/value variable '" + name + "'";
                return false;
            }
            auto value = rewriteInlineExpression(stmt->assign_value, flow, env);
            if (!value || !env.error.empty()) return false;
            if (type.width <= 0) {
                type = flow.types.count(name) ? flow.types[name] :
                       (old_it != flow.values.end() ? old_it->second->type : value->type);
            }
            if (findStructFields(type, env)) {
                int packed_width = flattenedTypeWidth(type, env);
                if (packed_width <= 0) {
                    env.error = "Helper function '" + callee +
                                "' assigns struct local with unknown flattened width '" +
                                name + "'";
                    return false;
                }
                ExprPtr packed = value;
                if (packed->type.width <= 0 || !packed->type.struct_name.empty()) {
                    packed = buildPackedStructValue(stmt->assign_value,
                                                    make_hw_type("UInt", packed_width, false),
                                                    env);
                    if (!packed || !env.error.empty()) return false;
                }
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed,
                                                make_hw_type("UInt", packed_width, false));
                }
                int offset = 0;
                if (!storeInlineStructLocal(name, type, packed, flow, env, callee, offset)) {
                    return false;
                }
                break;
            }
            TypeInfo assign_array_type = normalizeArrayType(type);
            if (assign_array_type.is_array && !assign_array_type.array_dims.empty()) {
                int packed_width = flattenedTypeWidth(assign_array_type, env);
                if (packed_width <= 0) {
                    env.error = "Helper function '" + callee +
                                "' assigns array local with unknown flattened width '" +
                                name + "'";
                    return false;
                }
                ExprPtr packed = value;
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed,
                                                make_hw_type("UInt", packed_width, false));
                }
                int offset = 0;
                if (!storeInlineArrayLocal(name, assign_array_type,
                                           packed, flow, env, callee, offset)) {
                    return false;
                }
                break;
            }
            value = castIfWidthChanges(value, type);
            if (isTrueLiteral(flow.active) || old_it == flow.values.end()) {
                flow.values[name] = std::move(value);
            } else {
                flow.values[name] = make_ternary(cloneExpr(flow.active), std::move(value),
                                                 cloneExpr(old_it->second), type);
            }
            break;
        }
        case StmtKind::If:
            if (!lowerInlineExpressionIf(stmt, callee, flow, env)) return false;
            break;
        case StmtKind::Block: {
            std::vector<std::string> visible_names;
            visible_names.reserve(flow.values.size());
            for (const auto& [name, value] : flow.values) visible_names.push_back(name);
            if (!lowerInlineExpressionSequence(stmt->block_stmts, callee, flow, env)) return false;
            std::unordered_set<std::string> visible(visible_names.begin(), visible_names.end());
            for (auto it = flow.values.begin(); it != flow.values.end();) {
                if (!visible.count(it->first)) {
                    flow.types.erase(it->first);
                    it = flow.values.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        }
        case StmtKind::Return: {
            if (!stmt->return_value.has_value()) {
                env.error = "Helper function '" + callee +
                            "' used as expression contains a void return";
                return false;
            }
            ExprPtr value;
            auto returned_local_name = [&](const ExprPtr& expr,
                                           const auto& self) -> std::string {
                if (!expr) return "";
                if (expr->kind == ExprKind::VarRef) return expr->var_name;
                if (expr->kind == ExprKind::Cast) return self(expr->cast_expr, self);
                if (expr->kind == ExprKind::Call && expr->args.size() == 1) {
                    const std::string callee_name = canonicalStructName(expr->callee);
                    const bool struct_wrapper =
                        env.struct_fields.count(callee_name) ||
                        env.struct_fields.count("struct " + callee_name) ||
                        findStructFields(expr->type, env) != nullptr;
                    if (struct_wrapper) return self(expr->args.front(), self);
                }
                return "";
            };
            const std::string name = returned_local_name(stmt->return_value.value(),
                                                         returned_local_name);
            if (!name.empty()) {
                auto type_it = flow.types.find(name);
                if (type_it != flow.types.end()) {
                    TypeInfo ret_array_type = normalizeArrayType(type_it->second);
                    if (ret_array_type.is_array && !ret_array_type.array_dims.empty()) {
                        value = packInlineArrayLocal(name, ret_array_type, flow, env, callee);
                    } else if (findStructFields(type_it->second, env)) {
                        value = packInlineStructLocal(name, type_it->second, flow, env, callee);
                    }
                }
            }
            if (!value) value = rewriteInlineExpression(stmt->return_value.value(), flow, env);
            if (!value || !env.error.empty()) return false;
            appendInlineReturn(flow, cloneExpr(flow.active), std::move(value));
            flow.active = make_literal("false", make_hw_type("bool", 1, false));
            break;
        }
        default:
            env.error = "Helper function '" + callee +
                        "' used as expression contains unsupported control flow or side effects";
            return false;
        }
    }
    return true;
}

ExprPtr inlineHelperCall(const ExprPtr& e, Env& env) {
    if (e->callee == "__bit" || e->callee == "__slice") {
        std::vector<ExprPtr> args;
        for (auto& arg : e->args) {
            args.push_back(rewriteExpr(arg, env));
            if (!env.error.empty()) return nullptr;
        }
        if (args.empty()) {
            env.error = "Unsupported bit/slice base expression";
            return nullptr;
        }
        int base_width = args.front() ? args.front()->type.width : 0;
        if (e->callee == "__bit") {
            if (args.size() < 2) {
                env.error = "bit_select requires static bit index";
                return nullptr;
            }
            int bit = literalIntValue(args[1], -1);
            if (bit < 0 || (base_width > 0 && bit >= base_width)) {
                env.error = "Bit select out of bounds";
                return nullptr;
            }
            return make_bit_select(args.front(), bit);
        }
        if (args.size() < 3) {
            env.error = "slice requires static hi/lo";
            return nullptr;
        }
        int hi = literalIntValue(args[1], -1);
        int lo = literalIntValue(args[2], -1);
        if (hi < 0 || lo < 0 || hi < lo || (base_width > 0 && hi >= base_width)) {
            env.error = "Slice out of bounds";
            return nullptr;
        }
        TypeInfo ty = make_hw_type(args.front()->type.is_signed ? "Int" : "UInt", hi - lo + 1, args.front()->type.is_signed);
        return make_slice(args.front(), hi, lo, ty);
    }

    if (e->callee == "__cat") {
        std::vector<ExprPtr> parts;
        for (auto& arg : e->args) {
            auto rewritten = rewriteExpr(arg, env);
            if (!env.error.empty()) return nullptr;
            parts.push_back(rewritten);
        }
        return make_concat(std::move(parts));
    }

    if (e->callee == "__repeat") {
        int count = e->args.empty() ? 0 : literalIntValue(e->args.front(), 0);
        if (e->args.size() < 2) {
            env.error = "repeat requires a value argument";
            return nullptr;
        }
        auto value = rewriteExpr(e->args[1], env);
        if (!env.error.empty()) return nullptr;
        if (count <= 0) {
            env.error = "repeat count must be a positive compile-time constant";
            return nullptr;
        }
        return make_repeat(value, count);
    }

    if (e->callee == "__unsupported_expr") {
        env.error = "Unsupported expression at " + e->literal_value;
        return nullptr;
    }

    if (e->intrinsic == IntrinsicKind::ReqHelperOutput || e->callee == "__vul_output") {
        ExprPtr receiver;
        std::vector<ExprPtr> values;
        if (!e->args.empty() && e->args.front() &&
            e->args.front()->kind == ExprKind::VarRef &&
            env.reqhelper_aliases.count(e->args.front()->var_name)) {
            receiver = e->args.front();
            values.assign(e->args.begin() + 1, e->args.end());
        } else {
            values = e->args;
        }
        auto alias = resolveReqHelperAlias(receiver, env);
        if (!alias.has_value()) {
            env.error = "Unsupported ReqHelper expression call without constructor alias";
            return nullptr;
        }
        if (alias->arg_data.empty() && !alias->ret_data.empty()) {
            if (!values.empty()) {
                env.error = "Unsupported ret-only ReqHelper expression call with arguments";
                return nullptr;
            }
            TypeInfo ret_type = symbolType(env, alias->ret_data, e->type);
            return make_var(alias->ret_data, ret_type);
        }
        env.error = "Unsupported ReqHelper output call used as value";
        return nullptr;
    }

    if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->callee == "__dynamic_range_at") {
        if (e->args.size() < 2) {
            env.error = "Unsupported dynamic range_at; requires receiver and index";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(rewriteExpr(e->args[0], env));
        if (!env.error.empty()) return nullptr;
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        if (out->type.width <= 0) {
            env.error = "Unsupported dynamic range_at with unknown width";
            return nullptr;
        }
        return out;
    }
    if (e->intrinsic == IntrinsicKind::DynamicBitAt || e->callee == "__dynamic_bit_at") {
        if (e->args.size() < 2) {
            env.error = "Unsupported dynamic bit_at; requires receiver and index";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(rewriteExpr(e->args[0], env));
        if (!env.error.empty()) return nullptr;
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        out->type = make_hw_type("bool", 1, false);
        return out;
    }

    if (e->callee == "lookup") {
        if (e->args.size() < 2) {
            env.error = "lookup requires table and index operands";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(cloneExpr(e->args.front()));
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        if (out->type.width <= 0) {
            env.error = "lookup output width must be known";
            return nullptr;
        }
        return out;
    }

    if (e->callee == "get") {
        if (e->args.empty()) {
            env.error = "Unsupported RegProxy get call without receiver";
            return nullptr;
        }
        std::string proxy;
        if (e->args.front() && e->args.front()->kind == ExprKind::FieldAccess) {
            proxy = baseName(e->args.front()->struct_base);
        } else {
            proxy = baseName(e->args.front());
        }
        auto value = readRegProxy(proxy, e->type, env);
        if (!value && env.error.empty()) {
            env.error = "Unsupported RegProxy get call without constructor alias for '" + proxy + "'";
        }
        return value;
    }

    if (auto proxy_method = lowerProxyMethodExpr(e, env)) {
        return proxy_method;
    }
    if (!env.error.empty()) return nullptr;

    if (e->callee.rfind("operator", 0) == 0) {
        std::string op = e->callee.substr(std::string("operator").size());
        op.erase(std::remove_if(op.begin(), op.end(), [](unsigned char c) {
            return std::isspace(c);
        }), op.end());
        if ((op == "+" || op == "-" || op == "*" || op == "&" || op == "|" ||
             op == "^" || op == "<<" || op == ">>" || op == "==" ||
             op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
             op == "/" || op == "%") &&
            e->args.size() >= 2) {
            auto lhs = rewriteExpr(e->args[e->args.size() - 2], env);
            if (!env.error.empty()) return nullptr;
            auto rhs = rewriteExpr(e->args[e->args.size() - 1], env);
            if (!env.error.empty()) return nullptr;
            normalizeConstantOperandsForBinary(op, lhs, rhs);
            if (op == "/" || op == "%") {
                TypeInfo builtin_result;
                if (unifyBuiltinDivRemOperands(lhs, rhs, builtin_result, env)) {
                    if (!env.error.empty()) return nullptr;
                    return make_binary(op, lhs, rhs, builtin_result);
                }
                return lowerPowerOfTwoDivRem(op, lhs, rhs, env);
            }
            if ((op == "&" || op == "|" || op == "^") &&
                lhs && rhs && lhs->type.width > 0 && rhs->type.width > 0 &&
                lhs->type.width != rhs->type.width) {
                if (isWidthCastableConstantExpr(lhs)) {
                    lhs = castIfWidthChanges(lhs, rhs->type);
                } else if (isWidthCastableConstantExpr(rhs)) {
                    rhs = castIfWidthChanges(rhs, lhs->type);
                } else if (widenBuiltinBitwiseOperands(lhs, rhs)) {
                    // C++ builtin uint8_t/uint32_t expressions are promoted
                    // before bitwise operations. Keep VUL Int/UInt mismatches
                    // fail-closed unless a builtin operand is present.
                } else {
                    env.error = IntSemantics::binaryResultType(op, lhs->type, rhs->type).error +
                        " for operator '" + op + "' lhs_width=" + std::to_string(lhs->type.width) +
                    " lhs_type='" + lhs->type.name + "/" + lhs->type.hw_kind +
                    "' rhs_width=" + std::to_string(rhs->type.width) +
                    " rhs_type='" + rhs->type.name + "/" + rhs->type.hw_kind + "'";
                return nullptr;
            }
            }
            if (auto folded = foldConstantOvershift(op, lhs, rhs)) return folded;
            return make_binary(op, lhs, rhs, resultTypeForBinary(op, lhs->type, rhs->type));
        }
        if ((op == "!" || op == "~" || op == "-") && !e->args.empty()) {
            auto operand = rewriteExpr(e->args.back(), env);
            if (!env.error.empty()) return nullptr;
            TypeInfo ty = op == "!" ? make_hw_type("bool", 1, false) : operand->type;
            return make_unary(op, operand, ty);
        }
    }

    // Clang represents aggregate copy initialization as a constructor-shaped
    // call named after the struct. For the restricted value semantics this is
    // an identity wrapper around the single source value; field unpacking is
    // performed by the declaration lowering that consumes it.
    TypeInfo call_array_type = normalizeArrayType(e->type);
    if (call_array_type.is_array && !call_array_type.array_dims.empty()) {
        int packed_width = flattenedTypeWidth(call_array_type, env);
        if (packed_width <= 0) {
            env.error = "Unsupported value-initialized array '" + e->callee +
                        "' with unknown flattened width";
            return nullptr;
        }
        if (e->args.empty()) {
            return make_literal("0", make_hw_type("UInt", packed_width, false));
        }
        TypeInfo elem_type = scalarTypeFromArray(call_array_type);
        std::vector<ExprPtr> parts;
        for (const auto& arg : e->args) {
            ExprPtr value;
            TypeInfo arg_type = arg ? arg->type : TypeInfo{};
            if (arg && arg->kind == ExprKind::VarRef && env.symbols.count(arg->var_name)) {
                arg_type = env.symbols[arg->var_name];
            }
            if (findStructFields(arg_type, env)) {
                int width = flattenedTypeWidth(arg_type, env);
                value = buildPackedStructValue(arg, make_hw_type("UInt", width, false), env);
            } else {
                value = rewriteExpr(arg, env);
            }
            if (!value || !env.error.empty()) return nullptr;
            parts.push_back(castIfWidthChanges(value, elem_type));
        }
        if (parts.empty()) {
            return make_literal("0", make_hw_type("UInt", packed_width, false));
        }
        std::reverse(parts.begin(), parts.end());
        auto packed = make_concat(std::move(parts));
        int concat_width = packed->type.width;
        packed->type = make_hw_type("UInt", concat_width > 0 ? concat_width : packed_width, false);
        return packed;
    }
    if (e->args.empty() && e->type.width > 0) {
        const std::string callee_type = canonicalStructName(e->callee);
        const std::string expr_type = canonicalStructName(e->type.name);
        if (!callee_type.empty() && callee_type == expr_type) {
            return make_literal("0", e->type);
        }
    }
    const std::string constructor_name = canonicalStructName(e->callee);
    bool is_struct_constructor = env.struct_fields.count(constructor_name) != 0 ||
                                 env.struct_fields.count("struct " + constructor_name) != 0;
    if (is_struct_constructor && e->args.empty()) {
        int width = flattenedTypeWidth(e->type, env);
        if (width <= 0) {
            TypeInfo constructor_type = e->type;
            constructor_type.struct_name = constructor_name;
            width = flattenedTypeWidth(constructor_type, env);
        }
        if (width <= 0) {
            env.error = "Unsupported value-initialized struct '" + constructor_name +
                        "' with unknown flattened width";
            return nullptr;
        }
        return make_literal("0", make_hw_type("UInt", width, false));
    }
    if (is_struct_constructor && !e->args.empty()) {
        for (const auto& arg : e->args) {
            TypeInfo arg_type = arg ? arg->type : TypeInfo{};
            if (arg && arg->kind == ExprKind::VarRef &&
                env.symbols.count(arg->var_name)) {
                arg_type = env.symbols[arg->var_name];
            }
            if (findStructFields(arg_type, env)) {
                int width = flattenedTypeWidth(arg_type, env);
                auto packed = buildPackedStructValue(
                    arg, make_hw_type("UInt", width, false), env);
                if (packed || !env.error.empty()) return packed;
            }
            auto value = rewriteExpr(arg, env);
            if (!value || !env.error.empty()) return nullptr;
            if (value->kind == ExprKind::VarRef) {
                auto sym = env.symbols.find(value->var_name);
                if (sym != env.symbols.end() && isProxyCarrierType(sym->second)) {
                    continue;
                }
            }
            return value;
        }
        env.error = "Unsupported struct constructor '" + e->callee +
                    "' without a lowerable value operand";
        return nullptr;
    }

    auto normalize_type_name = [](std::string name) {
        if (name.rfind("struct ", 0) == 0) name = name.substr(7);
        if (name.rfind("class ", 0) == 0) name = name.substr(6);
        return name;
    };
    auto lambda_return_from_operator_type = [&](const TypeInfo& type) -> std::string {
        std::string name = type.name;
        auto arrow = name.find("->");
        if (arrow == std::string::npos) return "";
        std::string ret = name.substr(arrow + 2);
        auto comment = ret.find("//");
        if (comment != std::string::npos) ret = ret.substr(0, comment);
        ret.erase(0, ret.find_first_not_of(" \t"));
        auto end = ret.find_last_not_of(" \t");
        if (end == std::string::npos) return "";
        ret.erase(end + 1);
        return normalize_type_name(ret);
    };

    std::string call_callee = e->callee;
    if (call_callee.rfind("__unsupported_operator_call_receiver", 0) == 0 &&
        e->args.empty()) {
        std::string expected_return = lambda_return_from_operator_type(e->type);
        std::string match;
        if (!expected_return.empty()) {
            for (const auto& [name, lambda] : env.lambdas) {
                if (name.rfind("__unsupported_operator_call_receiver", 0) == 0) continue;
                std::string lambda_return = normalize_type_name(lambda.return_type.name);
                if (lambda_return.empty() && !lambda.return_type.struct_name.empty()) {
                    lambda_return = normalize_type_name(lambda.return_type.struct_name);
                }
                if (lambda_return == expected_return) {
                    if (!match.empty()) {
                        env.error = "Ambiguous hidden lambda operator() call returning '" +
                                    expected_return + "'";
                        return nullptr;
                    }
                    match = name;
                }
            }
        }
        if (match.empty()) {
            env.error = "Unsupported hidden operator() call without recoverable lambda receiver";
            return nullptr;
        }
        call_callee = match;
    }
    if (call_callee.rfind("__unsupported_operator_call_receiver", 0) == 0) {
        env.error = "Unsupported hidden operator() call without recoverable lambda receiver";
        return nullptr;
    }

    const FunctionAST* helper_ptr = nullptr;
    auto helper_it = env.helpers.find(call_callee);
    if (helper_it != env.helpers.end()) helper_ptr = &helper_it->second;
    auto lambda_it = env.lambdas.find(call_callee);
    if (!helper_ptr && lambda_it != env.lambdas.end()) helper_ptr = &lambda_it->second;
    if (!helper_ptr) {
        env.error = "Unsupported function call '" + call_callee + "' args=" +
                    std::to_string(e->args.size()) +
                    " return_width=" + std::to_string(e->type.width) +
                    " return_type='" + e->type.name + "/" + e->type.hw_kind +
                    "/" + e->type.struct_name + "'";
        if (!e->args.empty() && e->args.front()) {
            env.error += " first_arg_kind=" + std::to_string(static_cast<int>(e->args.front()->kind));
            if (e->args.front()->kind == ExprKind::VarRef) {
                env.error += " first_arg=" + e->args.front()->var_name;
            } else if (e->args.front()->kind == ExprKind::FieldAccess) {
                env.error += " first_field=" + e->args.front()->field_name;
            }
        }
        return nullptr;
    }

    const auto& helper = *helper_ptr;
    std::vector<std::string> param_names;
    for (auto& p : helper.params) param_names.push_back(p.name);

    size_t arg_offset = 0;
    if (e->args.size() == param_names.size() + 1) {
        arg_offset = 1;
    }

    if (param_names.size() != e->args.size() - arg_offset) {
        env.error = "Helper function '" + call_callee + "' argument count mismatch: params=" +
            std::to_string(param_names.size()) + " args=" + std::to_string(e->args.size()) +
            " offset=" + std::to_string(arg_offset);
        return nullptr;
    }
    std::unordered_map<std::string, ExprPtr> arg_map;
    std::vector<std::string> used_refs;
    for (auto& stmt : helper.body) collectStmtVarRefs(stmt, used_refs);
    for (size_t i = 0; i < param_names.size(); ++i) {
        auto raw_arg = e->args[i + arg_offset];
        if (std::find(used_refs.begin(), used_refs.end(), param_names[i]) == used_refs.end()) {
            arg_map[param_names[i]] = cloneExpr(raw_arg);
            continue;
        }
        if (raw_arg && raw_arg->kind == ExprKind::VarRef) {
            TypeInfo arg_type = raw_arg->type;
            auto sym = env.symbols.find(raw_arg->var_name);
            if (sym != env.symbols.end()) arg_type = sym->second;
            if (arg_type.is_array && arg_type.array_dims.empty() && arg_type.array_size > 0) {
                arg_type.array_dims = {arg_type.array_size};
            }
            if (arg_type.is_array && !arg_type.array_dims.empty()) {
                std::vector<int> prefix;
                if (!allFlattenedElementsInitialized(raw_arg->var_name, arg_type, env, prefix, 0)) {
                    env.error = "Read of uninitialized array '" + raw_arg->var_name + "'";
                    return nullptr;
                }
                arg_map[param_names[i]] = cloneExpr(raw_arg);
                continue;
            }
        }
        auto arg = rewriteExpr(raw_arg, env);
        if (!env.error.empty()) return nullptr;
        arg_map[param_names[i]] = arg;
    }

    InlineExpressionFlow flow;
    flow.values = std::move(arg_map);
    for (std::size_t i = 0; i < helper.params.size(); ++i) {
        const auto& param = helper.params[i];
        flow.types[param.name] = param.type;
        TypeInfo param_array_type = normalizeArrayType(param.type);
        if (param_array_type.is_array && !param_array_type.array_dims.empty()) {
            TypeInfo elem_type = scalarTypeFromArray(param_array_type);
            auto base_it = flow.values.find(param.name);
            if (base_it == flow.values.end()) continue;
            std::vector<int> prefix;
            auto add_elements = [&](auto&& self, std::size_t depth, ExprPtr base) -> void {
                if (depth >= param_array_type.array_dims.size()) {
                    const std::string flat = joinIndexName(param.name, prefix);
                    flow.types[flat] = elem_type;
                    flow.values[flat] = base;
                    return;
                }
                for (int idx = 0; idx < param_array_type.array_dims[depth]; ++idx) {
                    prefix.push_back(idx);
                    auto access = make_array_access(
                        cloneExpr(base),
                        make_literal(std::to_string(idx), TypeInfo{"int", 32, true}),
                        depth + 1 == param_array_type.array_dims.size()
                            ? elem_type
                            : param_array_type);
                    self(self, depth + 1, std::move(access));
                    prefix.pop_back();
                }
            };
            add_elements(add_elements, 0, cloneExpr(base_it->second));
        }
    }
    if (!lowerInlineExpressionSequence(helper.body, e->callee, flow, env)) return nullptr;
    if (!flow.return_value) {
        env.error = "Helper function '" + e->callee + "' used as expression has no return value";
        return nullptr;
    }
    if (!isFalseLiteral(flow.active)) {
        env.error = "Helper function '" + e->callee +
                    "' does not return a value on every statically represented path";
        return nullptr;
    }
    TypeInfo helper_return_array = normalizeArrayType(helper.return_type);
    if (flow.return_value && flow.return_value->type.width <= 0 &&
        helper_return_array.is_array && !helper_return_array.array_dims.empty()) {
        std::string match;
        ExprPtr packed_match;
        for (const auto& [name, type] : flow.types) {
            TypeInfo candidate = normalizeArrayType(type);
            if (!candidate.is_array || candidate.array_dims != helper_return_array.array_dims) {
                continue;
            }
            if (!match.empty()) {
                env.error = "Helper function '" + e->callee +
                            "' has ambiguous array return slot";
                return nullptr;
            }
            match = name;
            packed_match = packInlineArrayLocal(name, candidate, flow, env, e->callee);
            if (!packed_match || !env.error.empty()) return nullptr;
        }
        if (packed_match) {
            flow.return_value = packed_match;
        }
    }
    if (flow.return_value && flow.return_value->type.width <= 0) {
        const auto* return_fields = findStructFields(helper.return_type, env);
        if (return_fields && !return_fields->empty()) {
            auto same_struct_type = [&](const TypeInfo& candidate) {
                const std::string want = canonicalStructName(
                    helper.return_type.struct_name.empty()
                        ? helper.return_type.name
                        : helper.return_type.struct_name);
                const std::string have = canonicalStructName(
                    candidate.struct_name.empty() ? candidate.name : candidate.struct_name);
                return !want.empty() && want == have;
            };
            std::string match;
            ExprPtr packed_match;
            for (const auto& [name, type] : flow.types) {
                if (!same_struct_type(type)) continue;
                if (!match.empty()) {
                    env.error = "Helper function '" + e->callee +
                                "' has ambiguous struct return slot";
                    return nullptr;
                }
                match = name;
                packed_match = packInlineStructLocal(name, type, flow, env, e->callee);
                if (!packed_match || !env.error.empty()) return nullptr;
            }
            if (packed_match) {
                flow.return_value = packed_match;
            } else {
                std::ostringstream detail;
                detail << "Helper function '" << e->callee
                       << "' returned width-unknown struct without a complete local return slot; types=[";
                bool first = true;
                for (const auto& [name, type] : flow.types) {
                    if (!first) detail << ",";
                    first = false;
                    detail << name << ":" << type.name << "/" << type.struct_name;
                }
                detail << "] values=[";
                first = true;
                for (const auto& [name, value] : flow.values) {
                    if (!first) detail << ",";
                    first = false;
                    detail << name << ":" << (value ? value->type.width : 0);
                }
                detail << "]";
                env.error = detail.str();
                return nullptr;
            }
        }
    }
    if (flow.return_value && flow.return_value->type.width > 0 &&
        findStructFields(helper.return_type, env)) {
        TypeInfo return_type = helper.return_type;
        return_type.width = flow.return_value->type.width;
        flow.return_value->type = return_type;
    }
    TypeInfo return_array_type = normalizeArrayType(e->type);
    if (!return_array_type.is_array) {
        return_array_type = normalizeArrayType(helper.return_type);
    }
    if (flow.return_value && return_array_type.is_array && !return_array_type.array_dims.empty()) {
        flow.return_value->type = make_hw_type("UInt", flow.return_value->type.width, false);
        return flow.return_value;
    }
    return castIfWidthChanges(flow.return_value, e->type);
}

void collectInlineLocalRenames(const std::vector<StmtPtr>& statements,
                               const std::vector<std::string>& parameter_names,
                               int inline_id,
                               std::unordered_map<std::string, ExprPtr>& substitutions) {
    for (const auto& stmt : statements) {
        if (!stmt) continue;
        if (stmt->kind == StmtKind::Decl && !stmt->decl_type.is_static &&
            stmt->decl_type.init_values.empty() &&
            std::find(parameter_names.begin(), parameter_names.end(), stmt->decl_name) ==
                parameter_names.end()) {
            substitutions.emplace(
                stmt->decl_name,
                make_var(stmt->decl_name + "__inl_" + std::to_string(inline_id),
                         stmt->decl_type));
        }
        collectInlineLocalRenames(stmt->if_then, parameter_names, inline_id, substitutions);
        collectInlineLocalRenames(stmt->if_else, parameter_names, inline_id, substitutions);
        collectInlineLocalRenames(stmt->block_stmts, parameter_names, inline_id, substitutions);
        collectInlineLocalRenames(stmt->for_body, parameter_names, inline_id, substitutions);
        collectInlineLocalRenames(stmt->while_body, parameter_names, inline_id, substitutions);
        if (stmt->for_init) {
            collectInlineLocalRenames({stmt->for_init}, parameter_names, inline_id, substitutions);
        }
        for (const auto& clause : stmt->switch_cases) {
            collectInlineLocalRenames(clause.body, parameter_names, inline_id, substitutions);
        }
    }
}

std::vector<StmtPtr> inlineProcedureCall(const ExprPtr& call, Env& env) {
    std::vector<StmtPtr> out;
    if (!call || call->kind != ExprKind::Call) return out;

    if (call->intrinsic == IntrinsicKind::RegProxySetNext || call->callee == "__vul_setnext") {
        if (call->args.size() < 3) {
            env.error = "Unsupported RegProxy setnext call";
            return out;
        }
        ExprPtr receiver = call->args[0];
        std::string proxy;
        if (receiver && receiver->kind == ExprKind::FieldAccess) {
            proxy = baseName(receiver->struct_base);
        } else {
            proxy = baseName(receiver);
        }
        int port = literalIntValue(call->args[1], -1);
        auto alias_it = env.regproxy_aliases.find(proxy);
        if (alias_it == env.regproxy_aliases.end()) {
            env.error = "Unsupported RegProxy setnext without constructor alias for '" + proxy + "'";
            return out;
        }
        std::string wdata = alias_it->second.wdata;
        std::string wen = alias_it->second.wen;
        TypeInfo wdata_type = env.symbols.count(wdata) ? env.symbols[wdata] : TypeInfo{};
        TypeInfo wen_type = env.symbols.count(wen) ? env.symbols[wen] : TypeInfo{"bool", 1, false};
        if (wdata_type.is_array && wdata_type.array_dims.empty() && wdata_type.array_size > 0) {
            wdata_type.array_dims = {wdata_type.array_size};
        }
        if (wen_type.is_array && wen_type.array_dims.empty() && wen_type.array_size > 0) {
            wen_type.array_dims = {wen_type.array_size};
        }
        ExprPtr element_index;
        ExprPtr next_value;
        if ((wdata_type.is_array || wen_type.is_array) && call->args.size() >= 4) {
            element_index = cloneExpr(call->args[2]);
            next_value = cloneExpr(call->args[3]);
        } else {
            if (port < 0) {
                if (wdata_type.is_array || wen_type.is_array) {
                    env.error = "Unsupported non-constant RegProxy port index";
                    return out;
                }
                port = 0;
            }
            element_index = make_literal(std::to_string(port), TypeInfo{"int", 32, true});
            if (call->args.size() < 3) {
                env.error = "Unsupported RegProxy setnext call without value";
                return out;
            }
            next_value = cloneExpr(call->args[2]);
        }
        TypeInfo wdata_scalar = wdata_type.is_array ? scalarTypeFromArray(wdata_type) : wdata_type;
        TypeInfo wen_scalar = wen_type.is_array ? scalarTypeFromArray(wen_type) : TypeInfo{"bool", 1, false};
        ExprPtr wtarget = wdata_type.is_array
            ? make_array_access(make_var(wdata, wdata_type), cloneExpr(element_index), wdata_scalar)
            : make_var(wdata, wdata_scalar);
        ExprPtr wentarget = wen_type.is_array
            ? make_array_access(make_var(wen, wen_type), cloneExpr(element_index), wen_scalar)
            : make_var(wen, wen_scalar);
        if (auto packed = buildPackedStructValue(next_value, wdata_scalar, env)) {
            next_value = packed;
        }
        if (!env.error.empty()) return out;
        out.push_back(makeAssignStmt(wtarget, castIfWidthChanges(next_value, wdata_scalar)));
        out.push_back(makeAssignStmt(wentarget, make_literal("true", TypeInfo{"bool", 1, false})));
        return out;
    }

    if (call->intrinsic == IntrinsicKind::ReqHelperOutput || call->callee == "__vul_output") {
        if (call->args.empty()) {
            env.error = "Unsupported output call without data argument";
            return out;
        }
        ExprPtr receiver;
        std::vector<ExprPtr> values;
        if (!call->args.empty() && call->args.front() &&
            call->args.front()->kind == ExprKind::VarRef &&
            env.reqhelper_aliases.count(call->args.front()->var_name)) {
            receiver = call->args.front();
            values.assign(call->args.begin() + 1, call->args.end());
        } else {
            values = call->args;
        }
        auto alias = resolveReqHelperAlias(receiver, env);
        if (!alias.has_value()) {
            env.error = "Unsupported output call without ReqHelper alias";
            return out;
        }
        if (!alias->vld.empty()) {
            TypeInfo valid_type = symbolType(env, alias->vld, TypeInfo{"bool", 1, false});
            out.push_back(makeAssignStmt(make_var(alias->vld, valid_type),
                                         make_literal("true", TypeInfo{"bool", 1, false})));
        }
        if (alias->arg_data.empty() && !alias->ret_data.empty()) {
            if (values.empty()) {
                // Ret-only query helpers are pure reads. As a statement they
                // have no side effect; expression lowering returns ret_data.
                return out;
            }
            if (values.size() != alias->ret_payloads.size()) {
                env.error = "Unsupported ReqHelper return call without data reference; callee='" +
                            call->callee + "' raw_args=" + std::to_string(call->args.size()) +
                            " values=" + std::to_string(values.size());
                if (!call->args.empty() && call->args.front() &&
                    call->args.front()->kind == ExprKind::VarRef) {
                    env.error += " first_arg='" + call->args.front()->var_name + "'";
                }
                return out;
            }
            for (std::size_t ret_index = 0; ret_index < alias->ret_payloads.size(); ++ret_index) {
                ExprPtr target = values[ret_index];
                if (!target) {
                    env.error = "Unsupported ReqHelper return call without data reference";
                    return out;
                }
                const std::string& port = alias->ret_payloads[ret_index].second;
                TypeInfo ret_type = symbolType(env, port, target->type);
                TypeInfo target_type = target->type;
                if (target->kind == ExprKind::VarRef && env.symbols.count(target->var_name)) {
                    target_type = env.symbols[target->var_name];
                }
                if (target->kind == ExprKind::VarRef && findStructFields(target_type, env)) {
                    auto block = std::make_shared<Stmt>();
                    block->kind = StmtKind::Block;
                    int offset = 0;
                    if (!appendStructUnpackDecls(block, target->var_name, target_type,
                                                 make_var(port, ret_type), offset, env)) {
                        env.error = "Unsupported ReqHelper return unpack for '" +
                                    target->var_name + "'";
                        return out;
                    }
                    out.push_back(std::move(block));
                } else {
                    out.push_back(makeAssignStmt(cloneExpr(target), make_var(port, ret_type)));
                }
            }
            return out;
        }
        if (alias->payloads.empty()) {
            env.error = "Unsupported ReqHelper output call without payload alias";
            return out;
        }
        const std::size_t expected_values = alias->payloads.size() + alias->ret_payloads.size();
        if (values.size() != expected_values) {
            env.error = "ReqHelper payload argument count mismatch: expected " +
                        std::to_string(expected_values) + ", got " +
                        std::to_string(values.size());
            return out;
        }

        for (std::size_t payload_index = 0; payload_index < alias->payloads.size(); ++payload_index) {
            const std::string& port = alias->payloads[payload_index].second;
            ExprPtr value = values[payload_index];
            TypeInfo data_type = symbolType(env, port, value ? value->type : TypeInfo{});
            TypeInfo value_type = value ? value->type : TypeInfo{};
            if (value && value->kind == ExprKind::VarRef && env.symbols.count(value->var_name)) {
                value_type = env.symbols[value->var_name];
            }
            if (value_type.is_array) {
                TypeInfo elem_type = scalarTypeFromArray(value_type);
                int elem_width = std::max(1, elem_type.width);
                int count = data_type.width > 0 ? data_type.width / elem_width : 0;
                if (count <= 0 || data_type.width % elem_width != 0) {
                    env.error = "Unsupported ReqHelper array payload pack width mismatch";
                    return out;
                }
                out.push_back(makeAssignStmt(make_var(port, data_type),
                                             make_literal("0", data_type)));
                for (int i = 0; i < count; ++i) {
                    auto elem = make_array_access(cloneExpr(value),
                                                  make_literal(std::to_string(i), TypeInfo{"int", 32, true}),
                                                  elem_type);
                    elem = rewriteExpr(elem, env);
                    if (!elem || !env.error.empty()) return out;
                    auto write = make_write_slice(make_var(port, data_type),
                                                  i * elem_width + elem_width - 1,
                                                  i * elem_width,
                                                  elem,
                                                  data_type);
                    out.push_back(makeAssignStmt(make_var(port, data_type), write));
                }
                continue;
            }
            if (auto packed = buildPackedStructValue(value, data_type, env)) {
                value = packed;
            }
            if (!env.error.empty()) return out;
            out.push_back(makeAssignStmt(make_var(port, data_type),
                                         castIfWidthChanges(cloneExpr(value), data_type)));
        }
        for (std::size_t ret_index = 0; ret_index < alias->ret_payloads.size(); ++ret_index) {
            ExprPtr target = values[alias->payloads.size() + ret_index];
            if (!target) {
                env.error = "Unsupported ReqHelper return call without data reference";
                return out;
            }
            const std::string& port = alias->ret_payloads[ret_index].second;
            TypeInfo ret_type = symbolType(env, port, target->type);
            TypeInfo target_type = target->type;
            if (target->kind == ExprKind::VarRef && env.symbols.count(target->var_name)) {
                target_type = env.symbols[target->var_name];
            }
            if (target->kind == ExprKind::VarRef && findStructFields(target_type, env)) {
                auto block = std::make_shared<Stmt>();
                block->kind = StmtKind::Block;
                int offset = 0;
                if (!appendStructUnpackDecls(block, target->var_name, target_type,
                                             make_var(port, ret_type), offset, env)) {
                    env.error = "Unsupported ReqHelper return unpack for '" +
                                target->var_name + "'";
                    return out;
                }
                out.push_back(std::move(block));
            } else {
                out.push_back(makeAssignStmt(cloneExpr(target), make_var(port, ret_type)));
            }
        }
        return out;
    }

    auto proxy_call = lowerProxyProcedureCall(call, env);
    if (!env.error.empty()) return out;
    if (!proxy_call.empty()) return proxy_call;

    auto normalize_type_name = [](std::string name) {
        if (name.rfind("struct ", 0) == 0) name = name.substr(7);
        if (name.rfind("class ", 0) == 0) name = name.substr(6);
        return name;
    };
    auto lambda_return_from_operator_type = [&](const TypeInfo& type) -> std::string {
        std::string name = type.name;
        auto arrow = name.find("->");
        if (arrow == std::string::npos) return "";
        std::string ret = name.substr(arrow + 2);
        auto comment = ret.find("//");
        if (comment != std::string::npos) ret = ret.substr(0, comment);
        ret.erase(0, ret.find_first_not_of(" \t"));
        auto end = ret.find_last_not_of(" \t");
        if (end == std::string::npos) return "";
        ret.erase(end + 1);
        return normalize_type_name(ret);
    };
    std::string call_callee = call->callee;
    if (call_callee.rfind("__unsupported_operator_call_receiver", 0) == 0 &&
        call->args.empty()) {
        std::string expected_return = lambda_return_from_operator_type(call->type);
        std::string match;
        if (!expected_return.empty()) {
            for (const auto& [name, lambda] : env.lambdas) {
                if (name.rfind("__unsupported_operator_call_receiver", 0) == 0) continue;
                std::string lambda_return = normalize_type_name(lambda.return_type.name);
                if (lambda_return.empty() && !lambda.return_type.struct_name.empty()) {
                    lambda_return = normalize_type_name(lambda.return_type.struct_name);
                }
                if (lambda_return == expected_return) {
                    if (!match.empty()) {
                        env.error = "Ambiguous hidden lambda operator() call returning '" +
                                    expected_return + "'";
                        return out;
                    }
                    match = name;
                }
            }
        }
        if (match.empty()) {
            env.error = "Unsupported hidden operator() call without recoverable lambda receiver";
            return out;
        }
        call_callee = match;
    }
    if (call_callee.rfind("__unsupported_operator_call_receiver", 0) == 0) {
        env.error = "Unsupported hidden operator() call without recoverable lambda receiver";
        return out;
    }

    const FunctionAST* helper_ptr = nullptr;
    auto helper_it = env.helpers.find(call_callee);
    if (helper_it != env.helpers.end()) helper_ptr = &helper_it->second;
    auto lambda_it = env.lambdas.find(call_callee);
    if (!helper_ptr && lambda_it != env.lambdas.end()) helper_ptr = &lambda_it->second;
    if (!helper_ptr) {
        env.error = "Unsupported function call '" + call_callee + "'";
        return out;
    }

    const auto& helper = *helper_ptr;
    std::vector<std::string> param_names;
    for (auto& p : helper.params) param_names.push_back(p.name);
    size_t arg_offset = call->args.size() == param_names.size() + 1 ? 1 : 0;
    if (param_names.size() != call->args.size() - arg_offset) {
        env.error = "Helper function '" + call_callee + "' argument count mismatch";
        return out;
    }

    std::unordered_map<std::string, ExprPtr> arg_map;
    for (size_t i = 0; i < param_names.size(); ++i) {
        arg_map[param_names[i]] = cloneExpr(call->args[i + arg_offset]);
    }
    int inline_id = (*env.inline_counter)++;
    collectInlineLocalRenames(helper.body, param_names, inline_id, arg_map);
    out = substituteInlineStmts(helper.body, arg_map);
    out = localizeProcedureReturns(out, call_callee, env.error);
    return out;
}

const std::vector<StructFieldInfo>* structFieldsForType(const TypeInfo& type, const Env& env) {
    for (const auto& key : {type.struct_name, type.name, std::string("struct ") + type.struct_name}) {
        if (key.empty()) continue;
        auto it = env.struct_fields.find(key);
        if (it != env.struct_fields.end()) return &it->second;
    }
    return nullptr;
}

TypeInfo normalizeArrayType(TypeInfo type) {
    if (type.is_array && type.array_dims.empty() && type.array_size > 0) {
        type.array_dims = {type.array_size};
    }
    return type;
}

std::string initializerArgForField(const StmtPtr& decl,
                                   const std::string& field,
                                   const Env& env) {
    if (!decl) return {};
    const auto* fields = structFieldsForType(decl->decl_type, env);
    if (!fields) return {};

    const std::vector<StructConstructorInfo>* constructors = nullptr;
    for (const auto& key : {decl->decl_type.struct_name, decl->decl_type.name,
                            std::string("struct ") + decl->decl_type.struct_name}) {
        if (key.empty()) continue;
        auto it = env.struct_constructors.find(key);
        if (it != env.struct_constructors.end()) {
            constructors = &it->second;
            break;
        }
    }
    if (constructors) {
        for (const auto& ctor : *constructors) {
            if (ctor.param_names.size() != decl->decl_init_args.size()) continue;
            auto mapped = ctor.field_to_param.find(field);
            if (mapped == ctor.field_to_param.end()) continue;
            auto param = std::find(ctor.param_names.begin(), ctor.param_names.end(), mapped->second);
            if (param == ctor.param_names.end()) continue;
            size_t index = static_cast<size_t>(std::distance(ctor.param_names.begin(), param));
            return index < decl->decl_init_args.size() ? directVarName(decl->decl_init_args[index]) : std::string{};
        }
    }
    for (size_t i = 0; i < fields->size() && i < decl->decl_init_args.size(); ++i) {
        if ((*fields)[i].name == field) return directVarName(decl->decl_init_args[i]);
    }
    return {};
}

ExprPtr buildArrayMux(const std::string& name,
                      const std::vector<int>& dims,
                      const std::vector<ExprPtr>& indices,
                      size_t depth,
                      std::vector<int>& prefix,
                      TypeInfo scalar_type) {
    if (depth >= indices.size() || depth >= dims.size()) {
        return make_var(joinIndexName(name, prefix), scalar_type);
    }

    int dim = dims[depth];
    prefix.push_back(dim - 1);
    ExprPtr result = buildArrayMux(name, dims, indices, depth + 1, prefix, scalar_type);
    prefix.pop_back();

    for (int i = dim - 2; i >= 0; --i) {
        prefix.push_back(i);
        auto value = buildArrayMux(name, dims, indices, depth + 1, prefix, scalar_type);
        prefix.pop_back();
        auto cond = make_binary("==",
            cloneExpr(indices[depth]),
            make_literal(std::to_string(i), indices[depth] ? indices[depth]->type : TypeInfo{"int", 32, true}),
            TypeInfo{"bool", 1, false});
        result = make_ite(cond, value, result, scalar_type);
    }

    return result;
}

void collectFlattenedArrayVars(const std::string& name,
                               const TypeInfo& arr_type,
                               std::vector<int>& prefix,
                               size_t depth,
                               std::vector<ExprPtr>& args,
                               int& width,
                               bool& is_signed) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        args.push_back(make_var(joinIndexName(name, prefix), scalar));
        width += scalar.width;
        is_signed = is_signed || scalar.is_signed;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        collectFlattenedArrayVars(name, arr_type, prefix, depth + 1, args, width, is_signed);
        prefix.pop_back();
    }
}

ExprPtr buildPackedArrayValue(const std::string& name, TypeInfo arr_type, Env& env) {
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    std::vector<ExprPtr> args;
    int width = 0;
    bool is_signed = false;
    std::vector<int> prefix;
    prefix.clear();
    collectFlattenedArrayVars(name, arr_type, prefix, 0, args, width, is_signed);
    std::reverse(args.begin(), args.end());

    return make_concat(std::move(args));
}

ExprPtr parseOpaqueArrayVar(const ExprPtr& e) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (!e || e->kind != ExprKind::VarRef) return nullptr;
    std::string text = e->var_name;
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    auto b = text.find('[');
    if (b == std::string::npos) return nullptr;
    std::string base = text.substr(0, b);
    if (base.empty()) return nullptr;
    ExprPtr out = make_var(base);
    size_t pos = b;
    while (pos < text.size()) {
        auto l = text.find('[', pos);
        if (l == std::string::npos) break;
        auto r = text.find(']', l);
        if (r == std::string::npos) return nullptr;
        std::string idx_text = text.substr(l + 1, r - l - 1);
        ExprPtr idx;
        if (!idx_text.empty() && std::all_of(idx_text.begin(), idx_text.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            idx = make_literal(idx_text, TypeInfo{"int", 32, true});
        } else {
            idx = make_var(idx_text, TypeInfo{"int", 32, true});
        }
        out = make_array_access(out, idx, e->type);
        pos = r + 1;
    }
    return out;
}

ExprPtr parseOpaqueHwMethodVar(const ExprPtr& e, Env& env) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (!e || e->kind != ExprKind::VarRef) return nullptr;
    (void)env;
    return nullptr;
}

ExprPtr buildPackedFlattenedPrefix(const std::string& name, Env& env) {
    std::vector<ExprPtr> args;
    int width = 0;
    bool is_signed = false;
    for (int i = 0;; ++i) {
        std::string flat = name + "_" + std::to_string(i);
        auto it = env.symbols.find(flat);
        if (it == env.symbols.end()) break;
        args.push_back(make_var(flat, it->second));
        width += it->second.width;
        is_signed = is_signed || it->second.is_signed;
    }
    if (args.empty()) return nullptr;
    std::reverse(args.begin(), args.end());
    return make_concat(std::move(args));
}

ExprPtr buildPackedStructValue(const ExprPtr& value, TypeInfo target_type, Env& env) {
    if (!value) return nullptr;
    TypeInfo value_type = value->type;
    if (value->kind == ExprKind::VarRef && env.symbols.count(value->var_name)) {
        value_type = env.symbols[value->var_name];
    }
    const auto* fields = findStructFields(value_type, env);
    if (!fields) return nullptr;
    std::vector<ExprPtr> parts;
    int width = 0;
    for (const auto& field : *fields) {
        auto field_expr = make_field_access(cloneExpr(value), field.name, field.type);
        ExprPtr part;
        if (findStructFields(field.type, env)) {
            part = buildPackedStructValue(field_expr, field.type, env);
        } else {
            part = rewriteExpr(field_expr, env);
        }
        if (!env.error.empty()) return nullptr;
        if (!part || part->type.width <= 0) {
            env.error = "Unsupported struct pack field without known width";
            return nullptr;
        }
        width += part->type.width;
        parts.push_back(part);
    }
    if (parts.empty()) return nullptr;
    std::reverse(parts.begin(), parts.end());
    auto packed = make_concat(std::move(parts));
    if (target_type.width > 0 && width != target_type.width) {
        packed = castIfWidthChanges(packed, target_type);
    }
    return packed;
}

void addFlattenedArraySymbols(Env& env,
                              const std::string& name,
                              const TypeInfo& type,
                              bool initialized,
                              std::vector<int>& prefix,
                              size_t depth) {
    if (depth >= type.array_dims.size()) {
        auto flat = joinIndexName(name, prefix);
        env.symbols[flat] = scalarTypeFromArray(type);
        if (initialized) env.initialized.insert(flat);
        return;
    }
    for (int i = 0; i < type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        addFlattenedArraySymbols(env, name, type, initialized, prefix, depth + 1);
        prefix.pop_back();
    }
}

bool allFlattenedElementsInitialized(const std::string& name,
                                     const TypeInfo& arr_type,
                                     Env& env,
                                     std::vector<int>& prefix,
                                     size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        return env.initialized.count(joinIndexName(name, prefix)) > 0;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        bool ok = allFlattenedElementsInitialized(name, arr_type, env, prefix, depth + 1);
        prefix.pop_back();
        if (!ok) return false;
    }
    return true;
}

std::string firstUninitializedFlattenedElement(const std::string& name,
                                               const TypeInfo& arr_type,
                                               Env& env,
                                               std::vector<int>& prefix,
                                               size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        std::string flat = joinIndexName(name, prefix);
        return env.initialized.count(flat) ? "" : flat;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        std::string missing = firstUninitializedFlattenedElement(name, arr_type, env, prefix, depth + 1);
        prefix.pop_back();
        if (!missing.empty()) return missing;
    }
    return "";
}

bool validateLiteralBounds(const std::string& name,
                           const std::vector<ExprPtr>& indices,
                           const std::vector<int>& dims,
                           Env& env) {
    for (size_t i = 0; i < indices.size() && i < dims.size(); ++i) {
        auto lit = literalIndex(indices[i]);
        if (!lit.has_value()) continue;
        if (*lit < 0 || *lit >= dims[i]) {
            env.error = "Array index out of bounds for '" + name + "' at dimension " +
                std::to_string(i) + ": " + std::to_string(*lit) +
                " not in [0, " + std::to_string(dims[i]) + ")";
            return false;
        }
    }
    return true;
}

const std::vector<StructFieldInfo>* findStructFields(const TypeInfo& type, Env& env) {
    std::string canonical = canonicalStructName(type.struct_name.empty() ? type.name : type.struct_name);
    for (const auto& key : {type.struct_name, type.name, canonical, std::string("struct ") + canonical}) {
        if (key.empty()) continue;
        auto it = env.struct_fields.find(key);
        if (it != env.struct_fields.end()) return &it->second;
    }
    return nullptr;
}

const std::vector<StructConstructorInfo>* findStructConstructors(const TypeInfo& type, Env& env) {
    if (!type.struct_name.empty()) {
        auto direct = env.struct_constructors.find(type.struct_name);
        if (direct != env.struct_constructors.end()) return &direct->second;
    }
    std::string canonical = canonicalStructName(type.struct_name.empty() ? type.name : type.struct_name);
    auto it = env.struct_constructors.find(canonical);
    if (it != env.struct_constructors.end()) return &it->second;
    auto struct_it = env.struct_constructors.find("struct " + canonical);
    if (struct_it != env.struct_constructors.end()) return &struct_it->second;
    return nullptr;
}

int flattenedTypeWidth(const TypeInfo& type, Env& env) {
    TypeInfo arr_type = normalizeArrayType(type);
    if (arr_type.is_array && !arr_type.array_dims.empty()) {
        TypeInfo elem_type = scalarTypeFromArray(arr_type);
        int elem_width = flattenedTypeWidth(elem_type, env);
        if (elem_width <= 0) return 0;
        int count = 1;
        for (int dim : arr_type.array_dims) count *= std::max(1, dim);
        return elem_width * count;
    }
    if (type.width > 0) return type.width;
    const auto* fields = findStructFields(type, env);
    if (!fields || fields->empty()) return 0;
    int width = 0;
    for (const auto& field : *fields) {
        int field_width = flattenedTypeWidth(field.type, env);
        if (field_width <= 0) return 0;
        width += field_width;
    }
    return width;
}

bool appendStructUnpackDecls(const StmtPtr& block,
                             const std::string& prefix,
                             const TypeInfo& type,
                             const ExprPtr& packed,
                             int& offset,
                             Env& env) {
    const auto* fields = findStructFields(type, env);
    if (!fields || fields->empty()) return false;
    for (const auto& field : *fields) {
        const std::string flat_name = prefix + "_" + field.name;
        if (findStructFields(field.type, env)) {
            if (!appendStructUnpackDecls(block, flat_name, field.type, packed, offset, env)) {
                return false;
            }
            markScalarFullyInitialized(env, flat_name, field.type);
            continue;
        }
        int field_width = flattenedTypeWidth(field.type, env);
        if (field_width <= 0) return false;
        TypeInfo leaf_type = field.type;
        leaf_type.width = field_width;
        env.symbols[flat_name] = leaf_type;
        markScalarFullyInitialized(env, flat_name, leaf_type);
        auto field_decl = std::make_shared<Stmt>();
        field_decl->kind = StmtKind::Decl;
        field_decl->decl_name = flat_name;
        field_decl->decl_type = leaf_type;
        field_decl->decl_init = make_slice(cloneExpr(packed),
                                           offset + field_width - 1,
                                           offset,
                                           leaf_type);
        block->block_stmts.push_back(std::move(field_decl));
        offset += field_width;
    }
    return true;
}

bool appendDefaultConstructedVulFields(const StmtPtr& block,
                                       const std::string& prefix,
                                       const TypeInfo& type,
                                       Env& env) {
    const auto* fields = findStructFields(type, env);
    if (!fields) return false;
    bool added = false;
    for (const auto& field : *fields) {
        if (field.type.is_reference || field.type.is_pointer) {
            continue;
        }
        const std::string flat_name = prefix + "_" + field.name;
        if (findStructFields(field.type, env)) {
            added = appendDefaultConstructedVulFields(
                        block, flat_name, field.type, env) || added;
            continue;
        }
        TypeInfo field_type = field.type;
        if (field_type.is_array && field_type.array_dims.empty() &&
            field_type.array_size > 0) {
            field_type.array_dims = {field_type.array_size};
        }
        const bool vul_zero_default =
            field_type.is_hw_int ||
            field_type.hw_kind == "Int" || field_type.hw_kind == "UInt" ||
            field_type.name.rfind("Int<", 0) == 0 ||
            field_type.name.rfind("UInt<", 0) == 0;
        if (!vul_zero_default) continue;

        auto add_decl = [&](const std::string& name, TypeInfo leaf_type) {
            if (leaf_type.is_array) leaf_type = scalarTypeFromArray(leaf_type);
            env.symbols[name] = leaf_type;
            markScalarFullyInitialized(env, name, leaf_type);
            auto decl = std::make_shared<Stmt>();
            decl->kind = StmtKind::Decl;
            decl->decl_name = name;
            decl->decl_type = leaf_type;
            decl->decl_init = make_literal("0", leaf_type);
            block->block_stmts.push_back(std::move(decl));
            added = true;
        };

        if (!field_type.is_array || field_type.array_dims.empty()) {
            add_decl(flat_name, field_type);
            continue;
        }
        std::vector<int> indices;
        std::function<void(size_t)> add_elements = [&](size_t depth) {
            if (depth == field_type.array_dims.size()) {
                add_decl(joinIndexName(flat_name, indices), field_type);
                return;
            }
            for (int i = 0; i < field_type.array_dims[depth]; ++i) {
                indices.push_back(i);
                add_elements(depth + 1);
                indices.pop_back();
            }
        };
        add_elements(0);
    }
    return added;
}

bool checkStructOutputInitialized(const std::string& base,
                                  const TypeInfo& type,
                                  Env& env,
                                  std::string& missing) {
    auto fields = findStructFields(type, env);
    if (fields) {
        for (auto& field : *fields) {
            if (!checkStructOutputInitialized(base + "_" + field.name, field.type, env, missing)) {
                return false;
            }
        }
        return true;
    }

    TypeInfo t = type;
    if (t.is_array && t.array_dims.empty() && t.array_size > 0) t.array_dims = {t.array_size};
    if (t.is_array && !t.array_dims.empty()) {
        std::vector<int> prefix;
        missing = firstUninitializedFlattenedElement(base, t, env, prefix, 0);
        return missing.empty();
    }

    if (!env.initialized.count(base)) {
        missing = base;
        return false;
    }
    return true;
}

void addStructFieldSymbols(const std::string& base,
                           const TypeInfo& type,
                           Env& env,
                           bool initialized) {
    auto fields = findStructFields(type, env);
    if (!fields) {
        return;
    }
    for (auto& field : *fields) {
        std::string field_name = base + "_" + field.name;
        TypeInfo field_type = field.type;
        if (findStructFields(field_type, env)) {
            addStructFieldSymbols(field_name, field_type, env, initialized);
            continue;
        }
        if (field_type.is_array && field_type.array_dims.empty() && field_type.array_size > 0) {
            field_type.array_dims = {field_type.array_size};
        }
        env.symbols[field_name] = field_type;
        if (field_type.is_array && !field_type.array_dims.empty()) {
            std::vector<int> prefix;
            addFlattenedArraySymbols(env, field_name, field_type, initialized, prefix, 0);
        } else if (initialized) {
            env.initialized.insert(field_name);
        }
    }
}

void expandArrayInitRec(const std::string& name,
                        const TypeInfo& arr_type,
                        Env& env,
                        std::vector<StmtPtr>& out,
                        std::vector<int>& prefix,
                        int& flat_index,
                        size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        std::string value = "0";
        if (flat_index < static_cast<int>(arr_type.init_values.size())) {
            value = arr_type.init_values[flat_index];
        }
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Assign;
        stmt->assign_target = make_var(joinIndexName(name, prefix), scalar);
        stmt->assign_value = make_literal(value, scalar);
        out.push_back(stmt);
        env.symbols[joinIndexName(name, prefix)] = scalar;
        env.initialized.insert(joinIndexName(name, prefix));
        ++flat_index;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandArrayInitRec(name, arr_type, env, out, prefix, flat_index, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewriteArrayInitDecl(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || s->kind != StmtKind::Decl || !s->decl_type.is_array ||
        s->decl_type.init_values.empty()) {
        return out;
    }
    TypeInfo arr_type = s->decl_type;
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
    if (arr_type.array_dims.empty()) return out;
    if (arr_type.is_static) return out;

    env.symbols[s->decl_name] = arr_type;
    std::vector<int> prefix;
    addFlattenedArraySymbols(env, s->decl_name, arr_type, false, prefix, 0);
    prefix.clear();
    int flat_index = 0;
    expandArrayInitRec(s->decl_name, arr_type, env, out, prefix, flat_index, 0);
    if (env.error.empty()) env.initialized.insert(s->decl_name);
    return out;
}

void expandPackedArrayInitRec(const std::string& name,
                              const TypeInfo& arr_type,
                              const ExprPtr& packed,
                              Env& env,
                              std::vector<StmtPtr>& out,
                              std::vector<int>& prefix,
                              int& bit_offset,
                              size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        int width = std::max(1, scalar.width);
        if (packed && packed->type.width > 0 && bit_offset + width - 1 >= packed->type.width) {
            env.error = "Slice out of bounds while unpacking array '" + name + "'";
            return;
        }
        auto slice = make_slice(cloneExpr(packed), bit_offset + width - 1, bit_offset, scalar);

        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Assign;
        auto flat = joinIndexName(name, prefix);
        stmt->assign_target = make_var(flat, scalar);
        stmt->assign_value = slice;
        out.push_back(stmt);
        env.symbols[flat] = scalar;
        env.initialized.insert(flat);
        bit_offset += width;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandPackedArrayInitRec(name, arr_type, packed, env, out, prefix, bit_offset, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewritePackedArrayInitDecl(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || s->kind != StmtKind::Decl || !s->decl_type.is_array ||
        !s->decl_init.has_value()) {
        return out;
    }
    TypeInfo arr_type = s->decl_type;
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
    if (arr_type.array_dims.empty()) return out;

    env.symbols[s->decl_name] = arr_type;
    std::vector<int> prefix;
    addFlattenedArraySymbols(env, s->decl_name, arr_type, false, prefix, 0);

    ExprPtr packed;
    auto init = s->decl_init.value();
    ExprPtr init_base;
    std::vector<ExprPtr> init_indices;
    if (collectArrayAccess(init, init_base, init_indices) && init_base) {
        TypeInfo init_array_type = init_base->type;
        std::string init_base_name = baseName(init_base);
        if (!init_base_name.empty() && env.symbols.count(init_base_name)) {
            init_array_type = env.symbols[init_base_name];
        }
        init_array_type = normalizeArrayType(init_array_type);
        if (init_array_type.is_array &&
            init_indices.size() == init_array_type.array_dims.size()) {
            auto value = rewriteExpr(init, env);
            if (!value || !env.error.empty()) return out;
            TypeInfo scalar = value->type;
            if (scalar.is_array) scalar = scalarTypeFromArray(init_array_type);
            std::vector<int> cleanup_prefix;
            auto cleanup = [&](auto&& self, std::size_t depth) -> void {
                if (depth >= arr_type.array_dims.size()) {
                    const std::string flat = joinIndexName(s->decl_name, cleanup_prefix);
                    env.symbols.erase(flat);
                    env.initialized.erase(flat);
                    return;
                }
                for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
                    cleanup_prefix.push_back(i);
                    self(self, depth + 1);
                    cleanup_prefix.pop_back();
                }
            };
            cleanup(cleanup, 0);
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::Decl;
            stmt->decl_name = s->decl_name;
            stmt->decl_type = scalar;
            stmt->decl_init = castIfWidthChanges(value, scalar);
            env.symbols[s->decl_name] = scalar;
            markScalarFullyInitialized(env, s->decl_name, scalar);
            out.push_back(std::move(stmt));
            return out;
        }
    }
    std::string init_name = directVarName(init);
    if (!init_name.empty() && env.symbols.count(init_name)) {
        TypeInfo src_type = env.symbols[init_name];
        if (src_type.is_array && src_type.array_dims.empty() && src_type.array_size > 0) {
            src_type.array_dims = {src_type.array_size};
        }
        if (src_type.is_array && src_type.array_dims == arr_type.array_dims) {
            std::vector<int> missing_prefix;
            std::string missing = firstUninitializedFlattenedElement(init_name, src_type, env, missing_prefix, 0);
            if (!missing.empty()) {
                env.error = "Array declaration initializer from '" + init_name +
                    "' reads uninitialized element '" + missing + "'";
                return out;
            }
            TypeInfo scalar = scalarTypeFromArray(arr_type);
            std::vector<int> copy_prefix;
            std::function<void(size_t)> copy_rec = [&](size_t depth) {
                if (depth >= arr_type.array_dims.size()) {
                    auto stmt = std::make_shared<Stmt>();
                    stmt->kind = StmtKind::Assign;
                    auto dst = joinIndexName(s->decl_name, copy_prefix);
                    auto src = joinIndexName(init_name, copy_prefix);
                    stmt->assign_target = make_var(dst, scalar);
                    stmt->assign_value = make_var(src, scalar);
                    out.push_back(stmt);
                    env.symbols[dst] = scalar;
                    env.initialized.insert(dst);
                    return;
                }
                for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
                    copy_prefix.push_back(i);
                    copy_rec(depth + 1);
                    copy_prefix.pop_back();
                }
            };
            copy_rec(0);
            if (env.error.empty()) env.initialized.insert(s->decl_name);
            return out;
        }
    }
    std::function<std::string(const ExprPtr&)> find_regproxy = [&](const ExprPtr& expr) -> std::string {
        if (!expr) return {};
        if (expr->kind == ExprKind::VarRef && env.regproxy_aliases.count(expr->var_name)) {
            return expr->var_name;
        }
        if (expr->kind == ExprKind::FieldAccess) return find_regproxy(expr->struct_base);
        if (expr->kind == ExprKind::Cast || expr->kind == ExprKind::ZExt ||
            expr->kind == ExprKind::SExt || expr->kind == ExprKind::Trunc) {
            return find_regproxy(expr->cast_expr);
        }
        if (expr->kind == ExprKind::Call) {
            for (const auto& arg : expr->args) {
                std::string found = find_regproxy(arg);
                if (!found.empty()) return found;
            }
        }
        return {};
    };
    std::string proxy_name = !init_name.empty() && env.regproxy_aliases.count(init_name)
        ? init_name
        : find_regproxy(init);
    if (!proxy_name.empty()) {
        auto alias_it = env.regproxy_aliases.find(proxy_name);
        if (alias_it != env.regproxy_aliases.end()) {
            std::string rdata = alias_it->second.rdata;
            if (env.symbols.count(rdata)) {
                packed = make_var(rdata, env.symbols[rdata]);
            }
        }
    }
    if (!packed) {
        packed = rewriteExpr(init, env);
        if (!env.error.empty()) return out;
    }
    if (packed) {
        TypeInfo packed_array_type = normalizeArrayType(packed->type);
        if (packed_array_type.is_array && !packed_array_type.array_dims.empty()) {
            int packed_array_width = flattenedTypeWidth(packed_array_type, env);
            if (packed_array_width > 0 && packed->type.width < packed_array_width) {
                packed->type.width = packed_array_width;
            }
        }
    }

    int elem_count = flatElementCount(arr_type);
    TypeInfo scalar = scalarTypeFromArray(arr_type);
    int target_array_width = flattenedTypeWidth(arr_type, env);
    if (packed && target_array_width > 0 && packed->type.width > 0 &&
        packed->type.width < target_array_width &&
        packed->kind == ExprKind::Trunc &&
        packed->cast_expr && packed->cast_expr->type.width >= target_array_width) {
        packed = packed->cast_expr;
    }
    if (packed && packed->type.width > 0 && elem_count > 1 &&
        scalar.width > 0 && scalar.width * elem_count > packed->type.width &&
        arr_type.width > 0 && arr_type.width % elem_count == 0) {
        arr_type.width = arr_type.width / elem_count;
    }

    int bit_offset = 0;
    prefix.clear();
    expandPackedArrayInitRec(s->decl_name, arr_type, packed, env, out, prefix, bit_offset, 0);
    if (env.error.empty()) env.initialized.insert(s->decl_name);
    return out;
}

ExprPtr rewriteArrayAccess(const ExprPtr& e, Env& env) {
    ExprPtr base;
    std::vector<ExprPtr> raw_indices;
    if (!collectArrayAccess(e, base, raw_indices) || !base) return cloneExpr(e);

    std::string name = baseName(base);
    if (name.empty()) {
        env.error = "Unsupported array base expression kind=" +
            std::to_string(base ? static_cast<int>(base->kind) : -1);
        if (base && base->kind == ExprKind::Call) {
            env.error += " base_callee='" + base->callee + "' base_arg_count=" +
                std::to_string(base->args.size());
        }
        return nullptr;
    }
    if (!env.initialized.count(name) && env.symbols.count(name) && !base->type.is_array &&
        !buildPackedFlattenedPrefix(name, env)) {
        env.error = "Read of uninitialized variable '" + name + "'";
        return nullptr;
    }

    std::vector<ExprPtr> indices;
    for (auto& idx : raw_indices) {
        indices.push_back(rewriteExpr(idx, env));
        if (!env.error.empty()) return nullptr;
    }

    auto regproxy_it = env.regproxy_aliases.find(name);
    if (regproxy_it != env.regproxy_aliases.end()) {
        const std::string& rdata = regproxy_it->second.rdata;
        TypeInfo rdata_type = symbolType(env, rdata);
        if (rdata_type.is_array && rdata_type.array_dims.empty() && rdata_type.array_size > 0) {
            rdata_type.array_dims = {rdata_type.array_size};
            env.symbols[rdata] = rdata_type;
        }
        if (!rdata_type.is_array || rdata_type.array_dims.empty()) {
            env.error = "RegProxy operator[] requires array rdata alias for '" + name + "'";
            return nullptr;
        }
        if (indices.size() != 1) {
            env.error = "RegProxy operator[] expects one dynamic register index for '" + name + "'";
            return nullptr;
        }
        std::vector<int> prefix;
        addFlattenedArraySymbols(env, rdata, rdata_type, true, prefix, 0);
        env.initialized.insert(rdata);
        env.input_arrays.insert(rdata);
        addSeedSymbolsWithPrefix(env, rdata);
        auto scalar = scalarTypeFromArray(rdata_type);
        auto access = make_array_access(make_var(rdata, rdata_type),
                                        cloneExpr(indices.front()),
                                        scalar);
        auto value = rewriteArrayAccess(access, env);
        if (!value || !env.error.empty()) return nullptr;
        TypeInfo target_type = e->type;
        if (target_type.is_array || target_type.width <= 0) {
            target_type = scalar;
        }
        return castIfWidthChanges(value, target_type);
    }

    TypeInfo arr_type = base->type;
    if (env.symbols.count(name)) arr_type = env.symbols[name];
    if (arr_type.is_array && arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    if (!arr_type.is_array || arr_type.array_dims.empty()) {
        auto rb = rewriteExpr(base, env);
        if (!env.error.empty()) return nullptr;
        auto out = std::make_shared<Expr>();
        out->kind = ExprKind::ArrayAccess;
        out->array_base = rb;
        out->index = indices.empty() ? nullptr : indices.front();
        out->type = e->type;
        return out;
    }

    if (indices.size() > arr_type.array_dims.size()) {
        env.error = "Too many indices for array '" + name + "'";
        return nullptr;
    }
    if (!validateLiteralBounds(name, indices, arr_type.array_dims, env)) {
        return nullptr;
    }

    bool dynamic_single_index = indices.size() == 1 && !literalIndex(indices.front()).has_value();
    if (!arr_type.init_values.empty() && indices.size() == 1 &&
        (arr_type.is_static || dynamic_single_index)) {
        env.lookup_tables[name] = arr_type.init_values;
        return makeLookupExpr(name, arr_type, indices.front());
    }

    bool all_literal = true;
    std::vector<int> literal_prefix;
    for (auto& idx : indices) {
        auto lit = literalIndex(idx);
        if (!lit.has_value()) {
            all_literal = false;
            break;
        }
        literal_prefix.push_back(*lit);
    }
    if (all_literal) {
        auto flat = joinIndexName(name, literal_prefix);
        if (!env.initialized.count(flat) &&
            (env.input_arrays.count(name) || env.initialized.count(name))) {
            std::vector<int> prefix;
            addFlattenedArraySymbols(env, name, arr_type, true, prefix, 0);
        }
        if (!env.initialized.count(flat)) {
            env.error = "Read of uninitialized array element '" + flat + "'";
            return nullptr;
        }
        return make_var(flat, scalarTypeFromArray(arr_type));
    }

    std::vector<int> init_prefix;
    if (env.input_arrays.count(name) || env.initialized.count(name)) {
        addFlattenedArraySymbols(env, name, arr_type, true, init_prefix, 0);
        init_prefix.clear();
    }
    std::string missing = firstUninitializedFlattenedElement(name, arr_type, env, init_prefix, 0);
    if (!missing.empty()) {
        env.error = "Dynamic array read from '" + name +
            "' requires all elements to be initialized; missing element '" + missing + "'";
        return nullptr;
    }

    std::vector<int> prefix;
    TypeInfo scalar = scalarTypeFromArray(arr_type);
    return buildArrayMux(name, arr_type.array_dims, indices, 0, prefix, scalar);
}

ExprPtr rewriteExpr(const ExprPtr& e, Env& env) {
    if (!e || !env.error.empty()) return nullptr;

    switch (e->kind) {
    case ExprKind::Literal:
        return cloneExpr(e);
    case ExprKind::VarRef: {
        ExprPtr var = cloneExpr(e);
        auto sym_it = env.symbols.find(e->var_name);
        if (sym_it != env.symbols.end()) var->type = sym_it->second;
        if (e->type.hw_kind == "signed_view") {
            var->type.is_signed = true;
            var->type.hw_kind = "signed_view";
        }
        if (env.regproxy_aliases.count(e->var_name)) {
            return readRegProxy(e->var_name, var->type, env);
        }
        if (allowUnsafeTextFallback) {
            if (auto hw_method = parseOpaqueHwMethodVar(e, env)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                return hw_method;
            }
            if (auto array_access = parseOpaqueArrayVar(e)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                return rewriteArrayAccess(array_access, env);
            }
        }
        if (!env.symbols.count(e->var_name)) {
            env.error = "Unknown variable '" + e->var_name + "'";
            return nullptr;
        }
        auto dir_it = env.param_directions.find(e->var_name);
        if (dir_it != env.param_directions.end() && dir_it->second == "Output" &&
            !env.initialized.count(e->var_name) &&
            !hasAnyBitInitialized(env, e->var_name)) {
            env.error = "Read of output port initial value '" + e->var_name +
                "' is not supported; output ports are write-only";
            return nullptr;
        }
        if (env.param_directions.count(e->var_name) &&
            env.ssa_seed_symbols.count(e->var_name)) {
            env.formal_reads->insert(e->var_name);
        }
        auto sym = env.symbols.find(e->var_name);
        if (sym != env.symbols.end()) {
            TypeInfo t = sym->second;
            if (t.is_array && t.array_dims.empty() && t.array_size > 0) t.array_dims = {t.array_size};
            if (t.is_array && !t.array_dims.empty()) {
                return buildPackedArrayValue(e->var_name, t, env);
            }
        }
        if (auto packed = buildPackedFlattenedPrefix(e->var_name, env)) {
            return packed;
        }
        if (!env.initialized.count(e->var_name) && env.symbols.count(e->var_name)) {
            env.error = "Read of uninitialized variable '" + e->var_name + "'";
            return nullptr;
        }
        return var;
    }
    case ExprKind::FieldAccess: {
        if (e->field_name.rfind("operator", 0) == 0) {
            return rewriteExpr(e->struct_base, env);
        }
        if (e->field_name == "setnext" || e->field_name == "call") {
            return rewriteExpr(e->struct_base, env);
        }
        if (e->field_name == "readdata" || e->field_name == "front" ||
            e->field_name == "enqready" || e->field_name == "deqvalid") {
            auto fake_call = std::make_shared<Expr>();
            fake_call->kind = ExprKind::Call;
            fake_call->callee = e->field_name;
            fake_call->type = e->type;
            fake_call->args.push_back(cloneExpr(e));
            if (auto lowered = lowerProxyMethodExpr(fake_call, env)) {
                return lowered;
            }
        }
        std::string alias_object;
        std::vector<std::string> alias_fields;
        if (fieldAccessPath(e, alias_object, alias_fields)) {
            if (auto alias = env.alias_graph.resolvePath(alias_object, alias_fields)) {
                TypeInfo alias_type = e->type;
                if (env.symbols.count(alias->canonical_name)) alias_type = env.symbols[alias->canonical_name];
                return rewriteExpr(make_var(alias->canonical_name, alias_type), env);
            }
        }
        // Struct values are represented by flattened field symbols. Reading a
        // field must validate that flattened symbol, not require a separate
        // initialized aggregate value that does not exist in the normalized IR.
        if (e->struct_base) {
            std::string base_object;
            std::vector<std::string> base_fields;
            const bool has_flattened_base_path = fieldAccessPath(e->struct_base, base_object, base_fields);
            const std::string base_name = has_flattened_base_path ? baseName(e->struct_base) : std::string{};
            if (base_name.empty()) {
                // Fall through to packed-expression handling below.
            } else {
            TypeInfo base_type = e->struct_base->type;
            auto base_sym = env.symbols.find(base_name);
            if (base_sym != env.symbols.end()) base_type = base_sym->second;
            if (env.regproxy_aliases.count(base_name)) {
                const auto* proxy_fields = findStructFields(base_type, env);
                if (proxy_fields) {
                    int total_width = 0;
                    int field_offset = 0;
                    const StructFieldInfo* selected = nullptr;
                    for (const auto& field : *proxy_fields) {
                        if (field.name == e->field_name) {
                            selected = &field;
                            field_offset = total_width;
                        }
                        total_width += field.type.width;
                    }
                    if (selected && selected->type.width > 0 && total_width > 0) {
                        auto packed = readRegProxy(base_name,
                                                   make_hw_type("UInt", total_width, false), env);
                        if (!packed || !env.error.empty()) return nullptr;
                        return make_slice(std::move(packed),
                                          field_offset + selected->type.width - 1,
                                          field_offset,
                                          selected->type);
                    }
                }
            }
            const std::string flat = base_name + "_" + e->field_name;
            TypeInfo field_type = e->type;
            auto known = env.symbols.find(flat);
            if (known != env.symbols.end()) field_type = known->second;
            if (known != env.symbols.end() ||
                findStructFieldByToken(base_type, env, e->field_name)) {
                env.symbols[flat] = field_type;
                if (!env.initialized.count(flat) && env.initialized.count(base_name)) {
                    markScalarFullyInitialized(env, flat, field_type);
                }
                if (!env.initialized.count(flat)) {
                    env.error = "Read of uninitialized field '" + flat + "'";
                    env.error += " base_name='" + base_name + "'";
                    env.error += " base_kind=" +
                        std::to_string(e->struct_base ? static_cast<int>(e->struct_base->kind) : -1);
                    return nullptr;
                }
                if (env.ssa_seed_symbols.count(base_name)) {
                    env.ssa_seed_symbols[flat] = field_type;
                }
                return make_var(flat, field_type);
            }
            }
        }
        if (e->struct_base) {
            const auto* value_fields = findStructFields(e->struct_base->type, env);
            if (value_fields) {
                int offset = 0;
                for (const auto& field : *value_fields) {
                    if (field.name == e->field_name) {
                        auto packed = rewriteExpr(e->struct_base, env);
                        if (!packed || !env.error.empty()) return nullptr;
                        int field_width = flattenedTypeWidth(field.type, env);
                        if (packed->type.width <= 0 &&
                            e->struct_base->kind == ExprKind::Call &&
                            e->struct_base->args.empty()) {
                            int total_width = flattenedTypeWidth(e->struct_base->type, env);
                            if (total_width <= 0) {
                                for (const auto& value_field : *value_fields) {
                                    int width = flattenedTypeWidth(value_field.type, env);
                                    if (width <= 0) {
                                        total_width = 0;
                                        break;
                                    }
                                    total_width += width;
                                }
                            }
                            if (total_width > 0) {
                                packed = make_literal(
                                    "0", make_hw_type("UInt", total_width, false));
                            }
                        }
                        if (field_width <= 0 || packed->type.width <= 0 ||
                            offset + field_width > packed->type.width) {
                            env.error = "Unsupported packed struct field read '" +
                                        e->field_name + "' with inconsistent width";
                            env.error += " base_kind=" +
                                std::to_string(e->struct_base ? static_cast<int>(e->struct_base->kind) : -1);
                            if (e->struct_base && e->struct_base->kind == ExprKind::Call) {
                                env.error += " base_callee='" + e->struct_base->callee + "'";
                                env.error += " base_args=" +
                                    std::to_string(e->struct_base->args.size());
                                if (!e->struct_base->args.empty() &&
                                    e->struct_base->args.front()) {
                                    env.error += " first_arg_kind=" +
                                        std::to_string(static_cast<int>(
                                            e->struct_base->args.front()->kind));
                                    if (e->struct_base->args.front()->kind ==
                                        ExprKind::Call) {
                                        env.error += " first_arg_callee='" +
                                            e->struct_base->args.front()->callee + "'";
                                    } else if (e->struct_base->args.front()->kind ==
                                               ExprKind::VarRef) {
                                        env.error += " first_arg_name='" +
                                            e->struct_base->args.front()->var_name + "'";
                                    }
                                }
                            }
                            env.error += " base_name='" + baseName(e->struct_base) + "'";
                            env.error += " packed_width=" + std::to_string(packed ? packed->type.width : 0);
                            env.error += " field_width=" + std::to_string(field_width);
                            env.error += " offset=" + std::to_string(offset);
                            return nullptr;
                        }
                        TypeInfo field_type = field.type;
                        field_type.width = field_width;
                        return make_slice(std::move(packed),
                                          offset + field_width - 1,
                                          offset,
                                          field_type);
                    }
                    offset += flattenedTypeWidth(field.type, env);
                }
            }
        }
        auto base = rewriteExpr(e->struct_base, env);
        if (!env.error.empty()) return nullptr;
        auto flat = baseName(base) + "_" + e->field_name;
        TypeInfo ty = e->type;
        if (env.symbols.count(flat)) ty = env.symbols[flat];
        env.symbols[flat] = ty;
        if (!env.initialized.count(flat) && env.initialized.count(baseName(base))) {
            env.initialized.insert(flat);
        }
        if (!ty.is_array && !env.initialized.count(flat)) {
            env.error = "Read of uninitialized field '" + flat + "'";
            env.error += " rewritten_base_name='" + baseName(base) + "'";
            env.error += " original_base_kind=" +
                std::to_string(e->struct_base ? static_cast<int>(e->struct_base->kind) : -1);
            env.error += " rewritten_base_kind=" +
                std::to_string(base ? static_cast<int>(base->kind) : -1);
            env.error += " field='" + e->field_name + "'";
            return nullptr;
        }
        if (env.ssa_seed_symbols.count(baseName(base))) {
            env.ssa_seed_symbols[flat] = ty;
        }
        return make_var(flat, ty);
    }
    case ExprKind::ArrayAccess: {
        return rewriteArrayAccess(e, env);
    }
    case ExprKind::BinaryOp: {
        auto l = rewriteExpr(e->left, env);
        auto r = rewriteExpr(e->right, env);
        if (!env.error.empty()) return nullptr;
        if (isBuiltinBinaryExpression(e, l, r)) {
            const bool shift = e->op == "<<" || e->op == ">>";
            const bool logical = e->op == "&&" || e->op == "||";
            if (!shift && !logical) {
                canonicalizeBuiltinBinaryOperands(l, r);
            }
            if (!shift && !logical && !builtinOperandsHaveCanonicalCommonType(l, r)) {
                env.error = "Builtin C++ operator '" + e->op +
                    "' operands were not canonicalized to a common type: lhs='" +
                    l->type.name + "/" + l->type.hw_kind + "' width=" +
                    std::to_string(l->type.width) + " signed=" +
                    (l->type.is_signed ? "true" : "false") + ", rhs='" +
                    r->type.name + "/" + r->type.hw_kind + "' width=" +
                    std::to_string(r->type.width) + " signed=" +
                    (r->type.is_signed ? "true" : "false") +
                    ", lhs_name='" + baseName(l) + "', rhs_name='" + baseName(r) + "'";
                return nullptr;
            }
            if (auto folded = foldConstantOvershift(e->op, l, r)) return folded;
            return make_binary(e->op, l, r, e->type);
        }
        if (e->op == "/" || e->op == "%") {
            TypeInfo builtin_result;
            if (unifyBuiltinDivRemOperands(l, r, builtin_result, env)) {
                if (!env.error.empty()) return nullptr;
                return make_binary(e->op, l, r, builtin_result);
            }
            return lowerPowerOfTwoDivRem(e->op, l, r, env);
        }
        if (e->op == "/=" || e->op == "%=") {
            env.error = IntSemantics::binaryResultType(e->op.substr(0, 1), l ? l->type : TypeInfo{},
                                                       r ? r->type : TypeInfo{}).error;
            return nullptr;
        }
        normalizeConstantOperandsForBinary(e->op, l, r);
        if ((e->op == "&" || e->op == "|" || e->op == "^") &&
            l && r && l->type.width > 0 && r->type.width > 0 &&
            l->type.width != r->type.width) {
                if (isWidthCastableConstantExpr(l)) {
                    l = castIfWidthChanges(l, r->type);
                } else if (isWidthCastableConstantExpr(r)) {
                    r = castIfWidthChanges(r, l->type);
                } else if (widenBuiltinBitwiseOperands(l, r)) {
                    // See the operator-call path above: this is limited to
                    // builtin C++ integer promotion, not VUL width coercion.
                } else {
                    env.error = IntSemantics::binaryResultType(e->op, l->type, r->type).error +
                        " for operator '" + e->op + "' lhs_width=" + std::to_string(l->type.width) +
                    " lhs_type='" + l->type.name + "/" + l->type.hw_kind +
                    "' rhs_width=" + std::to_string(r->type.width) +
                    " rhs_type='" + r->type.name + "/" + r->type.hw_kind + "'";
                return nullptr;
            }
        }
        if (auto folded = foldConstantOvershift(e->op, l, r)) return folded;
        auto out = make_binary(e->op, l, r, resultTypeForBinary(e->op, l ? l->type : TypeInfo{}, r ? r->type : TypeInfo{}));
        return out;
    }
    case ExprKind::UnaryOp: {
        if (e->op == "++" || e->op == "--") {
            env.error = "Unsupported increment/decrement in value expression; "
                        "use it as a standalone statement";
            return nullptr;
        }
        if (e->op == "*" || e->op == "&") {
            env.error = "Unsupported pointer/reference operation '" + e->op + "'";
            return nullptr;
        }
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        TypeInfo ty = e->type;
        if (e->op == "!" || e->op == "&&" || e->op == "||") ty = make_hw_type("bool", 1, false);
        else if ((e->op == "~" || e->op == "-") && op &&
                 !isBuiltinExpressionType(e->type)) {
            ty = op->type;
        }
        return make_unary(e->op, op, ty);
    }
    case ExprKind::Ternary: {
        auto c = rewriteExpr(e->cond, env);
        auto t = rewriteExpr(e->then_expr, env);
        auto f = rewriteExpr(e->else_expr, env);
        if (!env.error.empty()) return nullptr;
        if (t && f && isBuiltinExpressionType(e->type) &&
            isBuiltinExpressionType(t->type) &&
            isBuiltinExpressionType(f->type)) {
            canonicalizeBuiltinBinaryOperands(t, f);
            if (!builtinOperandsHaveCanonicalCommonType(t, f)) {
                env.error = "Builtin C++ conditional branches were not "
                            "canonicalized to a common type";
                return nullptr;
            }
            return make_ite(c, t, f, e->type);
        }
        normalizeConstantBranchesForTernary(t, f);
        TypeInfo out_type = e->type;
        if (t && f && t->type.width > 0 && t->type.width == f->type.width) {
            if (!t->type.hw_kind.empty()) {
                out_type = t->type;
            } else if (!f->type.hw_kind.empty()) {
                out_type = f->type;
            } else if (out_type.width != t->type.width) {
                out_type = t->type;
            }
        }
        return make_ite(c, t, f, out_type);
    }
    case ExprKind::Cast: {
        auto c = rewriteExpr(e->cast_expr, env);
        if (!env.error.empty()) return nullptr;
        return castIfWidthChanges(c, e->cast_type);
    }
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc: {
        auto c = rewriteExpr(e->cast_expr, env);
        if (!env.error.empty()) return nullptr;
        TypeInfo cast_type = e->type;
        if (cast_type.is_array) cast_type = scalarTypeFromArray(cast_type);
        int to_width = e->to_width > 0 ? e->to_width : cast_type.width;
        if (e->kind == ExprKind::SExt) {
            auto out = make_sext(c, to_width);
            out->type = cast_type.width > 0 ? cast_type : out->type;
            return out;
        }
        if (e->kind == ExprKind::Trunc) {
            auto out = make_trunc(c, to_width, cast_type.is_signed);
            out->type = cast_type.width > 0 ? cast_type : out->type;
            return out;
        }
        auto out = make_zext(c, to_width);
        out->type = cast_type.width > 0 ? cast_type : out->type;
        return out;
    }
    case ExprKind::Slice:
    case ExprKind::BitSelect: {
        auto b = rewriteExpr(e->base, env);
        if (!env.error.empty()) return nullptr;
        int width = b ? b->type.width : 0;
        if (e->kind == ExprKind::BitSelect) {
            if (e->bit < 0 || (width > 0 && e->bit >= width)) {
                env.error = "Bit select out of bounds";
                return nullptr;
            }
            return make_bit_select(b, e->bit);
        }
        if (e->hi < 0 || e->lo < 0 || e->hi < e->lo || (width > 0 && e->hi >= width)) {
            env.error = "Slice out of bounds";
            return nullptr;
        }
        TypeInfo slice_type = e->type;
        if (slice_type.is_array) {
            slice_type = scalarTypeFromArray(slice_type);
        }
        return make_slice(b, e->hi, e->lo, slice_type);
    }
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit: {
        ExprPtr b;
        if (e->base && e->base->kind == ExprKind::VarRef &&
            env.symbols.count(e->base->var_name) &&
            !env.initialized.count(e->base->var_name) &&
            !hasAnyBitInitialized(env, e->base->var_name)) {
            if (std::find(env.output_params.begin(), env.output_params.end(),
                          e->base->var_name) != env.output_params.end()) {
                b = zeroValueForType(env.symbols[e->base->var_name]);
            } else {
                b = cloneExpr(e->base);
                b->type = env.symbols[e->base->var_name];
            }
        } else {
            b = rewriteExpr(e->base, env);
        }
        auto v = rewriteExpr(e->value, env);
        if (!env.error.empty()) return nullptr;
        int base_width = b ? b->type.width : 0;
        if (e->kind == ExprKind::DynamicWriteBit) {
            auto idx = rewriteExpr(e->index, env);
            if (!env.error.empty()) return nullptr;
            return make_dynamic_write_bit(b, idx, castIfWidthChanges(v, make_hw_type("bool", 1, false)), e->type);
        }
        if (e->kind == ExprKind::DynamicWriteSlice) {
            auto idx = rewriteExpr(e->index, env);
            if (!env.error.empty()) return nullptr;
            int width = v ? v->type.width : 0;
            if (width <= 0 || (base_width > 0 && width > base_width)) {
                env.error = "Dynamic slice assignment width out of bounds";
                return nullptr;
            }
            TypeInfo slice_ty = make_hw_type(v && v->type.is_signed ? "Int" : "UInt",
                                             width,
                                             v ? v->type.is_signed : false);
            return make_dynamic_write_slice(b, idx, castIfWidthChanges(v, slice_ty), e->type);
        }
        if (e->kind == ExprKind::WriteBit) {
            if (e->bit < 0 || (base_width > 0 && e->bit >= base_width)) {
                env.error = "Bit assignment out of bounds";
                return nullptr;
            }
            return make_write_bit(b, e->bit, castIfWidthChanges(v, make_hw_type("bool", 1, false)), e->type);
        }
        if (e->hi < e->lo || e->hi < 0 || e->lo < 0 || (base_width > 0 && e->hi >= base_width)) {
            env.error = "Slice assignment out of bounds";
            return nullptr;
        }
        TypeInfo slice_ty = make_hw_type(e->type.is_signed ? "Int" : "UInt", e->hi - e->lo + 1, e->type.is_signed);
        return make_write_slice(b, e->hi, e->lo, castIfWidthChanges(v, slice_ty), e->type);
    }
    case ExprKind::Concat: {
        std::vector<ExprPtr> parts;
        for (auto& p : e->parts) {
            parts.push_back(rewriteExpr(p, env));
            if (!env.error.empty()) return nullptr;
        }
        return make_concat(std::move(parts));
    }
    case ExprKind::Repeat: {
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        if (e->times <= 0) {
            env.error = "repeat count must be a positive compile-time constant";
            return nullptr;
        }
        return make_repeat(op, e->times);
    }
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor: {
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        return make_reduce(e->kind, op);
    }
    case ExprKind::Call:
        return inlineHelperCall(e, env);
    }
    return cloneExpr(e);
}

ExprPtr indexMatchExpr(const std::vector<ExprPtr>& indices,
                       const std::vector<int>& prefix) {
    ExprPtr cond;
    for (size_t i = 0; i < indices.size() && i < prefix.size(); ++i) {
        auto eq = make_binary("==",
            cloneExpr(indices[i]),
            make_literal(std::to_string(prefix[i]), indices[i] ? indices[i]->type : TypeInfo{"int", 32, true}),
            TypeInfo{"bool", 1, false});
        cond = cond ? make_binary("&&", cond, eq, TypeInfo{"bool", 1, false}) : eq;
    }
    return cond ? cond : make_literal("true", TypeInfo{"bool", 1, false});
}

void expandArrayWriteRec(const std::string& name,
                         const TypeInfo& arr_type,
                         const std::vector<ExprPtr>& indices,
                         ExprPtr value,
                         Env& env,
                         std::vector<StmtPtr>& out,
                         std::vector<int>& prefix,
                         size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        auto flat = joinIndexName(name, prefix);
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        ExprPtr rhs = cloneExpr(value);
        bool all_literal = true;
        bool matches = true;
        for (size_t i = 0; i < indices.size() && i < prefix.size(); ++i) {
            auto lit = literalIndex(indices[i]);
            if (!lit.has_value()) {
                all_literal = false;
                break;
            }
            if (*lit != prefix[i]) matches = false;
        }
        if (all_literal && !matches) return;
        if (!all_literal) {
            ExprPtr old_value;
            if (env.initialized.count(flat)) {
                old_value = make_var(flat, scalar);
            } else if (hasSemanticDefaultFor(env, flat) || hasSemanticDefaultFor(env, name)) {
                old_value = zeroValueForType(scalar);
            } else {
                old_value = make_var(flat, scalar);
            }
            rhs = make_ite(indexMatchExpr(indices, prefix), rhs, old_value, scalar);
        }

        auto s = std::make_shared<Stmt>();
        s->kind = StmtKind::Assign;
        s->assign_target = make_var(flat, scalar);
        rhs = castIfWidthChanges(rhs, scalar);
        if (isRegProxyWdataAliasTarget(flat, env) && scalar.width > 0) {
            s->assign_value = make_write_slice(make_var(flat, scalar),
                                               scalar.width - 1,
                                               0,
                                               rhs,
                                               scalar);
        } else {
            s->assign_value = rhs;
        }
        out.push_back(s);
        env.symbols[flat] = scalar;
        env.initialized.insert(flat);
        return;
    }

    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandArrayWriteRec(name, arr_type, indices, value, env, out, prefix, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewriteArrayAssign(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    ExprPtr base;
    std::vector<ExprPtr> raw_indices;
    if (!collectArrayAccess(s->assign_target, base, raw_indices) || !base) {
        return out;
    }
    std::string name = baseName(base);
    if (name.empty()) {
        env.error = "Unsupported array assignment base expression kind=" +
            std::to_string(s->assign_target ? static_cast<int>(s->assign_target->kind) : -1) +
            " base_kind=" +
            std::to_string(base ? static_cast<int>(base->kind) : -1);
        if (base && base->kind == ExprKind::Call) {
            env.error += " base_callee='" + base->callee + "' base_arg_count=" +
                std::to_string(base->args.size());
        }
        return out;
    }

    TypeInfo arr_type = base->type;
    if (env.symbols.count(name)) arr_type = env.symbols[name];
    if (arr_type.is_array && arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    if (!arr_type.is_array || arr_type.array_dims.empty()) {
        env.error = "Assignment target '" + name + "' is not a static array";
        return out;
    }

    std::vector<ExprPtr> indices;
    bool all_literal = true;
    for (auto& idx : raw_indices) {
        indices.push_back(rewriteExpr(idx, env));
        if (!env.error.empty()) return out;
        if (!literalIndex(indices.back()).has_value()) all_literal = false;
    }
    if (!validateLiteralBounds(name, indices, arr_type.array_dims, env)) {
        return out;
    }
    auto value = rewriteExpr(s->assign_value, env);
    if (!env.error.empty()) return out;

    env.symbols[name] = arr_type;
    if (!all_literal) {
        std::vector<int> prefix;
        if (!allFlattenedElementsInitialized(name, arr_type, env, prefix, 0) &&
            !hasSemanticDefaultFor(env, name)) {
            env.error = "Dynamic array assignment to '" + name +
                "' requires all elements to be initialized";
            return out;
        }
    }
    std::vector<int> prefix;
    expandArrayWriteRec(name, arr_type, indices, value, env, out, prefix, 0);
    return out;
}

bool bodyEndsWithSwitchTerminator(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto last = body.back();
    if (!last) return false;
    if (last->kind == StmtKind::Break || last->kind == StmtKind::Return) return true;
    if (last->kind == StmtKind::Block) return bodyEndsWithSwitchTerminator(last->block_stmts);
    return false;
}

std::vector<StmtPtr> rewriteWholeArrayAssign(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || !s->assign_target || s->assign_target->kind != ExprKind::VarRef ||
        !s->assign_value || s->assign_value->kind != ExprKind::VarRef) {
        return out;
    }
    std::string dst = s->assign_target->var_name;
    std::string src = s->assign_value->var_name;
    TypeInfo dst_type = s->assign_target->type;
    if (env.symbols.count(dst)) dst_type = env.symbols[dst];
    TypeInfo src_type = s->assign_value->type;
    if (env.symbols.count(src)) src_type = env.symbols[src];
    if (dst_type.is_array && dst_type.array_dims.empty() && dst_type.array_size > 0) dst_type.array_dims = {dst_type.array_size};
    if (src_type.is_array && src_type.array_dims.empty() && src_type.array_size > 0) src_type.array_dims = {src_type.array_size};
    if (!dst_type.is_array || !src_type.is_array || dst_type.array_dims != src_type.array_dims) return out;

    std::vector<int> prefix;
    std::string missing = firstUninitializedFlattenedElement(src, src_type, env, prefix, 0);
    if (!missing.empty()) {
        env.error = "Whole-array assignment from '" + src +
            "' reads uninitialized element '" + missing + "'";
        return out;
    }
    prefix.clear();
    std::function<void(size_t)> rec = [&](size_t depth) {
        if (depth >= dst_type.array_dims.size()) {
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::Assign;
            auto scalar = scalarTypeFromArray(dst_type);
            auto d = joinIndexName(dst, prefix);
            auto v = joinIndexName(src, prefix);
            stmt->assign_target = make_var(d, scalar);
            stmt->assign_value = make_var(v, scalar);
            out.push_back(stmt);
            env.symbols[d] = scalar;
            env.initialized.insert(d);
            return;
        }
        for (int i = 0; i < dst_type.array_dims[depth]; ++i) {
            prefix.push_back(i);
            rec(depth + 1);
            prefix.pop_back();
        }
    };
    rec(0);
    return out;
}

StmtPtr rewriteBitSliceAssign(const StmtPtr& s, Env& env) {
    if (!s || !s->assign_target) return nullptr;
    auto target_call = s->assign_target;
    if (target_call->kind == ExprKind::VarRef) {
        std::string text = target_call->var_name;
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c);
        }), text.end());
        if (!text.empty() && text.front() == '(') {
            auto close_base = text.find(")(");
            if (close_base != std::string::npos) {
                text = text.substr(1, close_base - 1) + text.substr(close_base + 1);
            }
        }
        auto lp = text.find('(');
        auto rp = text.rfind(')');
        if (lp != std::string::npos && rp == text.size() - 1 && lp > 0) {
            std::string base_name = text.substr(0, lp);
            while (base_name.size() >= 2 && base_name.front() == '(' && base_name.back() == ')') {
                base_name = base_name.substr(1, base_name.size() - 2);
            }
            auto sym = env.symbols.find(base_name);
            if (sym != env.symbols.end() && sym->second.is_hw_int) {
                std::string inside = text.substr(lp + 1, rp - lp - 1);
                auto comma = inside.find(',');
                auto parse_int = [](const std::string& s, int& out) {
                    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
                        return false;
                    }
                    out = std::stoi(s);
                    return true;
                };
                if (comma == std::string::npos) {
                    int bit = -1;
                    if (parse_int(inside, bit)) target_call = make_bit_select(make_var(base_name, sym->second), bit);
                } else {
                    int hi = -1;
                    int lo = -1;
                    if (parse_int(inside.substr(0, comma), hi) && parse_int(inside.substr(comma + 1), lo)) {
                        target_call = make_slice(make_var(base_name, sym->second),
                                                 hi,
                                                 lo,
                                                 make_hw_type(sym->second.is_signed ? "Int" : "UInt",
                                                              hi - lo + 1,
                                                              sym->second.is_signed));
                    }
                }
            }
        }
        if (allowUnsafeTextFallback && target_call->kind == ExprKind::VarRef) {
            if (auto parsed = parseOpaqueHwMethodVar(target_call, env)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                if (parsed->kind == ExprKind::BitSelect || parsed->kind == ExprKind::Slice) {
                    target_call = parsed;
                }
            }
            if (!env.error.empty()) return nullptr;
        }
    }
    bool is_bit = false;
    bool is_slice = false;
    bool is_dynamic = false;
    ExprPtr base_expr;
    ExprPtr dynamic_index;
    int hi = -1;
    int lo = -1;
    if (target_call->kind == ExprKind::Call &&
        (target_call->callee == "__bit" || target_call->callee == "__slice")) {
        is_bit = target_call->callee == "__bit";
        is_slice = target_call->callee == "__slice";
        if (target_call->args.empty()) return nullptr;
        base_expr = target_call->args.front();
        if (is_bit && target_call->args.size() >= 2) hi = lo = literalIntValue(target_call->args[1], -1);
        if (is_slice && target_call->args.size() >= 3) {
            hi = literalIntValue(target_call->args[1], -1);
            lo = literalIntValue(target_call->args[2], -1);
        }
    } else if (target_call->kind == ExprKind::Call &&
               (target_call->intrinsic == IntrinsicKind::DynamicBitAt ||
                target_call->intrinsic == IntrinsicKind::DynamicRangeAt ||
                target_call->callee == "__dynamic_bit_at" ||
                target_call->callee == "__dynamic_range_at")) {
        is_bit = target_call->intrinsic == IntrinsicKind::DynamicBitAt ||
                 target_call->callee == "__dynamic_bit_at";
        is_slice = !is_bit;
        is_dynamic = true;
        if (target_call->args.size() < 2) {
            env.error = is_bit ? "Unsupported dynamic bit assignment without index"
                               : "Unsupported dynamic slice assignment without index";
            return nullptr;
        }
        base_expr = target_call->args[0];
        dynamic_index = rewriteExpr(target_call->args[1], env);
        if (!env.error.empty()) return nullptr;
        if (is_bit) {
            hi = lo = 0;
        } else {
            int width = target_call->to_width > 0 ? target_call->to_width : target_call->type.width;
            if (width > 0) {
                hi = width - 1;
                lo = 0;
            }
        }
    } else if (target_call->kind == ExprKind::BitSelect || target_call->kind == ExprKind::Slice) {
        is_bit = target_call->kind == ExprKind::BitSelect;
        is_slice = target_call->kind == ExprKind::Slice;
        base_expr = target_call->base;
        hi = is_bit ? target_call->bit : target_call->hi;
        lo = is_bit ? target_call->bit : target_call->lo;
    } else {
        return nullptr;
    }
    if (!base_expr || base_expr->kind != ExprKind::VarRef) {
        env.error = "Unsupported bit/slice assignment base expression";
        return nullptr;
    }
    std::string base = base_expr->var_name;
    TypeInfo base_type = env.symbols.count(base) ? env.symbols[base] : base_expr->type;
    auto value = rewriteExpr(s->assign_value, env);
    if (!env.error.empty()) return nullptr;
    if (is_dynamic && is_slice && (hi < 0 || lo < 0)) {
        int width = value ? value->type.width : 0;
        if (width <= 0) {
            env.error = "Unsupported dynamic slice assignment with unknown width";
            return nullptr;
        }
        hi = width - 1;
        lo = 0;
    }
    if (env.param_directions.count(base)) {
        markFormalWrite(env, base);
    }
    bool base_has_value = is_dynamic ? env.initialized.count(base)
                                     : (env.initialized.count(base) || hasAnyBitInitialized(env, base));
    ExprPtr base_value_override;
    if (!base_has_value &&
        std::find(env.output_params.begin(), env.output_params.end(), base) != env.output_params.end()) {
        // Do not use the incoming version of a pure output as the base for
        // first write_slice/write_bit. The hardware meaning is an update from
        // an explicit zero/false value unless a previous assignment in this
        // function has already produced a value.
        base_value_override = zeroValueForType(base_type);
        base_has_value = true;
    }
    if (!base_has_value && (is_dynamic || hi < 0 || lo < 0)) {
        env.error = "Dynamic bit/slice assignment to uninitialized '" + base +
            "' requires a previous value";
        return nullptr;
    }
    if (hi < lo || hi < 0 || lo < 0 || (base_type.width > 0 && hi >= base_type.width)) {
        env.error = is_bit ? "Bit assignment out of bounds" : "Slice assignment out of bounds";
        return nullptr;
    }
    ExprPtr write_expr;
    auto base_var = base_value_override ? base_value_override : make_var(base, base_type);
    if (is_dynamic && is_bit) {
        write_expr = make_dynamic_write_bit(
            base_var,
            dynamic_index,
            castIfWidthChanges(value, make_hw_type("bool", 1, false)),
            base_type);
    } else if (is_dynamic) {
        TypeInfo slice_ty = make_hw_type(value && value->type.is_signed ? "Int" : "UInt",
                                         hi - lo + 1,
                                         value ? value->type.is_signed : false);
        write_expr = make_dynamic_write_slice(
            base_var,
            dynamic_index,
            castIfWidthChanges(value, slice_ty),
            base_type);
    } else if (is_bit) {
        write_expr = make_write_bit(base_var, hi, castIfWidthChanges(value, make_hw_type("bool", 1, false)), base_type);
    } else {
        TypeInfo slice_ty = make_hw_type(value && value->type.is_signed ? "Int" : "UInt", hi - lo + 1, value ? value->type.is_signed : false);
        write_expr = make_write_slice(base_var, hi, lo, castIfWidthChanges(value, slice_ty), base_type);
    }

    auto out = std::make_shared<Stmt>();
    out->kind = StmtKind::Assign;
    out->assign_target = make_var(base, base_type);
    out->assign_value = write_expr;
    env.symbols[base] = base_type;
    if (is_dynamic) {
        markScalarFullyInitialized(env, base, base_type);
    } else if (hi >= 0 && lo >= 0) {
        markBitRangeInitialized(env, base, base_type, hi, lo);
        if (!env.error.empty()) return nullptr;
    } else {
        markScalarFullyInitialized(env, base, base_type);
    }
    return out;
}

ExprPtr rewriteTarget(const ExprPtr& e, Env& env) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::VarRef) {
        auto out = cloneExpr(e);
        auto it = env.symbols.find(out->var_name);
        if (it != env.symbols.end()) out->type = it->second;
        return out;
    }
    if (e->kind == ExprKind::FieldAccess) {
        std::string alias_object;
        std::vector<std::string> alias_fields;
        if (fieldAccessPath(e, alias_object, alias_fields)) {
            if (auto alias = env.alias_graph.resolvePath(alias_object, alias_fields)) {
                if (!alias->writable) {
                    env.error = "Unsupported write through const alias '" + alias_object + "." + e->field_name + "'";
                    return nullptr;
                }
                TypeInfo alias_type = e->type;
                if (env.symbols.count(alias->canonical_name)) alias_type = env.symbols[alias->canonical_name];
                markFormalWrite(env, alias->canonical_name);
                return make_var(alias->canonical_name, alias_type);
            }
        }
        ExprPtr array_base;
        std::vector<ExprPtr> array_indices;
        if (collectArrayAccess(e->struct_base, array_base, array_indices) && array_base) {
            std::string array_name = baseName(array_base);
            TypeInfo array_type = array_base->type;
            if (!array_name.empty() && env.symbols.count(array_name)) {
                array_type = env.symbols[array_name];
            }
            array_type = normalizeArrayType(array_type);
            if (array_type.is_array && array_indices.size() == array_type.array_dims.size()) {
                std::vector<int> literal_prefix;
                bool all_literal = true;
                for (const auto& idx : array_indices) {
                    auto lit = literalIndex(idx);
                    if (!lit.has_value()) {
                        all_literal = false;
                        break;
                    }
                    literal_prefix.push_back(*lit);
                }
                TypeInfo elem_type = scalarTypeFromArray(array_type);
                if (all_literal) {
                    if (const auto* fields = findStructFields(elem_type, env)) {
                        for (const auto& field : *fields) {
                            if (field.name == e->field_name) {
                                return make_var(joinIndexName(array_name, literal_prefix) +
                                                    "_" + field.name,
                                                field.type);
                            }
                        }
                    }
                }
            }
        }
        auto b = rewriteTarget(e->struct_base, env);
        return make_var(baseName(b) + "_" + e->field_name, e->type);
    }
    if (e->kind == ExprKind::UnaryOp && e->op == "*" && e->operand &&
        e->operand->kind == ExprKind::VarRef) {
        return make_var(e->operand->var_name, e->type);
    }
    if (e->kind == ExprKind::ArrayAccess) {
        env.error = "Array element assignment is not supported after flattening";
        return nullptr;
    }
    env.error = "Unsupported assignment target";
    return nullptr;
}

StmtPtr rewriteStmt(const StmtPtr& s, Env& env) {
    if (!s || !env.error.empty()) return nullptr;
    auto r = std::make_shared<Stmt>(*s);
    switch (s->kind) {
    case StmtKind::Decl: {
        env.symbols[s->decl_name] = s->decl_type;
        if (isRegProxyType(s->decl_type) || isReqHelperType(s->decl_type)) {
            if (isRegProxyType(s->decl_type)) {
                if (s->decl_init_args.size() < 3) {
                    env.error = "Unsupported RegProxy declaration without constructor aliases for '" + s->decl_name + "'";
                    return nullptr;
                }
                Env::RegProxyAlias alias{
                    directVarName(s->decl_init_args[0]),
                    directVarName(s->decl_init_args[1]),
                    directVarName(s->decl_init_args[2])
                };
                if (alias.rdata.empty() || alias.wen.empty() || alias.wdata.empty()) {
                    env.error = "Unsupported RegProxy declaration with non-variable constructor alias for '" + s->decl_name + "'";
                    return nullptr;
                }
                env.regproxy_aliases[s->decl_name] = alias;
                env.alias_graph.bindField(s->decl_name, "rdata", AliasTarget{alias.rdata, AliasKind::RegProxyPort, false});
                env.alias_graph.bindField(s->decl_name, "wen", AliasTarget{alias.wen, AliasKind::RegProxyPort, true});
                env.alias_graph.bindField(s->decl_name, "wdata", AliasTarget{alias.wdata, AliasKind::RegProxyPort, true});
                markFormalWrite(env, alias.wen);
                markFormalWrite(env, alias.wdata);
                env.output_default_reasons[alias.wen] = "write_enable_default_false";
                env.output_default_reasons[alias.wdata] = "wdata_default_zero_when_wen_false";
                env.output_paired_controls[alias.wdata] = alias.wen;
                auto register_array_alias = [&](const std::string& port, bool initialized) {
                    auto sym = env.symbols.find(port);
                    if (sym == env.symbols.end()) return;
                    TypeInfo port_type = sym->second;
                    if (port_type.is_array && port_type.array_dims.empty() &&
                        port_type.array_size > 0) {
                        port_type.array_dims = {port_type.array_size};
                        env.symbols[port] = port_type;
                    }
                    if (port_type.is_array && !port_type.array_dims.empty()) {
                        std::vector<int> prefix;
                        addFlattenedArraySymbols(env, port, port_type, initialized, prefix, 0);
                        if (initialized) {
                            env.initialized.insert(port);
                            env.input_arrays.insert(port);
                            addSeedSymbolsWithPrefix(env, port);
                        }
                    }
                };
                register_array_alias(alias.rdata, true);
                register_array_alias(alias.wen, false);
                register_array_alias(alias.wdata, false);
            } else {
                if (s->decl_init_args.empty()) {
                    env.error = "Unsupported ReqHelper declaration without output aliases for '" + s->decl_name + "'";
                    return nullptr;
                }
                const auto* helper_fields = structFieldsForType(s->decl_type, env);
                std::string vld_field;
                std::string rdy_field;
                std::vector<std::string> ret_fields;
                std::vector<std::string> payload_fields;
                if (helper_fields) {
                    for (const auto& field : *helper_fields) {
                        if (field.name == "vld_ports" || field.name == "vld" ||
                            field.name.rfind("vld_ports_", 0) == 0 || field.name.rfind("vld_", 0) == 0) {
                            vld_field = field.name;
                        }
                        else if (field.name == "rdy_ports" || field.name == "rdy" ||
                                 field.name.rfind("rdy_ports_", 0) == 0 || field.name.rfind("rdy_", 0) == 0) {
                            rdy_field = field.name;
                        }
                        else if (field.name.rfind("arg_", 0) == 0 || field.name == "payload") {
                            payload_fields.push_back(field.name);
                        }
                        else if (field.name.rfind("ret_", 0) == 0 || field.name == "ret" ||
                                 field.name.rfind("value_", 0) == 0 || field.name == "value") {
                            ret_fields.push_back(field.name);
                        }
                    }
                }
                std::string vld = initializerArgForField(s, vld_field, env);
                std::string rdy = initializerArgForField(s, rdy_field, env);
                std::vector<std::pair<std::string, std::string>> ret_payloads;
                for (const auto& field : ret_fields) {
                    auto actual = initializerArgForField(s, field, env);
                    if (actual.empty()) {
                        env.error = "Unsupported ReqHelper return alias for '" +
                                    s->decl_name + "." + field + "'";
                        return nullptr;
                    }
                    ret_payloads.emplace_back(field, std::move(actual));
                }
                std::string ret_data = ret_payloads.empty() ? std::string{} : ret_payloads.front().second;
                std::vector<std::pair<std::string, std::string>> payloads;
                for (const auto& field : payload_fields) {
                    auto actual = initializerArgForField(s, field, env);
                    if (actual.empty()) {
                        env.error = "Unsupported ReqHelper payload alias for '" +
                                    s->decl_name + "." + field + "'";
                        return nullptr;
                    }
                    payloads.emplace_back(field, std::move(actual));
                }
                std::string arg_data = payloads.empty() ? std::string{} : payloads.front().second;
                Env::ReqHelperAlias alias{
                    vld,
                    rdy,
                    arg_data,
                    ret_data,
                    payloads,
                    ret_payloads
                };
                const bool ret_only_query = alias.vld.empty() && alias.arg_data.empty() && !alias.ret_payloads.empty();
                if (!ret_only_query &&
                    (alias.vld.empty() || (alias.arg_data.empty() && alias.ret_payloads.empty()))) {
                    env.error = "Unsupported ReqHelper declaration without structured vld/payload aliases for '" + s->decl_name + "'";
                    return nullptr;
                }
                if (!alias.vld.empty()) {
                    markFormalWrite(env, alias.vld);
                    env.output_default_reasons[alias.vld] = "valid_default_false";
                }
                for (const auto& payload : alias.payloads) {
                    markFormalWrite(env, payload.second);
                    env.output_default_reasons[payload.second] = "payload_default_zero_when_valid_false";
                    env.output_paired_controls[payload.second] = alias.vld;
                }
                env.reqhelper_aliases[s->decl_name] = alias;
                if (!alias.vld.empty()) {
                    env.alias_graph.bindField(s->decl_name, "vld_ports", AliasTarget{alias.vld, AliasKind::ReqHelperPort, true});
                }
                for (const auto& payload : alias.payloads) {
                    env.alias_graph.bindField(s->decl_name, payload.first,
                                              AliasTarget{payload.second, AliasKind::ReqHelperPort, true});
                }
                for (const auto& ret_payload : alias.ret_payloads) {
                    env.alias_graph.bindField(s->decl_name, ret_payload.first,
                                              AliasTarget{ret_payload.second, AliasKind::ReqHelperPort, false});
                }
                if (!alias.ret_data.empty()) {
                    env.alias_graph.bindField(s->decl_name, "ret_data", AliasTarget{alias.ret_data, AliasKind::ReqHelperPort, false});
                }
                if (!alias.rdy.empty()) {
                    env.alias_graph.bindField(s->decl_name, "rdy_ports", AliasTarget{alias.rdy, AliasKind::ReqHelperPort, false});
                    env.alias_graph.bindField(s->decl_name, "rdy", AliasTarget{alias.rdy, AliasKind::ReqHelperPort, false});
                }
                markScalarFullyInitialized(env, s->decl_name, s->decl_type);
                if (ret_only_query) {
                    r->decl_init = std::nullopt;
                    return r;
                }
                TypeInfo valid_type = symbolType(env, alias.vld, TypeInfo{"bool", 1, false});
                env.initialized.insert(alias.vld);
                addSeedSymbol(env, alias.vld, valid_type);
                auto init_valid = std::make_shared<Stmt>();
                init_valid->kind = StmtKind::Assign;
                init_valid->assign_target = make_var(alias.vld, valid_type);
                init_valid->assign_value = make_literal("false", valid_type);
                return init_valid;
            }
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
            r->decl_init = std::nullopt;
            return r;
        }
        if (s->decl_type.is_array) {
            TypeInfo arr_type = s->decl_type;
            if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
            env.symbols[s->decl_name] = arr_type;
            if (!arr_type.array_dims.empty()) {
                std::vector<int> prefix;
                addFlattenedArraySymbols(env, s->decl_name, arr_type,
                    !arr_type.init_values.empty(), prefix, 0);
            }
            if (arr_type.is_static && !arr_type.init_values.empty()) {
                env.lookup_tables[s->decl_name] = arr_type.init_values;
                env.initialized.insert(s->decl_name);
                return nullptr;
            }
        }
        if (s->decl_type.is_reference && s->decl_init.has_value()) {
            if (!s->decl_type.is_const) {
                env.error = "Unsupported mutable local reference alias '" + s->decl_name + "'";
                return nullptr;
            }
            std::string object;
            std::vector<std::string> fields;
            AliasTarget target;
            target.kind = AliasKind::ConstRef;
            target.mutability = AliasMutability::Immutable;
            target.writable = false;
            target.reason = "local_const_reference_alias";
            if (fieldAccessPath(s->decl_init.value(), object, fields)) {
                target.base_object = object;
                target.field_path = fields;
                target.canonical_name = object;
                for (const auto& field : fields) {
                    if (!target.canonical_name.empty()) target.canonical_name += "_";
                    target.canonical_name += field;
                }
                target.name = target.canonical_name;
            } else {
                std::string actual = directVarName(s->decl_init.value());
                if (actual.empty()) {
                    env.error = "Unsupported const reference alias initializer for '" + s->decl_name + "'";
                    return nullptr;
                }
                target.name = actual;
                target.canonical_name = actual;
                target.base_object = actual;
            }
            env.alias_graph.bind(s->decl_name, std::move(target));
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
            return nullptr;
        }
        if (!s->decl_type.struct_name.empty() && !s->decl_init_args.empty()) {
            auto fields = findStructFields(s->decl_type, env);
                if (fields) {
                const bool proxy_constructor_aliases = isProxyCarrierType(s->decl_type);
                auto bind_field_alias = [&](const StructFieldInfo& field, size_t arg_index) -> bool {
                    if (!field.type.is_reference && !field.type.is_pointer &&
                        !proxy_constructor_aliases) {
                        return true;
                    }
                    if (arg_index >= s->decl_init_args.size()) {
                        env.error = "Unsupported constructor reference alias for '" +
                                    s->decl_name + "." + field.name + "'";
                        return false;
                    }
                    std::string actual = directVarName(s->decl_init_args[arg_index]);
                    if (actual.empty()) {
                        env.error = "Unsupported constructor reference alias for '" +
                                    s->decl_name + "." + field.name + "'";
                        return false;
                    }
                    if (!env.symbols.count(actual)) {
                        env.error = "Unknown constructor alias target '" + actual + "'";
                        return false;
                    }
                    bool writable = (field.type.is_pointer || field.type.is_reference) && !field.type.is_const;
                    AliasTarget target{actual,
                                       field.type.is_pointer ? AliasKind::Pointer :
                                           (field.type.is_reference
                                                ? (writable ? AliasKind::MutableRef : AliasKind::ConstRef)
                                                : AliasKind::Value),
                                       writable};
                    target.reason = "constructor_member_initializer";
                    env.alias_graph.bindField(s->decl_name, field.name, std::move(target));
                    if (proxy_constructor_aliases && env.symbols.count(actual)) {
                        TypeInfo actual_type = env.symbols[actual];
                        if (actual_type.is_array) {
                            std::vector<int> prefix;
                            addFlattenedArraySymbols(env, actual, actual_type, true, prefix, 0);
                            env.initialized.insert(actual);
                            addSeedSymbolsWithPrefix(env, actual);
                        } else if (!writable) {
                            markScalarFullyInitialized(env, actual, actual_type);
                            addSeedSymbol(env, actual, actual_type);
                        }
                    }
                    return true;
                };

                auto ctors = findStructConstructors(s->decl_type, env);
                const StructConstructorInfo* selected_ctor = nullptr;
                if (ctors) {
                    for (const auto& ctor : *ctors) {
                        if (ctor.param_names.size() == s->decl_init_args.size()) {
                            selected_ctor = &ctor;
                            break;
                        }
                    }
                    if (!selected_ctor) {
                        std::ostringstream detail;
                        detail << " candidates=[";
                        bool first = true;
                        for (const auto& ctor : *ctors) {
                            if (!first) detail << ", ";
                            first = false;
                            detail << "params=" << ctor.param_names.size()
                                   << ", fields=" << ctor.field_to_param.size();
                        }
                        detail << "]";
                        env.error = "Unsupported constructor alias mapping for '" + s->decl_name +
                                    "': no constructor initializer matches argument count; args=" +
                                    std::to_string(s->decl_init_args.size()) +
                                    detail.str();
                        return nullptr;
                    }
                }

                if (selected_ctor) {
                    for (const auto& field : *fields) {
                        auto mapped = selected_ctor->field_to_param.find(field.name);
                        if (mapped == selected_ctor->field_to_param.end()) {
                            if (!field.type.is_reference && !field.type.is_pointer) continue;
                            env.error = "Unsupported constructor alias mapping for '" +
                                        s->decl_name + "." + field.name + "'";
                            return nullptr;
                        }
                        auto param_it = std::find(selected_ctor->param_names.begin(),
                                                  selected_ctor->param_names.end(),
                                                  mapped->second);
                        if (param_it == selected_ctor->param_names.end()) {
                            env.error = "Unsupported constructor alias parameter '" + mapped->second + "'";
                            return nullptr;
                        }
                        size_t arg_index = static_cast<size_t>(std::distance(selected_ctor->param_names.begin(), param_it));
                        if (!bind_field_alias(field, arg_index)) return nullptr;
                    }
                } else {
                    size_t count = std::min(fields->size(), s->decl_init_args.size());
                    for (size_t i = 0; i < count; ++i) {
                        const auto& field = (*fields)[i];
                        if (!bind_field_alias(field, i)) return nullptr;
                    }
                }
                if (proxy_constructor_aliases) {
                    auto aliased_port = [&](const std::string& token) -> std::string {
                        const auto* field = findStructFieldByToken(s->decl_type, env, token);
                        if (!field) return {};
                        auto alias = env.alias_graph.resolvePath(s->decl_name, {field->name});
                        return alias ? alias->canonical_name : std::string{};
                    };
                    const std::string enq_valid = aliased_port("enqvalid");
                    const std::string enq_data = aliased_port("enqdata");
                    if (!enq_valid.empty()) {
                        env.output_default_reasons[enq_valid] = "valid_default_false";
                    }
                    if (!enq_data.empty() && !enq_valid.empty()) {
                        auto enq_data_type = symbolType(env, enq_data, TypeInfo{});
                        env.formal_reads->insert(enq_data);
                        addSeedSymbol(env, enq_data, enq_data_type);
                    }
                    const std::string deq_ready = aliased_port("deqready");
                    if (!deq_ready.empty()) {
                        env.output_default_reasons[deq_ready] =
                            "write_enable_default_false";
                    }
                }
                // A completed constructor call defines the object handle. Its
                // reference fields are resolved through AliasGraph; value
                // fields remain available through normal flattened access.
                markScalarFullyInitialized(env, s->decl_name, s->decl_type);
                const bool alias_only = std::all_of(
                    fields->begin(), fields->end(), [](const StructFieldInfo& field) {
                        return field.type.is_reference || field.type.is_pointer;
                    });
                if (alias_only) {
                    return nullptr;
                }
            }
        }
        if (!s->decl_type.struct_name.empty() && s->decl_init.has_value() &&
            s->decl_init.value() && s->decl_init.value()->kind == ExprKind::Call &&
            s->decl_init.value()->args.empty() &&
            (s->decl_init.value()->callee == s->decl_type.struct_name ||
             s->decl_init.value()->callee == canonicalStructName(s->decl_type.name))) {
            r->decl_init = std::nullopt;
            return nullptr;
        }
        if (s->decl_init.has_value() && findStructFields(s->decl_type, env)) {
            const auto* fields = findStructFields(s->decl_type, env);
            if (fields && !fields->empty()) {
                int packed_width = flattenedTypeWidth(s->decl_type, env);
                if (packed_width <= 0) {
                    env.error = "Unsupported struct initialization with unknown flattened width for '" +
                                s->decl_name + "'";
                    return nullptr;
                }
                auto packed = rewriteExpr(s->decl_init.value(), env);
                if (!packed || !env.error.empty()) return nullptr;
                if (packed->type.width <= 0 || !packed->type.struct_name.empty()) {
                    packed = buildPackedStructValue(s->decl_init.value(),
                                                    make_hw_type("UInt", packed_width, false), env);
                    if (!packed || !env.error.empty()) {
                        if (env.error.empty()) {
                            env.error = "Unsupported struct initialization source for '" +
                                        s->decl_name + "'";
                        }
                        return nullptr;
                    }
                }
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed,
                                                make_hw_type("UInt", packed_width, false));
                }

                auto block = std::make_shared<Stmt>();
                block->kind = StmtKind::Block;
                int offset = 0;
                if (!appendStructUnpackDecls(block, s->decl_name, s->decl_type,
                                             packed, offset, env)) {
                    env.error = "Unsupported struct initialization source for '" +
                                s->decl_name + "'";
                    return nullptr;
                }
                markScalarFullyInitialized(env, s->decl_name, s->decl_type);
                return block;
            }
        }
        if (findStructFields(s->decl_type, env) && !s->decl_init.has_value()) {
            // VUL Int/UInt fields have a real default constructor value of
            // zero. Materialize only those fields; builtin scalar fields stay
            // uninitialized until explicitly assigned.
            auto block = std::make_shared<Stmt>();
            block->kind = StmtKind::Block;
            if (appendDefaultConstructedVulFields(
                    block, s->decl_name, s->decl_type, env)) {
                return block;
            }
            return nullptr;
        }
        const bool is_vul_fixed_int = s->decl_type.is_hw_int ||
            s->decl_type.hw_kind == "Int" || s->decl_type.hw_kind == "UInt" ||
            s->decl_type.hw_kind == "signed_view" ||
            s->decl_type.name.rfind("Int<", 0) == 0 ||
            s->decl_type.name.rfind("UInt<", 0) == 0;
        const bool is_empty_vul_constructor = s->decl_init.has_value() &&
            s->decl_init.value() && s->decl_init.value()->kind == ExprKind::Call &&
            s->decl_init.value()->args.empty() &&
            (s->decl_init.value()->callee == "Int" ||
             s->decl_init.value()->callee == "UInt" ||
             s->decl_init.value()->callee.rfind("Int<", 0) == 0 ||
             s->decl_init.value()->callee.rfind("UInt<", 0) == 0);
        if ((s->decl_default_constructed || is_empty_vul_constructor) && is_vul_fixed_int &&
            s->decl_type.width > 0) {
            r->decl_init = make_literal("0", s->decl_type);
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
            return r;
        }
        if (s->decl_init.has_value() && !s->decl_type.is_array) {
            r->decl_init = rewriteExpr(s->decl_init.value(), env);
            r->decl_init = castIfWidthChanges(r->decl_init.value(), s->decl_type);
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
        }
        return r;
    }
    case StmtKind::Assign: {
        if (auto bit_assign = rewriteBitSliceAssign(s, env)) {
            return bit_assign;
        }
        if (!env.error.empty()) return nullptr;
        auto value = rewriteExpr(s->assign_value, env);
        auto target = rewriteTarget(s->assign_target, env);
        if (!env.error.empty()) return nullptr;
        if (target && target->kind == ExprKind::VarRef) {
            TypeInfo target_type = target->type;
            if (env.symbols.count(target->var_name)) target_type = env.symbols[target->var_name];
            if (findStructFields(target_type, env)) {
                int packed_width = flattenedTypeWidth(target_type, env);
                if (packed_width <= 0) {
                    env.error = "Unsupported struct assignment with unknown flattened width for '" +
                                target->var_name + "'";
                    return nullptr;
                }
                ExprPtr packed = value;
                if (!packed || packed->type.width <= 0 || !packed->type.struct_name.empty()) {
                    packed = buildPackedStructValue(s->assign_value, make_hw_type("UInt", packed_width, false), env);
                    if (!packed || !env.error.empty()) {
                        if (env.error.empty()) {
                            env.error = "Unsupported struct assignment source for '" +
                                        target->var_name + "'";
                        }
                        return nullptr;
                    }
                }
                if (packed->type.width != packed_width) {
                    packed = castIfWidthChanges(packed, make_hw_type("UInt", packed_width, false));
                }
                auto block = std::make_shared<Stmt>();
                block->kind = StmtKind::Block;
                int offset = 0;
                if (!appendStructUnpackDecls(block, target->var_name, target_type,
                                             packed, offset, env)) {
                    env.error = "Unsupported struct assignment source for '" +
                                target->var_name + "'";
                    return nullptr;
                }
                markScalarFullyInitialized(env, target->var_name, target_type);
                return block;
            }
        }
        value = castIfWidthChanges(value, target->type);
        std::string name = targetName(target);
        if (!name.empty()) {
            env.symbols[name] = target->type;
            if (env.param_directions.count(name)) {
                markFormalWrite(env, name);
            }
            markScalarFullyInitialized(env, name, target->type);
        }
        r->assign_target = target;
        if (!name.empty() && isRegProxyWdataAliasTarget(name, env) && target->type.width > 0) {
            // setnext packs a complete next-state value. A full-width write
            // must not read the previous value of a pure output port.
            value = make_write_slice(zeroValueForType(target->type),
                                     target->type.width - 1,
                                     0,
                                     castIfWidthChanges(value, target->type),
                                     target->type);
        }
        r->assign_value = value;
        return r;
    }
    case StmtKind::If: {
        r->if_cond = rewriteExpr(s->if_cond, env);
        if (!env.error.empty()) return nullptr;
        Env then_env = env;
        Env else_env = env;
        r->if_then = rewriteStmts(s->if_then, then_env);
        r->if_else = rewriteStmts(s->if_else, else_env);
        if (!then_env.error.empty()) env.error = then_env.error;
        if (!else_env.error.empty()) env.error = else_env.error;
        if (!env.error.empty()) return nullptr;
        std::unordered_set<std::string> merged = env.initialized;
        for (auto& v : then_env.initialized) {
            if (else_env.initialized.count(v)) merged.insert(v);
        }
        env.initialized = std::move(merged);
        std::unordered_set<std::string> merged_resets = env.formal_write_reset_done;
        for (const auto& v : then_env.formal_write_reset_done) {
            if (else_env.formal_write_reset_done.count(v)) merged_resets.insert(v);
        }
        env.formal_write_reset_done = std::move(merged_resets);
        env.symbols.insert(then_env.symbols.begin(), then_env.symbols.end());
        env.symbols.insert(else_env.symbols.begin(), else_env.symbols.end());
        env.lookup_tables.insert(then_env.lookup_tables.begin(), then_env.lookup_tables.end());
        env.lookup_tables.insert(else_env.lookup_tables.begin(), else_env.lookup_tables.end());
        return r;
    }
    case StmtKind::Switch:
        r->switch_expr = rewriteExpr(s->switch_expr, env);
        {

        bool has_default = false;
        bool first_case = true;
        std::unordered_set<std::string> merged;
        for (auto& c : r->switch_cases) {
            if (c.value.has_value()) c.value = rewriteExpr(c.value.value(), env);
            else has_default = true;
            Env case_env = env;
            c.body = rewriteStmts(c.body, case_env);
            if (!case_env.error.empty()) {
                env.error = case_env.error;
                return nullptr;
            }
            if (first_case) {
                merged = case_env.initialized;
                first_case = false;
            } else {
                for (auto it = merged.begin(); it != merged.end();) {
                    if (!case_env.initialized.count(*it)) it = merged.erase(it);
                    else ++it;
                }
            }
            env.symbols.insert(case_env.symbols.begin(), case_env.symbols.end());
            env.lookup_tables.insert(case_env.lookup_tables.begin(), case_env.lookup_tables.end());
        }
        if (has_default && !first_case) env.initialized = std::move(merged);
        }
        return r;
    case StmtKind::Block:
        r->block_stmts = rewriteStmts(s->block_stmts, env);
        return r;
    case StmtKind::For:
        r->for_body = rewriteStmts(s->for_body, env);
        if (s->for_init) r->for_init = rewriteStmt(s->for_init, env);
        r->for_cond = rewriteExpr(s->for_cond, env);
        r->for_step = rewriteExpr(s->for_step, env);
        return r;
    case StmtKind::While:
    case StmtKind::DoWhile:
        env.error = "Unsupported dynamic loop; use statically bounded for-loop";
        return nullptr;
    case StmtKind::Return:
        if (s->return_value.has_value()) r->return_value = rewriteExpr(s->return_value.value(), env);
        return r;
    case StmtKind::ExprStmt:
        r->expr_stmt = rewriteExpr(s->expr_stmt, env);
        return r;
    default:
        return r;
    }
}

std::vector<StmtPtr> rewriteStmts(const std::vector<StmtPtr>& stmts, Env& env) {
    std::vector<StmtPtr> out;
    for (auto& s : stmts) {
        if (s && s->kind == StmtKind::If && s->if_cond &&
            s->if_cond->kind == ExprKind::Call &&
            (s->if_cond->intrinsic == IntrinsicKind::ReqHelperOutput ||
             s->if_cond->callee == "__vul_output")) {
            ExprPtr receiver;
            if (!s->if_cond->args.empty() && s->if_cond->args.front() &&
                s->if_cond->args.front()->kind == ExprKind::VarRef &&
                env.reqhelper_aliases.count(s->if_cond->args.front()->var_name)) {
                receiver = s->if_cond->args.front();
            }
            auto alias = resolveReqHelperAlias(receiver, env);
            if (!alias.has_value()) break;
            if (alias->rdy.empty()) {
                env.error = "ReqHelper call used as expression has no ready/return alias";
                break;
            }
            auto effects = inlineProcedureCall(s->if_cond, env);
            if (!env.error.empty()) break;
            auto rewritten_effects = rewriteStmts(effects, env);
            if (!env.error.empty()) break;
            out.insert(out.end(), rewritten_effects.begin(), rewritten_effects.end());

            auto lowered_if = std::make_shared<Stmt>(*s);
            lowered_if->if_cond = make_var(alias->rdy,
                                           symbolType(env, alias->rdy,
                                                      TypeInfo{"bool", 1, false, true, "bool"}));
            auto rewritten_if = rewriteStmt(lowered_if, env);
            if (!env.error.empty()) break;
            if (rewritten_if) out.push_back(rewritten_if);
            continue;
        }
        if (s && s->kind == StmtKind::Decl && s->decl_type.is_array &&
            !s->decl_type.init_values.empty() && !s->decl_type.is_static) {
            auto expanded = rewriteArrayInitDecl(s, env);
            if (!env.error.empty()) break;
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        if (s && s->kind == StmtKind::Decl && s->decl_type.is_array &&
            s->decl_init.has_value()) {
            auto expanded = rewritePackedArrayInitDecl(s, env);
            if (!env.error.empty()) break;
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        if (s && s->kind == StmtKind::ExprStmt && s->expr_stmt &&
            s->expr_stmt->kind == ExprKind::Call) {
            auto inlined = inlineProcedureCall(s->expr_stmt, env);
            if (!env.error.empty()) break;
            auto rewritten = rewriteStmts(inlined, env);
            if (!env.error.empty()) break;
            out.insert(out.end(), rewritten.begin(), rewritten.end());
            continue;
        }
        if (s && s->kind == StmtKind::Assign && s->assign_target &&
            s->assign_target->kind == ExprKind::ArrayAccess) {
            auto expanded = rewriteArrayAssign(s, env);
            if (!env.error.empty()) break;
            out.insert(out.end(), expanded.begin(), expanded.end());
            continue;
        }
        if (s && s->kind == StmtKind::Assign) {
            auto expanded = rewriteWholeArrayAssign(s, env);
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        auto r = rewriteStmt(s, env);
        if (!env.error.empty()) break;
        if (r) out.push_back(r);
    }
    return out;
}

} // namespace

NormalizeResult normalizeFunction(const FunctionAST& func,
                                  const std::vector<StmtPtr>& body) {
    NormalizeResult result;
    Env env;
    env.struct_fields = func.struct_fields;
    env.struct_constructors = func.struct_constructors;

    for (auto& p : func.params) {
        std::string pname = p.name;
        env.symbols[pname] = p.type;
        env.param_directions[pname] = paramDirectionName(p.direction);
        bool is_output = isOutputParam(p);
        bool is_input = isInputParam(p);
        bool mutable_formal = p.passing == ParamPassingKind::MutableRef && !p.is_const;
        if (is_output) addOutputParam(env.output_params, pname);
        if (is_input) {
            markScalarFullyInitialized(env, pname, p.type);
            env.input_arrays.insert(pname);
        }
        if (is_input || is_output) {
            addSeedSymbol(env, pname, p.type);
        }
        TypeInfo param_type = p.type;
        if (param_type.is_array && param_type.array_dims.empty() && param_type.array_size > 0) {
            param_type.array_dims = {param_type.array_size};
            env.symbols[pname] = param_type;
        }
        if (param_type.is_array && !param_type.array_dims.empty()) {
            std::vector<int> prefix;
            bool input_array_by_value = p.passing == ParamPassingKind::Value ||
                                        p.passing == ParamPassingKind::ConstRef;
            bool initialized_array = is_input || input_array_by_value;
            addFlattenedArraySymbols(env, pname, param_type, initialized_array, prefix, 0);
            if (initialized_array) {
                env.input_arrays.insert(pname);
                env.initialized.insert(pname);
            }
            if (is_input || is_output || input_array_by_value) {
                addSeedSymbolsWithPrefix(env, pname);
            }
        }
        if (!param_type.struct_name.empty()) {
            addStructFieldSymbols(pname, param_type, env, is_input);
            if (is_input || is_output) addSeedSymbolsWithPrefix(env, pname);
        }
    }
    registerHandshakePayloadDefaults(env);
    registerChildServiceResponseDefaults(env);
    registerRequestOutputPayloadDefaults(env);

    result.body = rewriteStmts(body, env);
    result.error = env.error;

    env.output_params.clear();
    for (const auto& p : func.params) {
        const std::string& name = p.name;
        std::string direction = paramDirectionName(p.direction);
        env.param_directions[name] = direction;
        if (direction == "Output") {
            addOutputParam(env.output_params, name);
        }
        if (direction == "Input" || direction == "Output") {
            addSeedSymbol(env, name, env.symbols[name]);
            addSeedSymbolsWithPrefix(env, name);
        } else {
            env.ssa_seed_symbols.erase(name);
            const std::string prefix = name + "_";
            for (auto it = env.ssa_seed_symbols.begin(); it != env.ssa_seed_symbols.end();) {
                if (it->first.rfind(prefix, 0) == 0) {
                    it = env.ssa_seed_symbols.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    registerHandshakePayloadDefaults(env);
    registerChildServiceResponseDefaults(env);
    registerRequestOutputPayloadDefaults(env);
    result.symbols = env.symbols;
    result.ssa_seed_symbols = env.ssa_seed_symbols;
    result.lookup_tables = env.lookup_tables;
    result.output_params = env.output_params;
    result.param_directions = env.param_directions;
    result.output_default_reasons = env.output_default_reasons;
    result.output_paired_controls = env.output_paired_controls;
    return result;
}

} // namespace pred
