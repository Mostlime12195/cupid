#!/bin/bash
# Runs every test with the JIT off, compiling everything (threshold 1), and
# with a threshold that mixes interpreted and compiled frames; outputs must match.
cd "$(dirname "$0")"
failures=0
for t in tests/*.lua; do
  if ! expected=$(node run.mjs --nojit "$t" 2>&1); then
    echo "BROKEN $t (fails without the JIT)"; echo "$expected" | grep -v '^    at' | tail -3
    failures=$((failures + 1)); continue
  fi
  ok=1
  for th in 1 3; do
    actual=$(node run.mjs --threshold $th "$t" 2>&1)
    if [ "$expected" != "$actual" ]; then
      echo "FAIL $t (threshold $th)"
      diff <(echo "$expected") <(echo "$actual") | head -20
      ok=0
    fi
  done
  if [ $ok = 1 ]; then
    echo "ok   $t ($(node run.mjs --stats "$t" 2>&1 >/dev/null | tail -1))"
  else
    failures=$((failures + 1))
  fi
done
echo "$failures failure(s)"
[ $failures = 0 ]
