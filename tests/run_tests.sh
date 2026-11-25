#!/usr/bin/env bash
# Runs every macOS test program under Cider and checks stdout + exit code.
#   ./tests/run_tests.sh            run
#   ./tests/run_tests.sh --update   re-record expected output
set -uo pipefail
cd "$(dirname "$0")/.."

CIDER=build/cider.exe
BIN=build/tests
EXP=tests/expected
mkdir -p "$EXP"
update=0; [ "${1:-}" = "--update" ] && update=1
pass=0; fail=0

info() { "$CIDER" --info "$@" | grep -v '^Executable:'; return "${PIPESTATUS[0]}"; }

check() { # name expected-exit command...
  local name=$1 want=$2; shift 2
  local out code
  out=$("$@" 2>/dev/null); code=$?
  if [ $update = 1 ]; then printf '%s\n' "$out" > "$EXP/$name.txt"; fi
  if [ "$code" = "$want" ] && [ "$out" = "$(cat "$EXP/$name.txt" 2>/dev/null)" ]; then
    pass=$((pass + 1)); echo "  ok    $name"
  else
    fail=$((fail + 1)); echo "  FAIL  $name (exit $code, wanted $want)"
    diff <(printf '%s\n' "$out") "$EXP/$name.txt" | head -20
  fi
}

for flavour in classic chained; do
  echo "$flavour fixups:"
  check t_printf 0 $CIDER $BIN/$flavour/t_printf
  check t_libc 3 $CIDER $BIN/$flavour/t_libc arg1 "two words"
  check t_files 0 $CIDER $BIN/$flavour/t_files
  check t_threads 0 $CIDER $BIN/$flavour/t_threads
  check t_dylib 0 $CIDER $BIN/$flavour/t_dylib
  check t_missing 127 $CIDER $BIN/$flavour/t_missing
done

echo "architectures:"
check t_printf 0 $CIDER $BIN/t_universal
check arm64_only 1 $CIDER $BIN/t_arm64_only

if [ -d $BIN/fixtures ]; then
  echo "bundles and disk images:"
  check app_native 3 $CIDER "$BIN/fixtures/Cider Test.app" arg1 "two words"
  check dmg_native 3 $CIDER --fresh $BIN/fixtures/CiderTest.dmg arg1 "two words"
  check dmg_info 0 info $BIN/fixtures/CiderTest.dmg
  check app_java 0 $CIDER "$BIN/fixtures/Java Hello.app"
  check app_electron_info 0 info "$BIN/fixtures/Electron Hello.app"
fi

echo
echo "$pass passed, $fail failed"
[ $fail = 0 ]
