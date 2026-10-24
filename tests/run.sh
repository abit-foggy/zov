#!/bin/sh
# Phase 1 smoke tests: compile with zovc, run, verify exit status / stdout.
set -u
cd "$(dirname "$0")/.."

ZOV=./zovc
TMP=${TMPDIR:-/tmp}/zov-tests
mkdir -p "$TMP"
fails=0

run_exit() { # name file expected_status
    name=$1; file=$2; want=$3
    out="$TMP/$name"
    if ! $ZOV "$file" -o "$out" 2> "$TMP/$name.err"; then
        echo "FAIL: $name (compile)"; cat "$TMP/$name.err"; fails=$((fails + 1)); return
    fi
    "$out" >/dev/null 2>&1
    got=$?
    if [ "$got" != "$want" ]; then
        echo "FAIL: $name (exit $got, want $want)"; fails=$((fails + 1)); return
    fi
    echo "PASS: $name"
}

run_stdout() { # name file expected_stdout_file
    name=$1; file=$2; wantfile=$3
    out="$TMP/$name"
    if ! $ZOV "$file" -o "$out" 2> "$TMP/$name.err"; then
        echo "FAIL: $name (compile)"; cat "$TMP/$name.err"; fails=$((fails + 1)); return
    fi
    "$out" > "$TMP/$name.out" 2>&1
    if ! cmp -s "$TMP/$name.out" "$wantfile"; then
        echo "FAIL: $name (stdout mismatch)"
        diff "$wantfile" "$TMP/$name.out" | sed 's/^/    /'
        fails=$((fails + 1)); return
    fi
    echo "PASS: $name"
}

run_exit  01_arith tests/01_arith.zov 42
run_stdout 02_defer  tests/02_defer.zov  tests/02_defer.expected
run_stdout 03_ffi    tests/03_ffi.zov    tests/03_ffi.expected
run_stdout 04_union_try tests/04_union_try.zov tests/04_union_try.expected
rm -f zov_c_nullable_file.txt
run_stdout 05_c_nullable_file tests/05_c_nullable_file.zov tests/05_c_nullable_file.expected
rm -f zov_c_nullable_file.txt

run_exit  06_borrow tests/06_borrow_and_moves.zov 0
run_exit  07_generics tests/07_generics.zov 0

run_neg() { # negative sema case: compilation MUST fail
    file=$1
    if $ZOV "$file" -o "$TMP/neg_out" 2> "$TMP/neg.err"; then
        echo "FAIL: $file (compiled but must be rejected)"
        fails=$((fails + 1))
        return
    fi
    echo "PASS: $file"
}

for neg in tests/neg/*.zov; do
    run_neg "$neg"
done

if [ "$fails" -ne 0 ]; then
    echo "$fails test(s) failed"
    exit 1
fi
echo "all tests passed"
