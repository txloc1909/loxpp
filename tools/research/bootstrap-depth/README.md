# Bootstrap stack-depth research probes

Evidence for the plan and the review on `txloc1909/loxpp` #248 (bootstrap
interpreter: no reliable guard against deep target-program recursion). This
branch is evidence only. Nothing here is a deliverable, and nothing under
`src/`, `runtime/`, `spec/`, or `bootstrap/` changes. Every number below was
measured on `main` at 041636c, release preset, in
`localhost/loxpp-dev-env-managed`.

## Files

| File | What it is |
|---|---|
| `probes/*.lox` | The probe programs. `_tmpl` files carry `__N__` (recursion or nesting depth), `__D__` (ambient call depth), `__E__` (an expression) for the runner to fill. |
| `measure.sh` | The runner. Modes: `catch`, `depth`, `nest "<D list>"`, `shapes`, `managed`, `catch2`, `proto`, `proto2`. Each does a binary search and prints "largest N that runs clean, then what N+1 does". |
| `headroom.sh` | The corpus padding sweep: for each `examples/*.lox` with `// CHECK:` lines, the smallest number of extra native frames that makes it crash under the bootstrap. |
| `make_noguard.sh` | Writes a copy of the interpreter with the `stringify()` guard out of reach (raw native ceiling). |
| `make_pad.py` | Writes a copy whose `interpret()` call sits under `PAD` extra native frames (used by `headroom.sh`). |
| `make_proto.py` | Writes the option-4 prototype: a global evaluator-depth counter at `execStmt`, `evalExpr`, `LoxFunction.call`, `stringify`, with threshold `T`. 18 changed lines. |
| `results/headroom.log` | Raw output of `headroom.sh`. |
| `results/corpus.log` | `tools/check_examples.py` on the shipped interpreter and on the prototype at `T=110`. |

## How to run

```bash
# release build in your own worktree, then generate the interpreter copies
cmake --preset release && cmake --build build -j"$(nproc)"
R=tools/research/bootstrap-depth
$R/make_noguard.sh bootstrap/loxpp_interpreter.lox $R/probes/interp_noguard.lox
python3 $R/make_pad.py bootstrap/loxpp_interpreter.lox $R/probes/interp_pad.lox
for T in 110 120 125; do python3 $R/make_proto.py bootstrap/loxpp_interpreter.lox $R/probes/interp_proto_$T.lox $T; done

# in the container, with the worktree at /workspace and probes/ at /probes
podman run --rm -v "$PWD":/workspace:z -v "$PWD/$R/probes":/probes:z localhost/loxpp-dev-env-managed \
  bash -lc '/workspace/tools/research/bootstrap-depth/measure.sh nest "0 10 20 30"'
podman run --rm -v "$PWD":/workspace:z -v "$PWD/$R/probes":/probes:z localhost/loxpp-dev-env-managed \
  bash -lc '/workspace/tools/research/bootstrap-depth/measure.sh shapes'
podman run --rm -v "$PWD":/workspace:z -v "$PWD/$R/probes":/probes:z localhost/loxpp-dev-env-managed \
  bash -lc '/workspace/tools/research/bootstrap-depth/measure.sh proto'
podman run --rm -v "$PWD":/workspace:z -v "$PWD/$R/probes":/probes:z localhost/loxpp-dev-env-managed \
  bash -lc '/workspace/tools/research/bootstrap-depth/headroom.sh'
```

`managed` and `catch2` need `tools/build_lox_rt.sh` and `tools/build_lox_rt_clr.sh` first.

## Results

### Ambient depth against stringify nesting (`measure.sh nest "0 10 20 30"`)

```
D=0 guard-on : 99 (N=100 -> guard: LOXERR70 Value nesting is too deep.)
D=0 no-guard : 120 (N=121 -> overflow: [line 3011] in script)
D=10 guard-on : 90 (N=91 -> overflow: [line 3011] in script)
D=10 no-guard : 90 (N=91 -> overflow: [line 3011] in script)
D=20 guard-on : 60 (N=61 -> overflow: [line 3011] in script)
D=20 no-guard : 60 (N=61 -> overflow: [line 3011] in script)
D=30 guard-on : 30 (N=31 -> overflow: [line 3011] in script)
D=30 no-guard : 30 (N=31 -> overflow: [line 3011] in script)
D=40 guard-on : 0 (N=1 -> overflow: [line 3011] in script)
```

### Recursion ceiling per shape, shipped interpreter (`measure.sh depth`, `shapes`, `proto2`)

```
native direct : 254 (N=255 -> overflow: [line 2] in script)
bootstrap     : 29 (N=30 -> overflow: [line 3011] in script)
tail   'if (n==0) return 0; return f(n-1);'      : 39 (N=40 -> overflow)
binary 'if (n==0) return 0; return f(n-1)+1;'    : 29 (N=30 -> overflow)
block  'if (n>0) { return f(n-1); } return 0;'   : 23 (N=24 -> overflow)
method : 39   ctor : 23   match : 29   super : 19
```

### Parser ceiling (`measure.sh shapes`, second half)

```
D=0 : 20
D=10 : 20
D=20 : 20
```

### Option-4 prototype (`measure.sh proto`, `proto2`)

```
### T=110: tail 34, binary 26, block 20; nest D=0 99 (guard), D=10 73, D=20 43, D=30 13; all clean-SOE
### T=120: tail 38, binary 28, block 22; method 38, ctor 22, match 28, super 18; nest D=10 83, D=20 53, D=30 23
### T=125: tail 39, binary 29, block 23; method 39, ctor 23, match 29, super 19; nest D=10 88, D=20 58, D=30 28
### every T: catch probe under bootstrap prints "caught StackOverflowError" / "Stack overflow." / "after", exit=0
```

`results/corpus.log`: shipped interpreter 88 passed, 0 failed, 12 skipped, 2.7 s.
Prototype at `T=110`: 87 passed, 1 failed (`parser.lox`), 2.5 s. `parser.lox`
also fails at `T=120` and passes at `T=125`.

### Headroom sweep (`results/headroom.log`)

```
HEADROOM 16 parser.lox
HEADROOM 73 memo_fib.lox
HEADROOM 146 test_stringify_depth_guard.lox
HEADROOM 163 fibonacci.lox
... 85 of 88 examples survive 150 or more extra native frames
```

### Catchability (`measure.sh catch`, `managed`, `catch2`)

```
native:   Stack overflow. + traceback, exit=70, catch block never runs
jvm:      java.lang.StackOverflowError, exit=1; f(5000) prints 5000 exit=0 (no ceiling)
clr:      f(255) -> Stack overflow. exit=134; catch block runs with e == nil, exit=0
```
