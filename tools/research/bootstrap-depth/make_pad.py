"""Write a copy of bootstrap/loxpp_interpreter.lox whose interpret() call sits
under PAD extra native frames. Used by headroom.sh to find how many frames of
headroom each example has: the runner replaces __P__ per run.

usage: make_pad.py <bootstrap/loxpp_interpreter.lox> <out.lox>
"""
import sys
s = open(sys.argv[1]).read()
old = ("        var interpreter = Interpreter(resolver.locals);\n"
       "        interpreter.interpret(stmts);\n")
new = ("        var interpreter = Interpreter(resolver.locals);\n"
       "        fun __pad(d) { if (d > 0) { __pad(d - 1); return; } interpreter.interpret(stmts); }\n"
       "        __pad(PAD);\n")
assert s.count(old) == 1, "interpret() call site not found exactly once"
open(sys.argv[2], "w").write("var PAD = __P__;\n" + s.replace(old, new))
print("wrote", sys.argv[2])
