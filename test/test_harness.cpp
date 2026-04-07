#include "test_harness.h"
#include "vm.h"
#include "compiler.h"
#include "debug.h"
#include "function.h"
#include "memory_manager.h"
#include "object.h"
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
    return stringify(vm.lastResult());
}

std::string compile_to_bytecode(const std::string& expr) {
    MemoryManager mm;
    ObjFunction* fn = compile(expr + ";", &mm);
    if (!fn)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    const Chunk& chunk = fn->chunk;
    for (int offset = 0; offset < static_cast<int>(chunk.size());) {
        offset = disassembleInstruction(chunk, mm, offset, oss);
    }
    return oss.str();
}

std::string compile_program_to_bytecode(const std::string& source) {
    MemoryManager mm;
    ObjFunction* fn = compile(source, &mm);
    if (!fn)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    const Chunk& chunk = fn->chunk;
    for (int offset = 0; offset < static_cast<int>(chunk.size());) {
        offset = disassembleInstruction(chunk, mm, offset, oss);
    }
    return oss.str();
}

InterpretResult run_program(const std::string& source) {
    VM vm;
    return vm.interpret(source);
}

// ---------------------------------------------------------------------------
// compile_fn_body_to_bytecode
// ---------------------------------------------------------------------------

std::string compile_fn_body_to_bytecode(const std::string& source, int n) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (!script)
        throw std::runtime_error("Compilation failed");

    int found = 0;
    const Chunk& scriptChunk = script->chunk;
    for (int i = 0; i < static_cast<int>(scriptChunk.size());) {
        Byte op = scriptChunk.at(i);
        if (toOpcode(op) == Op::CONSTANT) {
            uint8_t idx = scriptChunk.at(i + 1);
            Value v = scriptChunk.getConstant(idx);
            if (is<Obj*>(v) && isObjType(as<Obj*>(v), ObjType::FUNCTION)) {
                if (found == n) {
                    ObjFunction* fn = asObjFunction(v);
                    std::ostringstream oss;
                    const Chunk& chunk = fn->chunk;
                    for (int offset = 0;
                         offset < static_cast<int>(chunk.size());) {
                        offset = disassembleInstruction(chunk, mm, offset, oss);
                    }
                    return oss.str();
                }
                found++;
            }
            i += 2;
        } else {
            i++;
        }
    }
    throw std::runtime_error("Not enough inner functions found");
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
    return stringify(*v);
}
