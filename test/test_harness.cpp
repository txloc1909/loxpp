#include "test_harness.h"
#include "vm.h"
#include "compiler.h"
#include "debug.h"
#include "simple_allocator.h"
#include <sstream>

Value eval_expr(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr + ";") != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return vm.lastResult();
}

std::string eval_expr_str(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr + ";") != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return stringify(vm.lastResult(), vm.allocator());
}

std::string compile_to_bytecode(const std::string& expr) {
    SimpleAllocator allocator;
    auto chunk = compile(expr + ";", &allocator);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    for (int offset = 0; offset < static_cast<int>(chunk->size());) {
        offset = disassembleInstruction(*chunk, allocator, offset, oss);
    }
    return oss.str();
}

std::string compile_program_to_bytecode(const std::string& source) {
    SimpleAllocator allocator;
    auto chunk = compile(source, &allocator);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    for (int offset = 0; offset < static_cast<int>(chunk->size());) {
        offset = disassembleInstruction(*chunk, allocator, offset, oss);
    }
    return oss.str();
}

InterpretResult run_program(const std::string& source) {
    VM vm;
    return vm.interpret(source);
}

// ---------------------------------------------------------------------------
// VMTestHarness
// ---------------------------------------------------------------------------

InterpretResult VMTestHarness::run(const std::string& source) {
    return m_vm.interpret(source);
}

std::string VMTestHarness::getGlobalStr(const std::string& name) const {
    auto v = m_vm.getGlobal(name);
    if (!v)
        return "<undefined>";
    return stringify(*v, m_vm.allocator());
}
