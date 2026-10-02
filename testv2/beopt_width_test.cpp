#include "backend/beopt.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>

using namespace pred::beir;
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; std::exit(1); } } while (0)

static Operand literal(const llvm::APInt& value) {
    Operand out;
    out.kind = OperandKind::Literal;
    out.type = {static_cast<int>(value.getBitWidth()), {}};
    out.constant.width = out.type.width;
    out.constant.limbs.assign(value.getRawData(), value.getRawData() + value.getNumWords());
    return out;
}
static Operand signal(Program& program, Operation op) {
    Signal value;
    value.id = program.signals.size();
    value.name = "v" + std::to_string(value.id);
    value.type = op.type;
    value.driver = std::move(op);
    program.signals.push_back(value);
    Operand out;
    out.kind = OperandKind::Symbol;
    out.node = value.id;
    out.text = value.name;
    out.type = value.type;
    return out;
}
static Operand assign(Program& program, Operand input, int width) {
    Operation op;
    op.kind = OperationKind::ZExt;
    op.type = {width, {}};
    op.to_width = width;
    op.operands = {std::move(input)};
    return signal(program, std::move(op));
}
// Independent arbitrary-width evaluator: compare numeric values rather than IR shapes.
static llvm::APInt evaluate(const Program& program) {
    std::vector<std::optional<llvm::APInt>> cache(program.signals.size());
    std::function<llvm::APInt(const Operand&)> operand;
    std::function<llvm::APInt(NodeId)> node = [&](NodeId id) -> llvm::APInt {
        if (cache.at(id)) return *cache[id];
        const auto& current = program.signal(id);
        CHECK(current.driver.has_value());
        const auto& op = *current.driver;
        unsigned width = current.type.width;
        auto arg = [&](unsigned index) { return operand(op.operands.at(index)); };
        llvm::APInt value(width, 0);
        switch (op.kind) {
        case OperationKind::Assign: case OperationKind::Cast:
        case OperationKind::ZExt: case OperationKind::Trunc:
            value = arg(0).zextOrTrunc(width); break;
        case OperationKind::SExt: value = arg(0).sextOrTrunc(width); break;
        case OperationKind::Slice:
            value = arg(0).lshr(op.lo).zextOrTrunc(width); break;
        case OperationKind::Concat:
            for (unsigned i = 0; i < op.operands.size(); ++i) {
                auto part = arg(i);
                value = value.shl(part.getBitWidth()) | part.zextOrTrunc(width);
            }
            break;
        case OperationKind::WriteSlice: {
            auto mask = llvm::APInt::getBitsSet(width, op.lo, op.hi + 1);
            value = (arg(0).zextOrTrunc(width) & ~mask) |
                    (arg(1).zextOrTrunc(width).shl(op.lo) & mask);
            break;
        }
        case OperationKind::Binary:
            CHECK(op.op == OpCode::Add);
            value = arg(0).zextOrTrunc(width) + arg(1).zextOrTrunc(width); break;
        default: CHECK(false);
        }
        cache[id] = value;
        return value;
    };
    operand = [&](const Operand& input) {
        if (input.kind == OperandKind::Symbol) return node(input.node).zextOrTrunc(input.type.width);
        CHECK(input.kind == OperandKind::Literal);
        return llvm::APInt(input.constant.width, llvm::ArrayRef<uint64_t>(input.constant.limbs)).zextOrTrunc(input.type.width);
    };
    for (const auto& current : program.signals) {
        if (current.name == program.outputs.front()) return node(current.id);
    }
    CHECK(false);
    return llvm::APInt(1, 0);
}
static void checkSum(int width, const llvm::APInt& a, const llvm::APInt& b) {
    Program program;
    // Model a zeroed register temporary whose low slice is explicitly initialized.
    Operation write;
    write.kind = OperationKind::WriteSlice;
    write.type = {width, {}};
    write.lo = 0;
    write.hi = a.getBitWidth() - 1;
    write.operands = {literal(llvm::APInt(width, 0)), literal(a)};
    auto lhs = signal(program, std::move(write));
    auto rhs = assign(program, literal(b), width);
    Operation add;
    add.kind = OperationKind::Binary;
    add.op = OpCode::Add;
    add.type = {width, {}};
    add.operands = {lhs, rhs};
    auto output = signal(program, std::move(add));
    program.outputs.push_back(output.text);
    const auto expected = a.zextOrTrunc(width) + b.zextOrTrunc(width);
    CHECK(evaluate(program) == expected);
    auto optimized = pred::beir::opt::optimizeProgram(program);
    CHECK(evaluate(optimized) == expected);
    CHECK(evaluate(pred::beir::opt::optimizeProgram(optimized)) == expected);
}
int main() {
    for (int width : {8, 64, 65, 129}) {
        checkSum(width, llvm::APInt(3, 3), llvm::APInt(3, 5));
        checkSum(width, llvm::APInt(3, 7), llvm::APInt(1, 1));
        checkSum(width, llvm::APInt::getAllOnes(width), llvm::APInt(1, 1));
        if (width > 64) checkSum(width, llvm::APInt::getAllOnes(64), llvm::APInt(1, 1));
    }
    checkSum(3, llvm::APInt(3, 3), llvm::APInt(3, 5)); // Original modulo boundary.
    for (unsigned a = 0; a < 16; ++a) for (unsigned b = 0; b < 16; ++b)
        checkSum(8, llvm::APInt(4, a), llvm::APInt(4, b));
    std::cout << "beopt_width_test passed\n";
}
