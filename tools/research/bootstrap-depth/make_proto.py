import sys
src_path, out_path, limit = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(src_path).read()
def rep(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"expected 1 match, got {n} for: {old[:60]!r}"
    s = s.replace(old, new)
GUARD_STMT = '        this.depth = this.depth + 1;\n        if (this.depth > DEPTH_LIMIT) { this.depth = this.depth - 1; this.setError("StackOverflowError", "Stack overflow.", 0); return; }\n'
GUARD_EXPR = '        this.depth = this.depth + 1;\n        if (this.depth > DEPTH_LIMIT) { this.depth = this.depth - 1; this.setError("StackOverflowError", "Stack overflow.", 0); return nil; }\n'
GUARD_CALL = '        interp.depth = interp.depth + 1;\n        if (interp.depth > DEPTH_LIMIT) { interp.depth = interp.depth - 1; interp.setError("StackOverflowError", "Stack overflow.", 0); return nil; }\n'
s = f"var DEPTH_LIMIT = {limit};\n" + s
rep('        this.stringifyDepth = 0;\n', '        this.stringifyDepth = 0;\n        this.depth = 0;\n')
rep('    execStmt(stmt, env) {\n        match stmt {\n', '    execStmt(stmt, env) {\n' + GUARD_STMT + '        match stmt {\n')
rep('        };\n    }\n\n    execFor(', '        };\n        this.depth = this.depth - 1;\n    }\n\n    execFor(')
rep('    evalExpr(expr, env) {\n        return match expr {\n', '    evalExpr(expr, env) {\n' + GUARD_EXPR + '        var r = match expr {\n')
rep('        };\n    }\n\n    lookupVariable(', '        };\n        this.depth = this.depth - 1;\n        return r;\n    }\n\n    lookupVariable(')
rep('    call(interp, args) {\n        var callEnv = Environment(this.closure);\n', '    call(interp, args) {\n' + GUARD_CALL + '        var callEnv = Environment(this.closure);\n')
rep('        if (this.isInitializer) return this.closure.getAt(0, "this");\n        return returnVal;\n', '        interp.depth = interp.depth - 1;\n        if (this.isInitializer) return this.closure.getAt(0, "this");\n        return returnVal;\n')
rep('        this.stringifyDepth = this.stringifyDepth + 1;\n', '        this.stringifyDepth = this.stringifyDepth + 1;\n        this.depth = this.depth + 1;\n        if (this.depth > DEPTH_LIMIT) { this.depth = this.depth - 1; this.stringifyDepth = this.stringifyDepth - 1; this.setError("StackOverflowError", "Stack overflow.", 0); return "..."; }\n')
rep('            this.stringifyDepth = this.stringifyDepth - 1;\n            this.setError("MaxDepthExceededError"', '            this.stringifyDepth = this.stringifyDepth - 1;\n            this.depth = this.depth - 1;\n            this.setError("MaxDepthExceededError"')
rep('        this.stringifyDepth = this.stringifyDepth - 1;\n        return result;\n', '        this.stringifyDepth = this.stringifyDepth - 1;\n        this.depth = this.depth - 1;\n        return result;\n')
open(out_path, 'w').write(s)
print(f"wrote {out_path} limit={limit}")
