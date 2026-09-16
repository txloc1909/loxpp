#!/bin/bash
# Runs inside the container. /workspace = my worktree, /probes = my scratchpad probes.
LOXPP=${LOXPP:-/workspace/build/loxpp}
INTERP=${INTERP:-/workspace/bootstrap/loxpp_interpreter.lox}
NOGUARD=${NOGUARD:-/probes/interp_noguard.lox}
P=${PROBES:-/probes}
run_native() { timeout 30 "$LOXPP" "$1" 2>&1; echo "exit=$?"; }
run_boot() {
  local prog="$1" interp="${2:-$INTERP}"
  { printf '__SOURCE_BEGIN__\n'; cat "$prog"; printf '__SOURCE_END__\n'; } | timeout 120 "$LOXPP" "$interp" 2>&1
  echo "exit=${PIPESTATUS[1]}"
}
classify() {
  local out="$1"
  if grep -q 'LOXERR70 Stack overflow' <<<"$out"; then echo clean-SOE
  elif grep -q 'Stack overflow' <<<"$out"; then echo overflow
  elif grep -q -E "MaxDepthExceededError|Value nesting is too deep" <<<"$out"; then echo guard
  elif grep -q 'exit=0' <<<"$out"; then echo ok
  else echo other; fi
}
# bsearch <mode> <tmpl> <lo> <hi> [D] : largest N in [lo,hi] whose run is "ok"
bsearch() {
  local mode="$1" tmpl="$2" lo="$3" hi="$4" D="${5:-0}" interp="${6:-$INTERP}"
  local best=0 mid out cls
  while [ "$lo" -le "$hi" ]; do
    mid=$(( (lo + hi) / 2 ))
    sed -e "s/__N__/$mid/" -e "s/__D__/$D/" "$tmpl" > /tmp/prog.lox
    case "$mode" in
      native) out=$(run_native /tmp/prog.lox) ;;
      boot)   out=$(run_boot /tmp/prog.lox "$interp") ;;
    esac
    cls=$(classify "$out")
    if [ "$cls" = ok ]; then best=$mid; lo=$((mid + 1)); else hi=$((mid - 1)); fi
  done
  # report the first failing classification at best+1
  sed -e "s/__N__/$((best + 1))/" -e "s/__D__/$D/" "$tmpl" > /tmp/prog.lox
  case "$mode" in native) out=$(run_native /tmp/prog.lox) ;; boot) out=$(run_boot /tmp/prog.lox "$interp") ;; esac
  echo "$best (N=$((best + 1)) -> $(classify "$out"): $(echo "$out" | grep -v '^exit=' | tail -1 | cut -c1-80))"
}
case "$1" in
  catch)
    echo "### native, direct"; run_native $P/catch_overflow.lox
    echo "### bootstrap on native"; run_boot $P/catch_overflow.lox ;;
  depth)
    echo "### plain recursion f(N): max N"
    echo "native direct : $(bsearch native $P/depth_tmpl.lox 1 4000)"
    echo "bootstrap     : $(bsearch boot   $P/depth_tmpl.lox 1 400)" ;;
  nest)
    for D in ${2:-0 20 40}; do
      echo "D=$D guard-on : $(bsearch boot $P/nest_tmpl.lox 1 300 $D)"
      echo "D=$D no-guard : $(bsearch boot $P/nest_tmpl.lox 1 300 $D $NOGUARD)"
    done ;;
esac
case "$1" in
  managed)
    cd /workspace
    for be in jvm clr; do
      echo "### $be: catch_overflow.lox"
      timeout 120 tools/loxpp_$be.sh $P/catch_overflow.lox 2>&1 | head -12; echo "exit=${PIPESTATUS[0]}"
      for n in 254 255 1000 5000; do
        echo "### $be: f($n)"
        timeout 120 tools/loxpp_$be.sh $P/depth_$n.lox 2>&1 | head -4; echo "exit=${PIPESTATUS[0]}"
      done
    done ;;
esac
case "$1" in
  catch2)
    cd /workspace
    echo "### native: catch_overflow2.lox"; timeout 60 build/loxpp $P/catch_overflow2.lox > /tmp/o.txt 2>&1; echo "exit=$?"; grep -v '^\[line' /tmp/o.txt | head -8
    for be in jvm clr; do
      echo "### $be: catch_overflow2.lox"; timeout 120 tools/loxpp_$be.sh $P/catch_overflow2.lox > /tmp/o.txt 2>&1; echo "exit=$?"; head -8 /tmp/o.txt
    done
    echo "### clr: f(255) exit code"; timeout 120 tools/loxpp_clr.sh $P/depth_255.lox > /tmp/o.txt 2>&1; echo "exit=$?"; head -3 /tmp/o.txt
    echo "### jvm: f(255) with try/catch"; timeout 120 tools/loxpp_jvm.sh $P/catch_overflow.lox > /tmp/o.txt 2>&1; echo "exit=$?"; head -2 /tmp/o.txt ;;
esac
case "$1" in
  shapes)
    echo "### bootstrap max N per recursion shape"
    echo "tail   'if (n==0) return 0; return f(n-1);'      : $(bsearch boot $P/depth_tail_tmpl.lox 1 400)"
    echo "binary 'if (n==0) return 0; return f(n-1)+1;'    : $(bsearch boot $P/depth_tmpl.lox 1 400)"
    echo "block  'if (n>0) { return f(n-1); } return 0;'   : $(bsearch boot $P/depth_if_tmpl.lox 1 400)"
    echo "### bootstrap: nested parens expression ((((1)))) at ambient D, max paren depth"
    for D in 0 10 20; do
      lo=1; hi=300; best=0
      while [ $lo -le $hi ]; do mid=$(( (lo+hi)/2 )); E=$(printf '(%.0s' $(seq $mid))1$(printf ')%.0s' $(seq $mid)); sed -e "s/__D__/$D/" -e "s/__E__/$E/" $P/expr_tmpl.lox > /tmp/prog.lox; out=$(run_boot /tmp/prog.lox); if [ "$(classify "$out")" = ok ]; then best=$mid; lo=$((mid+1)); else hi=$((mid-1)); fi; done
      echo "D=$D : $best"
    done ;;
esac

case "$1" in
  proto)
    for T in 110 120 125; do
      I=$P/interp_proto_$T.lox
      echo "### T=$T shapes: best ok N, then what N+1 does"
      echo "tail   : $(bsearch boot $P/depth_tail_tmpl.lox 1 400 0 $I)"
      echo "binary : $(bsearch boot $P/depth_tmpl.lox 1 400 0 $I)"
      echo "block  : $(bsearch boot $P/depth_if_tmpl.lox 1 400 0 $I)"
      echo "### T=$T nest at ambient D: best ok N, then what N+1 does"
      for D in 0 10 20 30; do echo "D=$D : $(bsearch boot $P/nest_tmpl.lox 1 300 $D $I)"; done
      echo "### T=$T catch probe under bootstrap"
      run_boot $P/catch_overflow.lox $I | grep -v '^\[line' | head -5
    done ;;
esac
case "$1" in
  proto2)
    for T in 120 125; do
      I=$P/interp_proto_$T.lox
      echo "### T=$T extra shapes: best ok N, then what N+1 does"
      for sh in method ctor match super; do
        echo "$sh : $(bsearch boot $P/depth_${sh}_tmpl.lox 1 400 0 $I)"
      done
    done
    echo "### unpatched interpreter, same shapes (native ceiling)"
    for sh in method ctor match super; do
      echo "$sh : $(bsearch boot $P/depth_${sh}_tmpl.lox 1 400 0)"
    done ;;
esac
