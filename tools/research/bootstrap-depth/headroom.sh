#!/bin/bash
# For each example that runs clean under the bootstrap, find the smallest number of
# extra native frames P+1 (P pad levels + the __pad(0) frame) that makes it crash.
cd /workspace
pad_run() { # $1 example path, $2 PAD
  sed "s/^var PAD = __P__;/var PAD = $2;/" ${PROBES:-/probes}/interp_pad.lox > /tmp/interp_pad.lox
  local inp="examples/$(basename "$1" .lox).input"
  { printf '__SOURCE_BEGIN__\n'; cat "$1"; printf '__SOURCE_END__\n'; [ -f "$inp" ] && cat "$inp"; } | timeout 60 build/loxpp /tmp/interp_pad.lox 2>&1
}
for ex in examples/*.lox; do
  grep -q '// CHECK:' "$ex" || continue
  out=$(pad_run "$ex" 0)
  if grep -q 'Stack overflow' <<<"$out"; then echo "CRASH-AT-PAD0 $(basename $ex)"; continue; fi
  if grep -q 'LOXERR' <<<"$out"; then echo "SKIP-LOXERR $(basename $ex)"; continue; fi
  lo=0; hi=250; first_crash=251
  while [ $lo -le $hi ]; do
    mid=$(( (lo + hi) / 2 ))
    if grep -q 'Stack overflow' <<<"$(pad_run "$ex" $mid)"; then first_crash=$mid; hi=$((mid - 1)); else lo=$((mid + 1)); fi
  done
  # headroom in native frames = pad frames that still pass = first_crash (P) frames: P pads survive... report P+1 minus 1
  echo "HEADROOM $first_crash $(basename $ex)"
done
