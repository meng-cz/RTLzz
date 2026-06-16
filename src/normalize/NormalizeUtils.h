#pragma once

#include "ast/AST.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred {

bool isUIntName(const std::string& name);
int explicitHwWidthFromName(const std::string& name);

bool isMutableParam(const ParamDecl& p);
bool isOutputParam(const ParamDecl& p);
bool isInputParam(const ParamDecl& p);

std::string baseName(const ExprPtr& e);
ExprPtr cloneExpr(const ExprPtr& e);
bool isConstantExpr(const ExprPtr& e);
bool isWidthCastableConstantExpr(const ExprPtr& e);

TypeInfo resultTypeForBinary(const std::string& op, const TypeInfo& a, const TypeInfo& b);
ExprPtr castIfWidthChanges(ExprPtr value, const TypeInfo& target_type);
void normalizeConstantOperandsForBinary(const std::string& op, ExprPtr& lhs, ExprPtr& rhs);
void normalizeConstantBranchesForTernary(ExprPtr& then_expr, ExprPtr& else_expr);

int literalIntValue(const ExprPtr& e, int fallback = -1);
bool isSignedViewExpr(const ExprPtr& e);
ExprPtr foldConstantOvershift(const std::string& op, const ExprPtr& lhs, const ExprPtr& rhs);

std::string directVarName(const ExprPtr& e);
bool fieldAccessPath(const ExprPtr& e, std::string& object, std::vector<std::string>& fields);
std::string canonicalStructName(std::string name);

ExprPtr substituteInlineExpr(const ExprPtr& e,
                             const std::unordered_map<std::string, ExprPtr>& args);
StmtPtr substituteInlineStmt(const StmtPtr& s,
                             const std::unordered_map<std::string, ExprPtr>& args);
std::vector<StmtPtr> substituteInlineStmts(const std::vector<StmtPtr>& stmts,
                                           const std::unordered_map<std::string, ExprPtr>& args);
void collectVarRefs(const ExprPtr& e, std::vector<std::string>& order);
void collectStmtVarRefs(const StmtPtr& s, std::vector<std::string>& order);

bool isTrueLiteral(const ExprPtr& e);
bool isFalseLiteral(const ExprPtr& e);
ExprPtr notExpr(ExprPtr e);
ExprPtr andExpr(ExprPtr a, ExprPtr b);
StmtPtr guardStmt(ExprPtr guard, StmtPtr stmt);
bool isVoidReturnStmt(const StmtPtr& s);
bool containsReturnStmt(const std::vector<StmtPtr>& body);
std::vector<StmtPtr> localizeProcedureReturns(const std::vector<StmtPtr>& stmts,
                                              const std::string& callee,
                                              std::string& error);
StmtPtr makeAssignStmt(ExprPtr target, ExprPtr value);
ExprPtr makeLookupExpr(const std::string& table_name, const TypeInfo& table_type, ExprPtr index);

bool collectArrayAccess(const ExprPtr& e, ExprPtr& base, std::vector<ExprPtr>& indices);
TypeInfo scalarTypeFromArray(TypeInfo t);
std::string joinIndexName(const std::string& base, const std::vector<int>& idxs);
int flatElementCount(const TypeInfo& type);
std::string targetName(const ExprPtr& e);
std::optional<int> literalIndex(const ExprPtr& e);

} // namespace pred
