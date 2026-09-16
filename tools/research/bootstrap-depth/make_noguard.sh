#!/bin/bash
# Copy of the interpreter with the stringify() nesting guard moved out of reach,
# to measure the raw native ceiling instead of the guard.
# usage: make_noguard.sh <bootstrap/loxpp_interpreter.lox> <out.lox>
sed 's/if (this.stringifyDepth > 100) {/if (this.stringifyDepth > 1000000) {/' "$1" > "$2"
grep -q 'stringifyDepth > 1000000' "$2" && echo "wrote $2"
