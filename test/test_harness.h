#pragma once
#include <string>
#include <memory>
#include <vector>
#include "value.h"
#include "chunk.h"

// Evaluates a Lox++ expression and returns the result as a Value.
Value eval_expr(const std::string& expr);

// Evaluates a Lox++ expression and returns the result as a string (for easy
// comparison in tests).
std::string eval_expr_str(const std::string& expr);

// Compiles a Lox++ expression and returns a readable string representation of
// the bytecode chunk.
std::string compile_to_bytecode(const std::string& expr);
