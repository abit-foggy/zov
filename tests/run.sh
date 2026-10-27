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
run_stdout 08_println tests/08_println_chain.zov tests/08_println_chain.expected
rm -f zov_std_vec_elems.txt
run_exit  09_std_vec tests/09_std_vec_and_methods.zov 0
rm -f zov_std_fs_test.txt
run_exit  10_std_fs tests/10_std_fs.zov 0
rm -f zov_std_fs_test.txt
run_stdout 12_enums tests/12_enums_and_slices.zov tests/12_enums_and_slices.expected
run_stdout 13_std_arena tests/13_std_arena_and_str.zov tests/13_std_arena_and_str.expected
run_stdout 14_bitwise_map tests/14_bitwise_and_map.zov tests/14_bitwise_and_map.expected
run_stdout 15_capstone tests/15_capstone_interpreter.zov tests/15_capstone_interpreter.expected
run_stdin 17_std_io tests/17_std_io.zov "first line" tests/17_std_io.expected
run_exit  18_std_time tests/18_std_time.zov 0
run_exit  19_std_math tests/19_std_math_and_separators.zov 0

run_stdin() { # name file input_line expected_stdout_file
    name=$1; file=$2; input=$3; wantfile=$4
    out="$TMP/$name"
    if ! $ZOV "$file" -o "$out" 2> "$TMP/$name.err"; then
        echo "FAIL: $name (compile)"; cat "$TMP/$name.err"; fails=$((fails + 1)); return
    fi
    printf '%s\n' "$input" | "$out" > "$TMP/$name.out" 2>&1
    if ! cmp -s "$TMP/$name.out" "$wantfile"; then
        echo "FAIL: $name (stdout mismatch)"
        diff "$wantfile" "$TMP/$name.out" | sed 's/^/    /'
        fails=$((fails + 1)); return
    fi
    echo "PASS: $name"
}

# test-block runner: zovc test <file>
if $ZOV test tests/11_test_blocks.zov > "$TMP/11.out" 2>&1; then
    if cmp -s "$TMP/11.out" tests/11_test_blocks.expected; then
        echo "PASS: 11_test_blocks"
    else
        echo "FAIL: 11_test_blocks (output mismatch)"
        diff tests/11_test_blocks.expected "$TMP/11.out" | sed 's/^/    /'
        fails=$((fails + 1))
    fi
else
    echo "FAIL: 11_test_blocks (zovc test exited nonzero)"
    fails=$((fails + 1))
fi

# workspace package flow: build, run, and test via zov.toml
WSAPP=tests/pkg_workspace/app
rm -rf "$WSAPP/target" "$WSAPP/zov.lock"
if $ZOV build -C "$WSAPP" > "$TMP/ws.out" 2>&1 \
   && test -f "$WSAPP/zov.lock" \
   && test -f "$WSAPP/target/app"; then
    echo "PASS: pkg build + zov.lock + target/app"
else
    echo "FAIL: pkg build"
    cat "$TMP/ws.out" | sed 's/^/    /'
    fails=$((fails + 1))
fi
if $ZOV run -C "$WSAPP" > "$TMP/wsrun.out" 2>&1 \
   && grep -q "triple(7)=21" "$TMP/wsrun.out" \
   && grep -q "sqrt(4)=2" "$TMP/wsrun.out"; then
    echo "PASS: pkg run output"
else
    echo "FAIL: pkg run output"
    cat "$TMP/wsrun.out" | sed 's/^/    /'
    fails=$((fails + 1))
fi
if $ZOV test -C "$WSAPP" > "$TMP/wstest.out" 2>&1 \
   && grep -q "2 passed, 0 failed" "$TMP/wstest.out"; then
    echo "PASS: pkg test harness"
else
    echo "FAIL: pkg test harness"
    cat "$TMP/wstest.out" | sed 's/^/    /'
    fails=$((fails + 1))
fi
rm -rf "$WSAPP/target" "$WSAPP/zov.lock"

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

# --check: valid file exits 0 silently, invalid file exits 1 with a
# file:line:col diagnostic row
if $ZOV --check tests/01_arith.zov 2>"$TMP/chk.err" \
   && test ! -s "$TMP/chk.err"; then
    echo "PASS: --check valid"
else
    echo "FAIL: --check valid"
    cat "$TMP/chk.err" | sed 's/^/    /'
    fails=$((fails + 1))
fi
printf 'export fn main() i32 = {\n    let q: i32 = "bad";\n    return 0;\n};\n' \
    > "$TMP/invalid.zov"
if $ZOV --check "$TMP/invalid.zov" >"$TMP/chk.out" 2>"$TMP/chk.err"; then
    echo "FAIL: --check invalid (exited 0)"
    fails=$((fails + 1))
else
    if grep -qE '^[^:]+:[0-9]+:[0-9]+: error: ' "$TMP/chk.err"; then
        echo "PASS: --check invalid diagnostic format"
    else
        echo "FAIL: --check invalid diagnostic format"
        cat "$TMP/chk.err" | sed 's/^/    /'
        fails=$((fails + 1))
    fi
fi

# zov-lsp: JSON-RPC lifecycle with live diagnostics and completion
if test -x ./zov-lsp; then
    python3 tests/lsp_protocol.py "$TMP" ./zov-lsp "$PWD/tests/lsp_fixture.zov"
    if [ $? -eq 0 ]; then
        echo "PASS: zov-lsp JSON-RPC protocol"
    else
        echo "FAIL: zov-lsp JSON-RPC protocol"
        fails=$((fails + 1))
    fi
else
    echo "FAIL: zov-lsp binary missing"
    fails=$((fails + 1))
fi

# zovc cc: drop-in C99 compiler driver
if printf '#include <stdio.h>\nint main(void){printf("cc ok\\n");return 0;}\n' \
        > "$TMP/cc_standalone.c" \
   && $ZOV cc "$TMP/cc_standalone.c" -o "$TMP/cc_standalone" 2>/dev/null \
   && "$TMP/cc_standalone" > "$TMP/cc1.out" 2>&1 \
   && grep -q "cc ok" "$TMP/cc1.out"; then
    echo "PASS: zovc cc standalone"
else
    echo "FAIL: zovc cc standalone"
    fails=$((fails + 1))
fi
if printf 'int helper(void){return 42;}\n' > "$TMP/cc_helper.c" \
   && $ZOV cc -c "$TMP/cc_helper.c" -o "$TMP/cc_helper.o" 2>/dev/null \
   && printf 'int helper(void);\nint main(void){return helper() - 42;}\n' \
        > "$TMP/cc_main.c" \
   && $ZOV cc "$TMP/cc_helper.o" "$TMP/cc_main.c" -o "$TMP/cc_app" \
        2>/dev/null \
   && "$TMP/cc_app"; then
    echo "PASS: zovc cc object + link"
else
    echo "FAIL: zovc cc object + link"
    fails=$((fails + 1))
fi

# freestanding mode: zero hosted headers, panic-routed asserts, and a
# clean -ffreestanding -nostdlib object with only the kernel hook undefined
if $ZOV --freestanding --emit-c tests/16_freestanding_smoke.zov \
        > "$TMP/fs.c" 2>"$TMP/fs.err"; then
    hosted_refs=$(grep -cE "stdio|stdlib|abort|main\(int" "$TMP/fs.c" || true)
    if [ "$hosted_refs" = "0" ] \
       && grep -q "zov_panic" "$TMP/fs.c" \
       && grep -q "zov_kmain" "$TMP/fs.c"; then
        echo "PASS: 16_freestanding emit"
    else
        echo "FAIL: 16_freestanding emit (hosted refs: $hosted_refs)"
        fails=$((fails + 1))
    fi
    if cc -ffreestanding -nostdlib -mno-red-zone -fno-pie -std=c99 \
         -Wall -Wextra -Werror -c "$TMP/fs.c" -o "$TMP/fs.o" \
         2>"$TMP/fscc.err"; then
        if [ "$(nm -u "$TMP/fs.o" | grep -c zov_panic)" = "1" ] \
           && [ "$(nm -u "$TMP/fs.o" | grep -cv zov_panic)" = "0" ]; then
            echo "PASS: 16_freestanding object"
        else
            echo "FAIL: 16_freestanding object symbols"
            nm -u "$TMP/fs.o" | sed 's/^/    /'
            fails=$((fails + 1))
        fi
    else
        echo "FAIL: 16_freestanding object compile"
        cat "$TMP/fscc.err" | sed 's/^/    /' | head -4
        fails=$((fails + 1))
    fi
else
    echo "FAIL: 16_freestanding emit (zovc error)"
    cat "$TMP/fs.err" | sed 's/^/    /'
    fails=$((fails + 1))
fi

# manifest: freestanding = true builds a kernel .o object
FSPJ="$TMP/fsproj"
rm -rf "$FSPJ"
mkdir -p "$FSPJ/src"
printf '[package]\nname = "zovos"\nversion = "0.1.0"\nentry = "src/kernel.zov"\nfreestanding = true\n' \
    > "$FSPJ/zov.toml"
printf 'export fn kmain() void = {\n    let mut x: i32 = 1;\n    x = x + 1;\n};\n' \
    > "$FSPJ/src/kernel.zov"
if $ZOV build -C "$FSPJ" > "$TMP/fsb.out" 2>&1 \
   && test -f "$FSPJ/target/zovos.o"; then
    echo "PASS: freestanding manifest object build"
else
    echo "FAIL: freestanding manifest object build"
    cat "$TMP/fsb.out" | sed 's/^/    /' | head -3
    fails=$((fails + 1))
fi
rm -rf "$FSPJ"

if [ "$fails" -ne 0 ]; then
    echo "$fails test(s) failed"
    exit 1
fi
echo "all tests passed"
