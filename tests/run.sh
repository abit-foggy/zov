#!/bin/sh
# Zov test runner: compile, execute, and verify diagnostics.
#
# The runner is deliberately explicit instead of using `set -e`: tests are
# expected to fail compilation, and each command's status must be classified
# as success, a Zov diagnostic, a timeout, or a crash.  All generated files
# live under $TMP, outside the repository.
set -u
LC_ALL=C
export LC_ALL
unset CDPATH

cd "$(dirname "$0")/.." || {
    echo "FAIL: cannot enter repository root" >&2
    exit 1
}

ROOT=$(pwd)
ZOV=$ROOT/zovc
ZOV_LSP=$ROOT/zov-lsp
ZOV_STD_PATH=${ZOV_STD_PATH:-$ROOT/lib/std}
export ZOV_STD_PATH

TMP=${ZOV_TEST_TMP:-${TMPDIR:-/tmp}/zov-tests.$$}
case $TMP in
    /|'') echo "FAIL: refusing unsafe test temp directory '$TMP'" >&2; exit 1 ;;
    /*) ;;
    *)
        echo "FAIL: ZOV_TEST_TMP must be an absolute path outside the repository: '$TMP'" >&2
        exit 1
        ;;
esac
if [ "$TMP" = "$ROOT" ]; then
    echo "FAIL: refusing to use the repository root as the test temp directory" >&2
    exit 1
fi
case $ROOT/ in
    "$TMP"/*)
        echo "FAIL: refusing test temp directory '$TMP' because it contains the repository" >&2
        exit 1
        ;;
esac

if ! rm -rf "$TMP" || ! mkdir -p "$TMP"; then
    echo "FAIL: cannot create test temp directory '$TMP'" >&2
    exit 1
fi

TIMEOUT_SECS=${ZOV_TEST_TIMEOUT:-30}
case $TIMEOUT_SECS in
    ''|*[!0-9]*)
        echo "FAIL: ZOV_TEST_TIMEOUT must be a positive integer" >&2
        exit 1
        ;;
esac
if [ "$TIMEOUT_SECS" -le 0 ]; then
    echo "FAIL: ZOV_TEST_TIMEOUT must be greater than zero" >&2
    exit 1
fi

fails=0
passed=0
skipped=0

fail() {
    fails=$((fails + 1))
    printf 'FAIL: %s\n' "$*"
}

pass() {
    passed=$((passed + 1))
    printf 'PASS: %s\n' "$*"
}

skip() {
    skipped=$((skipped + 1))
    printf 'SKIP: %s\n' "$*"
}

show_file() {
    if [ -f "$1" ]; then
        sed 's/^/    /' "$1"
    fi
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "required command not found: $1"
        return 1
    fi
    return 0
}

require_file() {
    if [ ! -f "$1" ]; then
        fail "missing fixture or expected file: $1"
        return 1
    fi
    return 0
}

require_dir() {
    if [ ! -d "$1" ]; then
        fail "missing directory: $1"
        return 1
    fi
    return 0
}

require_exec() {
    if [ ! -x "$1" ]; then
        fail "missing executable: $1"
        return 1
    fi
    return 0
}

# Run a command with separate stdout/stderr logs and a bounded lifetime.
# `timeout` is used when available; the background watchdog is a POSIX-only
# fallback for systems (notably stock macOS) where it is not installed.
run_limited() {
    _stdout=$1
    _stderr=$2
    shift 2

    if command -v timeout >/dev/null 2>&1; then
        timeout "$TIMEOUT_SECS" "$@" >"$_stdout" 2>"$_stderr"
        return $?
    fi

    "$@" >"$_stdout" 2>"$_stderr" &
    _pid=$!
    (
        sleep "$TIMEOUT_SECS"
        kill -TERM "$_pid" 2>/dev/null
        sleep 1
        kill -KILL "$_pid" 2>/dev/null
    ) &
    _watchdog=$!
    wait "$_pid"
    _status=$?
    kill "$_watchdog" 2>/dev/null || true
    wait "$_watchdog" 2>/dev/null || true
    return "$_status"
}

compile_zov() { # name source output
    name=$1
    source=$2
    output=$3
    require_file "$source" || return 1
    run_limited "$TMP/$name.compile.out" "$TMP/$name.compile.err" \
        "$ZOV" "$source" -o "$output"
    status=$?
    if [ "$status" -eq 124 ]; then
        fail "$name (compiler timed out after ${TIMEOUT_SECS}s)"
        show_file "$TMP/$name.compile.err"
        return 1
    fi
    if [ "$status" -ge 128 ]; then
        fail "$name (compiler terminated by signal; status $status)"
        show_file "$TMP/$name.compile.err"
        return 1
    fi
    if [ "$status" -ne 0 ]; then
        fail "$name (compiler exited $status)"
        show_file "$TMP/$name.compile.err"
        return 1
    fi
    if [ ! -x "$output" ]; then
        fail "$name (compiler produced no executable: $output)"
        show_file "$TMP/$name.compile.err"
        return 1
    fi
    return 0
}

run_program() { # name executable stdin
    name=$1
    executable=$2
    input=$3
    run_limited "$TMP/$name.out" "$TMP/$name.err" \
        sh -c 'cd "$1" && exec "$2" < "$3"' \
        sh "$TMP" "$executable" "$input"
}

check_program_status() { # name status expected_status
    _name=$1
    _status=$2
    _want=$3
    if [ "$_status" -eq 124 ]; then
        fail "$_name (program timed out after ${TIMEOUT_SECS}s)"
        show_file "$TMP/$_name.err"
        return 1
    fi
    if [ "$_status" -ge 128 ]; then
        fail "$_name (program terminated by signal; status $_status)"
        show_file "$TMP/$_name.err"
        return 1
    fi
    if [ "$_status" -ne "$_want" ]; then
        fail "$_name (program exited $_status, want $_want)"
        show_file "$TMP/$_name.err"
        return 1
    fi
    return 0
}

run_exit() { # name source expected_status
    name=$1
    source=$2
    want=$3
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" /dev/null
    status=$?
    check_program_status "$name" "$status" "$want" || return
    pass "$name"
}

run_stdout() { # name source expected_stdout
    name=$1
    source=$2
    expected=$3
    require_file "$expected" || return
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" /dev/null
    status=$?
    check_program_status "$name" "$status" 0 || return
    if ! cmp -s "$TMP/$name.out" "$expected"; then
        fail "$name (stdout mismatch)"
        diff "$expected" "$TMP/$name.out" | sed 's/^/    /'
        show_file "$TMP/$name.err"
        return
    fi
    pass "$name"
}

run_stdin() { # name source input_line expected_stdout
    name=$1
    source=$2
    input=$3
    expected=$4
    require_file "$expected" || return
    if ! printf '%s\n' "$input" >"$TMP/$name.stdin"; then
        fail "$name (cannot create stdin fixture)"
        return
    fi
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" "$TMP/$name.stdin"
    status=$?
    check_program_status "$name" "$status" 0 || return
    if ! cmp -s "$TMP/$name.out" "$expected"; then
        fail "$name (stdout mismatch)"
        diff "$expected" "$TMP/$name.out" | sed 's/^/    /'
        show_file "$TMP/$name.err"
        return
    fi
    pass "$name"
}

check_panic_report() { # name code message_log
    name=$1
    code=$2
    message=$3
    log=$4
    if [ "$(head -1 "$log" 2>/dev/null)" != "Zov runtime error" ]; then
        fail "$name (missing runtime error header)"
        show_file "$log"
        return 1
    fi
    if ! grep -qF "error[$code]: $message" "$log"; then
        fail "$name (missing runtime diagnostic [$code]: $message)"
        show_file "$log"
        return 1
    fi
    if ! grep -qE '^  --> .*\.zov:[0-9]+:[0-9]+$' "$log"; then
        fail "$name (runtime panic lost its source location)"
        show_file "$log"
        return 1
    fi
    if grep -q '?:0:0' "$log"; then
        fail "$name (runtime panic used a fake source location)"
        show_file "$log"
        return 1
    fi
    return 0
}

run_panic() { # name source expected_code expected_stderr_text
    name=$1
    source=$2
    code=$3
    expected=$4
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" /dev/null
    status=$?
    if [ "$status" -eq 124 ]; then
        fail "$name (program timed out after ${TIMEOUT_SECS}s)"
        show_file "$TMP/$name.err"
        return
    fi
    if [ "$status" -ge 128 ]; then
        fail "$name (program terminated by signal; status $status)"
        show_file "$TMP/$name.err"
        return
    fi
    if [ "$status" -ne 101 ]; then
        fail "$name (program exited $status, want panic status 101)"
        show_file "$TMP/$name.err"
        return
    fi
    check_panic_report "$name" "$code" "$expected" "$TMP/$name.err" || return
    pass "$name"
}

run_panic_release() { # name source expected_code expected_stderr_text
    label=$1
    source=$2
    code=$3
    expected=$4
    output=$TMP/$label-release
    run_limited "$TMP/$label-release.compile.out" \
        "$TMP/$label-release.compile.err" \
        "$ZOV" --release "$source" -o "$output"
    status=$?
    if [ "$status" -ne 0 ] || [ ! -x "$output" ]; then
        fail "$label (release compile exited $status)"
        show_file "$TMP/$label-release.compile.err"
        return
    fi
    run_program "$label-release" "$output" /dev/null
    status=$?
    if [ "$status" -ne 101 ]; then
        fail "$label (release run exited $status, want 101)"
        show_file "$TMP/$label-release.err"
        return
    fi
    check_panic_report "${label}-release" "$code" "$expected" \
        "$TMP/$label-release.err" || return
    pass "$label release"
}

run_neg() { # absolute source path
    source=$1
    base=${source##*/}
    require_file "$source" || return

    entry=$(awk -F'|' -v f="$base" '$1 == f { print; exit }' "$NEG_EXPECTED")
    if [ -z "$entry" ]; then
        fail "$base (no entry in tests/neg/expected.txt)"
        return
    fi

    IFS='|' read -r _mfile expect_line expect_col expect_code expect_msg <<EOF
$entry
EOF

    case $expect_line in
        ''|*[!0-9]*)
            fail "$base (malformed expected.txt entry: invalid line '$expect_line')"
            return
            ;;
    esac
    case $expect_col in
        ''|*[!0-9]*)
            fail "$base (malformed expected.txt entry: invalid column '$expect_col')"
            return
            ;;
    esac
    if [ -z "$expect_msg" ]; then
        fail "$base (malformed expected.txt entry: empty message)"
        return
    fi

    rm -f "$TMP/neg_out"
    run_limited "$TMP/neg_$base.out" "$TMP/neg_$base.err" \
        "$ZOV" "$source" -o "$TMP/neg_out"
    status=$?

    if [ "$status" -eq 0 ]; then
        fail "$base (compiled but must be rejected)"
        show_file "$TMP/neg_$base.err"
        return
    fi
    if [ "$status" -eq 124 ]; then
        fail "$base (compiler timed out after ${TIMEOUT_SECS}s)"
        show_file "$TMP/neg_$base.err"
        return
    fi
    if [ "$status" -ge 128 ]; then
        fail "$base (compiler crashed or was signaled; status $status)"
        show_file "$TMP/neg_$base.err"
        return
    fi
    if [ "$status" -ne 1 ]; then
        fail "$base (compiler exited $status; expected a diagnostic exit of 1)"
        show_file "$TMP/neg_$base.err"
        return
    fi

    first_error=$(awk '/error:/ { print; exit }' "$TMP/neg_$base.err")
    if [ -z "$first_error" ]; then
        fail "$base (compiler failed without a Zov error diagnostic)"
        show_file "$TMP/neg_$base.err"
        return
    fi

    case $first_error in
        "$source:$expect_line:$expect_col: error: "*) ;;
        *)
            fail "$base (diagnostic location/message mismatch)"
            printf '    expected: %s:%s:%s: error: ...\n' \
                "$source" "$expect_line" "$expect_col"
            printf '    got:      %s\n' "$first_error"
            return
            ;;
    esac

    if [ "$expect_code" = "-" ] || [ -z "$expect_code" ]; then
        fail "$base (expected.txt must assert a diagnostic code)"
        return
    fi
    case $expect_code in
        [EW][0-9][0-9][0-9][0-9]) ;;
        *)
            fail "$base (malformed diagnostic code '$expect_code')"
            return
            ;;
    esac
    case $first_error in
        *" error: [$expect_code] "*)
            ;;
        *)
            fail "$base (expected error code [$expect_code])"
            printf '    got:      %s\n' "$first_error"
            return
            ;;
    esac

    if ! printf '%s\n' "$first_error" | grep -Fq "$expect_msg"; then
        fail "$base (diagnostic message does not contain '$expect_msg')"
        printf '    got:      %s\n' "$first_error"
        return
    fi

    pass "$base"
}

run_check() {
    run_limited "$TMP/check.out" "$TMP/check.err" \
        "$ZOV" --check "$ROOT/tests/01_arith.zov"
    status=$?
    if [ "$status" -eq 0 ] && [ ! -s "$TMP/check.err" ]; then
        pass "--check valid"
    else
        fail "--check valid (status $status, or unexpected stderr)"
        show_file "$TMP/check.out"
        show_file "$TMP/check.err"
    fi

    printf 'export fn main() i32 = {\n    let q: i32 = "bad";\n    return 0;\n};\n' \
        >"$TMP/invalid.zov"
    run_limited "$TMP/check-invalid.out" "$TMP/check-invalid.err" \
        "$ZOV" --check "$TMP/invalid.zov"
    status=$?
    if [ "$status" -eq 0 ]; then
        fail "--check invalid (exited 0)"
        return
    fi
    if [ "$status" -eq 124 ]; then
        fail "--check invalid (timed out after ${TIMEOUT_SECS}s)"
        return
    fi
    if [ "$status" -ge 128 ]; then
        fail "--check invalid (compiler crashed or was signaled; status $status)"
        show_file "$TMP/check-invalid.err"
        return
    fi
    if [ "$status" -ne 1 ]; then
        fail "--check invalid (exited $status, want 1)"
        show_file "$TMP/check-invalid.err"
        return
    fi
    expected_diag="$TMP/invalid.zov:2:5: error: [E0601] cannot bind str to i32"
    if grep -Fq "$expected_diag" "$TMP/check-invalid.err"; then
        pass "--check invalid diagnostic location"
    else
        fail "--check invalid diagnostic location (missing '$expected_diag')"
        show_file "$TMP/check-invalid.err"
    fi
}

run_import_diagnostic() {
    mkdir -p "$TMP/importmod"
    printf 'export fn bad() i32 = {\n    return "nope";\n};\n' \
        >"$TMP/importmod/helper.zov"
    printf 'use helper;\n\nexport fn main() i32 = {\n    return helper::bad();\n};\n' \
        >"$TMP/importmod/main.zov"
    run_limited "$TMP/import.out" "$TMP/import.err" \
        sh -c 'cd "$1" && exec "$2" --check main.zov' \
        sh "$TMP/importmod" "$ZOV"
    status=$?
    if [ "$status" -ne 1 ]; then
        fail "imported-file diagnostic (status $status, want 1)"
        show_file "$TMP/import.err"
        return
    fi
    if grep -Fq "./helper.zov:2:5: error: [E0601]" "$TMP/import.err"; then
        pass "imported-file diagnostic location and code"
    else
        fail "imported-file diagnostic (missing helper.zov:2:5 [E0601])"
        show_file "$TMP/import.err"
    fi
}

run_diagnostic_registry() {
    run_limited "$TMP/diagcodes.out" "$TMP/diagcodes.err" \
        "$ZOV" diagnostics
    status=$?
    if [ "$status" -ne 0 ] || [ ! -s "$TMP/diagcodes.out" ]; then
        fail "diagnostics listing (status $status)"
        show_file "$TMP/diagcodes.err"
        return
    fi
    dup=$(awk '{print $1}' "$TMP/diagcodes.out" | sort | uniq -d)
    if [ -n "$dup" ]; then
        fail "diagnostics listing has duplicate codes: $dup"
        return
    fi
    if ! grep -q "^E0402  undefined symbol$" "$TMP/diagcodes.out" \
        || ! grep -q "^E0503  use of moved value$" "$TMP/diagcodes.out"; then
        fail "diagnostics listing is missing stable codes"
        return
    fi
    pass "diagnostic code registry"

    run_limited "$TMP/explain.out" "$TMP/explain.err" \
        "$ZOV" explain E0402
    status=$?
    if [ "$status" -eq 0 ] && grep -q "undefined symbol" "$TMP/explain.out"; then
        pass "explain diagnostic code"
    else
        fail "explain diagnostic code"
        show_file "$TMP/explain.out"
        show_file "$TMP/explain.err"
    fi
}

run_lsp() {
    require_exec "$ZOV_LSP" || return
    require_cmd python3 || return
    require_file "$ROOT/tests/lsp_protocol.py" || return
    run_limited "$TMP/lsp.out" "$TMP/lsp.err" \
        sh -c 'cd "$1" && exec python3 "$2" "$3" "$4" "$5"' \
        sh "$TMP" "$ROOT/tests/lsp_protocol.py" "$TMP" \
        "$ZOV_LSP" "$ROOT/tests/01_arith.zov"
    status=$?
    if [ "$status" -eq 0 ]; then
        pass "zov-lsp JSON-RPC protocol"
    elif [ "$status" -eq 124 ]; then
        fail "zov-lsp JSON-RPC protocol (timed out after ${TIMEOUT_SECS}s)"
        show_file "$TMP/lsp.err"
    else
        fail "zov-lsp JSON-RPC protocol (exited $status)"
        show_file "$TMP/lsp.out"
        show_file "$TMP/lsp.err"
    fi
}

run_zovc_cc() {
    printf '#include <stdio.h>\nint main(void){printf("cc ok\\n");return 0;}\n' \
        >"$TMP/cc_standalone.c"
    run_limited "$TMP/cc1.compile.out" "$TMP/cc1.compile.err" \
        "$ZOV" cc "$TMP/cc_standalone.c" -o "$TMP/cc_standalone"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "zovc cc standalone (compile exited $status)"
        show_file "$TMP/cc1.compile.err"
    else
        run_limited "$TMP/cc1.out" "$TMP/cc1.err" \
            sh -c 'cd "$1" && exec "$2" < /dev/null' sh "$TMP" \
            "$TMP/cc_standalone"
        status=$?
        if [ "$status" -eq 0 ] && grep -q "cc ok" "$TMP/cc1.out"; then
            pass "zovc cc standalone"
        else
            fail "zovc cc standalone (exit $status or missing output)"
            show_file "$TMP/cc1.out"
            show_file "$TMP/cc1.err"
        fi
    fi

    printf 'int helper(void){return 42;}\n' >"$TMP/cc_helper.c"
    printf 'int helper(void);\nint main(void){return helper() - 42;}\n' \
        >"$TMP/cc_main.c"
    run_limited "$TMP/cc2a.compile.out" "$TMP/cc2a.compile.err" \
        "$ZOV" cc -c "$TMP/cc_helper.c" -o "$TMP/cc_helper.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "zovc cc object + link (helper compile exited $status)"
        show_file "$TMP/cc2a.compile.err"
        return
    fi
    run_limited "$TMP/cc2b.compile.out" "$TMP/cc2b.compile.err" \
        "$ZOV" cc "$TMP/cc_helper.o" "$TMP/cc_main.c" -o "$TMP/cc_app"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "zovc cc object + link (link exited $status)"
        show_file "$TMP/cc2b.compile.err"
        return
    fi
    run_limited "$TMP/cc2.out" "$TMP/cc2.err" \
        sh -c 'cd "$1" && exec "$2" < /dev/null' sh "$TMP" "$TMP/cc_app"
    status=$?
    if [ "$status" -eq 0 ]; then
        pass "zovc cc object + link"
    else
        fail "zovc cc object + link (program exited $status)"
        show_file "$TMP/cc2.err"
    fi
}

run_freestanding() {
    require_cmd cc || return
    require_cmd nm || return
    source=$ROOT/tests/16_freestanding_smoke.zov
    require_file "$source" || return

    run_limited "$TMP/fs.c" "$TMP/fs.err" \
        "$ZOV" --freestanding --emit-c "$source"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "16_freestanding emit (zovc exited $status)"
        show_file "$TMP/fs.err"
        return
    fi
    hosted_refs=$(grep -cE 'stdio|stdlib|abort|main\(int' "$TMP/fs.c" || true)
    if [ "$hosted_refs" != "0" ] \
       || ! grep -q "zov_panic" "$TMP/fs.c" \
       || ! grep -q "zov_kmain" "$TMP/fs.c"; then
        fail "16_freestanding emit (hosted refs: $hosted_refs)"
        return
    fi
    pass "16_freestanding emit"

    run_limited "$TMP/fscc.out" "$TMP/fscc.err" \
        cc -ffreestanding -nostdlib -mno-red-zone -fno-pie -std=c99 \
        -pedantic-errors -Wall -Wextra -Werror -c "$TMP/fs.c" -o "$TMP/fs.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "16_freestanding object compile (cc exited $status)"
        show_file "$TMP/fscc.err"
        return
    fi
    run_limited "$TMP/nm.out" "$TMP/nm.err" nm -u "$TMP/fs.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "16_freestanding object symbols (nm exited $status)"
        show_file "$TMP/nm.err"
        return
    fi
    panic_refs=$(grep -c "zov_panic" "$TMP/nm.out" || true)
    other_refs=$(grep -cv "zov_panic" "$TMP/nm.out" || true)
    if [ "$panic_refs" = "1" ] && [ "$other_refs" = "0" ]; then
        pass "16_freestanding object"
    else
        fail "16_freestanding object symbols (zov_panic=$panic_refs, other=$other_refs)"
        show_file "$TMP/nm.out"
    fi

    # freestanding builds are allocation-free: String allocation must be
    # rejected at compile time with an actionable diagnostic
    printf 'export fn kmain() void = {\n    let s = String::from("x");\n    s.free();\n};\n' \
        >"$TMP/fs_alloc.zov"
    run_limited "$TMP/fs_alloc.c" "$TMP/fs_alloc.err" \
        "$ZOV" --freestanding --emit-c "$TMP/fs_alloc.zov"
    status=$?
    if [ "$status" -eq 0 ]; then
        fail "freestanding String allocation was accepted"
    elif [ "$status" -ge 128 ]; then
        fail "freestanding String allocation check crashed (status $status)"
    elif ! grep -q "allocation is unavailable in freestanding" \
        "$TMP/fs_alloc.err"; then
        fail "freestanding String allocation diagnostic missing"
        show_file "$TMP/fs_alloc.err"
    else
        pass "freestanding allocation diagnostic"
    fi
}

run_freestanding_link() {
    require_cmd cc || return
    require_file "$ROOT/runtime/example/hosted_runtime.c" || return

    run_limited "$TMP/rt_smoke.c" "$TMP/rt_smoke.emit.err" \
        "$ZOV" --freestanding --emit-c \
        "$ROOT/tests/16_freestanding_smoke.zov"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding runtime link (emit exited $status)"
        show_file "$TMP/rt_smoke.emit.err"
        return
    fi
    run_limited "$TMP/rt_smoke.cc.out" "$TMP/rt_smoke.cc.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        -ffreestanding -c "$TMP/rt_smoke.c" -o "$TMP/rt_smoke.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding runtime link (object compile exited $status)"
        show_file "$TMP/rt_smoke.cc.err"
        return
    fi
    run_limited "$TMP/rt_smoke.link.out" "$TMP/rt_smoke.link.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        "$ROOT/runtime/example/hosted_runtime.c" "$TMP/rt_smoke.o" \
        -o "$TMP/rt_smoke"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding runtime link (link exited $status)"
        show_file "$TMP/rt_smoke.link.err"
        return
    fi
    run_limited "$TMP/rt_smoke.out" "$TMP/rt_smoke.err" \
        sh -c 'exec "$1" < /dev/null' sh "$TMP/rt_smoke"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding runtime link (program exited $status)"
        show_file "$TMP/rt_smoke.err"
        return
    fi
    pass "freestanding runtime link"

    # Panic hook invocation: the platform hook receives the stable code
    # and the Zov source site.
    printf 'export fn kmain() void = {\n    assert(false);\n};\n' \
        >"$TMP/rt_panic.zov"
    run_limited "$TMP/rt_panic.c" "$TMP/rt_panic.emit.err" \
        "$ZOV" --freestanding --emit-c "$TMP/rt_panic.zov"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding panic hook (emit exited $status)"
        show_file "$TMP/rt_panic.emit.err"
        return
    fi
    run_limited "$TMP/rt_panic.cc.out" "$TMP/rt_panic.cc.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        -ffreestanding -c "$TMP/rt_panic.c" -o "$TMP/rt_panic.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding panic hook (object compile exited $status)"
        show_file "$TMP/rt_panic.cc.err"
        return
    fi
    run_limited "$TMP/rt_panic.link.out" "$TMP/rt_panic.link.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        "$ROOT/runtime/example/hosted_runtime.c" "$TMP/rt_panic.o" \
        -o "$TMP/rt_panic"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding panic hook (link exited $status)"
        show_file "$TMP/rt_panic.link.err"
        return
    fi
    run_limited "$TMP/rt_panic.out" "$TMP/rt_panic.err" \
        sh -c 'exec "$1" < /dev/null' sh "$TMP/rt_panic"
    status=$?
    if [ "$status" -ne 101 ]; then
        fail "freestanding panic hook (program exited $status, want 101)"
        show_file "$TMP/rt_panic.err"
        return
    fi
    if ! grep -qF 'error[E1006]: assert failed' "$TMP/rt_panic.err" \
        || ! grep -qE '^  --> .*rt_panic\.zov:[0-9]+:[0-9]+$' \
            "$TMP/rt_panic.err"; then
        fail "freestanding panic hook (missing code or source location)"
        show_file "$TMP/rt_panic.err"
        return
    fi
    pass "freestanding panic hook"
}

run_strict_c99() {
    require_cmd cc || return
    strict_fail=0
    strict_count=0

    for src in "$ROOT"/tests/[0-9][0-9]_*.zov; do
        [ -f "$src" ] || continue
        base=${src##*/}
        base=${base%.zov}
        case $base in
            11_*|16_*) continue ;;
        esac
        strict_count=$((strict_count + 1))
        cfile=$TMP/strict-$base.c
        if ! run_limited "$cfile" "$TMP/strict-$base.emit.err" \
            "$ZOV" --emit-c "$src"; then
            fail "strict C99 emit: $base"
            show_file "$TMP/strict-$base.emit.err"
            strict_fail=1
            continue
        fi
        if grep -q '__builtin_trap' "$cfile"; then
            fail "strict C99 codegen: $base emits __builtin_trap"
            strict_fail=1
        fi
        if ! run_limited "$TMP/strict-$base.cc.out" \
            "$TMP/strict-$base.cc.err" \
            cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
            -c "$cfile" -o "$TMP/strict-$base.o"; then
            fail "strict C99 compile: $base"
            sed 's/^/    /' "$TMP/strict-$base.cc.err"
            strict_fail=1
        fi
    done

    # test-mode runner and freestanding output use different entry points
    if run_limited "$TMP/strict-test11.c" "$TMP/strict-test11.emit.err" \
        "$ZOV" --test --emit-c "$ROOT/tests/11_test_blocks.zov"; then
        strict_count=$((strict_count + 1))
        if ! run_limited "$TMP/strict-test11.cc.out" \
            "$TMP/strict-test11.cc.err" \
            cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
            -c "$TMP/strict-test11.c" -o "$TMP/strict-test11.o"; then
            fail "strict C99 compile: 11_test_blocks (test runner)"
            sed 's/^/    /' "$TMP/strict-test11.cc.err"
            strict_fail=1
        fi
    else
        fail "strict C99 emit: 11_test_blocks (test runner)"
        show_file "$TMP/strict-test11.emit.err"
        strict_fail=1
    fi

    if run_limited "$TMP/strict-fs.c" "$TMP/strict-fs.emit.err" \
        "$ZOV" --freestanding --emit-c \
        "$ROOT/tests/16_freestanding_smoke.zov"; then
        strict_count=$((strict_count + 1))
        if ! run_limited "$TMP/strict-fs.cc.out" "$TMP/strict-fs.cc.err" \
            cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
            -ffreestanding -c "$TMP/strict-fs.c" -o "$TMP/strict-fs.o"; then
            fail "strict C99 compile: 16_freestanding_smoke"
            sed 's/^/    /' "$TMP/strict-fs.cc.err"
            strict_fail=1
        fi
    else
        fail "strict C99 emit: 16_freestanding_smoke"
        show_file "$TMP/strict-fs.emit.err"
        strict_fail=1
    fi

    if [ "$strict_fail" -eq 0 ]; then
        pass "strict C99 codegen ($strict_count fixtures)"
    fi
}

run_freestanding_arch() {
    source=$ROOT/tests/16_freestanding_smoke.zov
    require_file "$source" || return
    mkdir -p "$TMP/fakebin" || {
        fail "freestanding arch (cannot create fake cc directory)"
        return
    }
    cat >"$TMP/fakebin/cc" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$ZOV_FAKECC_LOG"
out=
while [ "$#" -gt 0 ]; do
    case $1 in
        -o) out=$2; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$out" ] && : >"$out"
cat >/dev/null
exit 0
SH
    chmod +x "$TMP/fakebin/cc" || {
        fail "freestanding arch (cannot make fake cc executable)"
        return
    }

    : >"$TMP/fs-arch-aarch64.log"
    run_limited "$TMP/fs-arch-aarch64.out" "$TMP/fs-arch-aarch64.err" \
        env ZOV_FAKECC_LOG="$TMP/fs-arch-aarch64.log" \
        ZOV_TARGET_ARCH=aarch64 PATH="$TMP/fakebin:$PATH" \
        "$ZOV" --freestanding "$source" -o "$TMP/fs-aarch64.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding aarch64 flags (zovc exited $status)"
        show_file "$TMP/fs-arch-aarch64.err"
        return
    fi
    if grep -q -- '-mno-red-zone' "$TMP/fs-arch-aarch64.log"; then
        fail "freestanding aarch64 flags (x86 red-zone flag applied)"
        show_file "$TMP/fs-arch-aarch64.log"
        return
    fi

    : >"$TMP/fs-arch-x86.log"
    run_limited "$TMP/fs-arch-x86.out" "$TMP/fs-arch-x86.err" \
        env ZOV_FAKECC_LOG="$TMP/fs-arch-x86.log" \
        ZOV_TARGET_ARCH=x86_64 PATH="$TMP/fakebin:$PATH" \
        "$ZOV" --freestanding "$source" -o "$TMP/fs-x86.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding x86_64 flags (zovc exited $status)"
        show_file "$TMP/fs-arch-x86.err"
        return
    fi
    if ! grep -q -- '-mno-red-zone' "$TMP/fs-arch-x86.log"; then
        fail "freestanding x86_64 flags (red-zone flag missing)"
        show_file "$TMP/fs-arch-x86.log"
        return
    fi
    pass "freestanding arch flags"
}

run_freestanding_package() {
    fsproj=$TMP/fsproj
    rm -rf "$fsproj"
    mkdir -p "$fsproj/src" || {
        fail "freestanding manifest (cannot create project)"
        return
    }
    printf '[package]\nname = "zovos"\nversion = "0.1.0"\nentry = "src/kernel.zov"\nfreestanding = true\n' \
        >"$fsproj/zov.toml"
    printf 'export fn kmain() void = {\n    let mut x: i32 = 1;\n    x = x + 1;\n};\n' \
        >"$fsproj/src/kernel.zov"
    run_limited "$TMP/fsb.out" "$TMP/fsb.err" "$ZOV" build -C "$fsproj"
    status=$?
    if [ "$status" -eq 0 ] && [ -f "$fsproj/target/zovos.o" ]; then
        pass "freestanding manifest object build"
    else
        fail "freestanding manifest object build (exit $status or missing target)"
        show_file "$TMP/fsb.out"
        show_file "$TMP/fsb.err"
    fi
}

run_package() {
    require_dir "$ROOT/tests/pkg_workspace" || return
    require_file "$ROOT/tests/pkg_workspace/app/zov.toml" || return
    ws=$TMP/pkg_workspace
    rm -rf "$ws"
    if ! cp -R "$ROOT/tests/pkg_workspace" "$ws"; then
        fail "package workspace copy failed"
        return
    fi
    wsapp=$ws/app
    rm -rf "$wsapp/target" "$wsapp/zov.lock"

    run_limited "$TMP/ws.out" "$TMP/ws.err" "$ZOV" build -C "$wsapp"
    status=$?
    if [ "$status" -ne 0 ] || [ ! -f "$wsapp/zov.lock" ] \
       || [ ! -f "$wsapp/target/app" ]; then
        fail "pkg build + zov.lock + target/app (exit $status)"
        show_file "$TMP/ws.out"
        show_file "$TMP/ws.err"
    else
        pass "pkg build + zov.lock + target/app"
    fi

    run_limited "$TMP/wsrun.out" "$TMP/wsrun.err" \
        "$ZOV" run -C "$wsapp"
    status=$?
    if [ "$status" -eq 0 ] \
       && grep -q "triple(7)=21" "$TMP/wsrun.out" \
       && grep -q "sqrt(4)=2" "$TMP/wsrun.out"; then
        pass "pkg run output"
    else
        fail "pkg run output (exit $status)"
        show_file "$TMP/wsrun.out"
        show_file "$TMP/wsrun.err"
    fi

    run_limited "$TMP/wstest.out" "$TMP/wstest.err" \
        "$ZOV" test -C "$wsapp"
    status=$?
    if [ "$status" -eq 0 ] && grep -q "2 passed, 0 failed" "$TMP/wstest.out"; then
        pass "pkg test harness"
    else
        fail "pkg test harness (exit $status)"
        show_file "$TMP/wstest.out"
        show_file "$TMP/wstest.err"
    fi
}

run_sanitizer() {
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
    export ASAN_OPTIONS
    : >"$TMP/asan_probe.log"
    asan_cc=
    asan_seen=' '

    # Compiling is not enough: some installations ship the ASan headers but
    # not the runtime library.  Probe both compile and run, and try the other
    # installed C compilers when the default one cannot produce an ASan binary.
    for asan_candidate in "${CC:-}" clang gcc cc; do
        [ -n "$asan_candidate" ] || continue
        if ! command -v "$asan_candidate" >/dev/null 2>&1; then
            continue
        fi
        asan_path=$(command -v "$asan_candidate")
        case $asan_seen in
            *" $asan_path "*) continue ;;
        esac
        asan_seen=$asan_seen$asan_path' '

        rm -f "$TMP/asan_probe"
        printf 'int main(void){return 0;}\n' >"$TMP/asan_probe.c"
        if ! run_limited "$TMP/asan_probe.cc.out" "$TMP/asan_probe.cc.err" \
            "$asan_candidate" -std=gnu11 -fsanitize=address \
            "$TMP/asan_probe.c" -o "$TMP/asan_probe"; then
            {
                printf '  %s: ASan compilation failed\n' "$asan_candidate"
                sed 's/^/    /' "$TMP/asan_probe.cc.err"
            } >>"$TMP/asan_probe.log"
            continue
        fi
        run_limited "$TMP/asan_probe.out" "$TMP/asan_probe.err" \
            "$TMP/asan_probe"
        status=$?
        if [ "$status" -ne 0 ]; then
            {
                printf '  %s: ASan binary exited %s\n' "$asan_candidate" "$status"
                sed 's/^/    /' "$TMP/asan_probe.err"
            } >>"$TMP/asan_probe.log"
            continue
        fi
        asan_cc=$asan_candidate
        break
    done

    if [ -z "$asan_cc" ]; then
        printf '    no installed C compiler could build and run an ASan binary:\n'
        show_file "$TMP/asan_probe.log"
        skip "asan smoke (no working -fsanitize=address toolchain)"
        return
    fi

    run_limited "$TMP/asan.c" "$TMP/asan.emit.err" \
        "$ZOV" --emit-c "$ROOT/tests/01_arith.zov"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "asan smoke (zovc --emit-c exited $status)"
        show_file "$TMP/asan.emit.err"
        return
    fi
    run_limited "$TMP/asan.cc.out" "$TMP/asan.cc.err" \
        "$asan_cc" -std=gnu11 -O1 -g -fsanitize=address \
        "$TMP/asan.c" -o "$TMP/asan_bin"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "asan smoke ($asan_cc C compile exited $status)"
        show_file "$TMP/asan.cc.err"
        return
    fi
    run_limited "$TMP/asan.out" "$TMP/asan.err" \
        sh -c 'cd "$1" && exec "$2" < /dev/null' sh "$TMP" "$TMP/asan_bin"
    status=$?
    if [ "$status" -ne 42 ]; then
        fail "asan smoke (exit $status, want 42)"
        show_file "$TMP/asan.err"
        return
    fi
    if grep -q "ERROR: AddressSanitizer" "$TMP/asan.err"; then
        fail "asan smoke ($asan_cc reported an AddressSanitizer error)"
        show_file "$TMP/asan.err"
        return
    fi
    pass "asan smoke ($asan_cc)"
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

require_exec "$ZOV" || {
    echo "FAIL: build zovc first (make zovc)" >&2
    exit 1
}
require_cmd cc || {
    echo "FAIL: a system C compiler is required for cimport/ASan tests" >&2
    exit 1
}
NEG_EXPECTED=$ROOT/tests/neg/expected.txt
if ! require_file "$NEG_EXPECTED"; then
    echo "FAIL: negative-test diagnostic manifest is missing" >&2
    exit 1
fi

# A stale, duplicated, or malformed manifest entry must not be able to make a
# negative test silently pass or point at a fixture that no longer exists.
if ! awk -F'|' '
    /^[[:space:]]*(#|$)/ { next }
    NF != 5 { printf "    line %d: expected 5 fields, got %d\n", NR, NF; bad = 1 }
    seen[$1]++ { printf "    line %d: duplicate entry for %s\n", NR, $1; bad = 1 }
    END { exit bad }
' "$NEG_EXPECTED"; then
    fail "negative-test diagnostic manifest is malformed"
    exit 1
fi
while IFS= read -r _entry || [ -n "$_entry" ]; do
    case $_entry in
        ''|'#'*) continue ;;
    esac
    _manifest_file=${_entry%%|*}
    if [ ! -f "$ROOT/tests/neg/$_manifest_file" ]; then
        fail "negative-test manifest names missing fixture: $_manifest_file"
    fi
done <"$NEG_EXPECTED"
if [ "$fails" -ne 0 ]; then
    echo "FAIL: fix the negative-test diagnostic manifest before running tests" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Positive execution tests
# ---------------------------------------------------------------------------

run_exit  01_arith tests/01_arith.zov 42
run_stdout 02_defer  tests/02_defer.zov  tests/02_defer.expected
run_stdout 03_ffi    tests/03_ffi.zov    tests/03_ffi.expected
run_stdout 04_union_try tests/04_union_try.zov tests/04_union_try.expected
run_stdout 05_c_nullable_file tests/05_c_nullable_file.zov tests/05_c_nullable_file.expected
run_exit  06_borrow tests/06_borrow_and_moves.zov 0
run_exit  07_generics tests/07_generics.zov 0
run_stdout 08_println tests/08_println_chain.zov tests/08_println_chain.expected
run_exit  09_std_vec tests/09_std_vec_and_methods.zov 0
run_exit  10_std_fs tests/10_std_fs.zov 0
run_stdout 12_enums tests/12_enums_and_slices.zov tests/12_enums_and_slices.expected
run_stdout 13_std_arena tests/13_std_arena_and_str.zov tests/13_std_arena_and_str.expected
run_stdout 14_bitwise_map tests/14_bitwise_and_map.zov tests/14_bitwise_and_map.expected
run_stdout 15_capstone tests/15_capstone_interpreter.zov tests/15_capstone_interpreter.expected
run_stdin 17_std_io tests/17_std_io.zov "first line" tests/17_std_io.expected
run_exit  18_std_time tests/18_std_time.zov 0
run_exit  19_std_math tests/19_std_math_and_separators.zov 0
run_stdout 20_string_ownership tests/20_string_ownership.zov tests/20_string_ownership.expected
run_stdout 22_strict_c99 tests/22_strict_c99_constructs.zov tests/22_strict_c99_constructs.expected
run_stdout 23_eval_order tests/23_eval_order.zov tests/23_eval_order.expected
run_stdout 24_borrow_scopes tests/24_borrow_scopes.zov tests/24_borrow_scopes.expected
run_stdout 25_owned_values tests/25_owned_values.zov tests/25_owned_values.expected
run_stdout 26_nullable_wildcard tests/26_nullable_wildcard.zov tests/26_nullable_wildcard.expected
run_stdout 27_return_defer_order tests/27_return_defer_order.zov tests/27_return_defer_order.expected
run_stdout 28_return_paths tests/28_return_paths.zov tests/28_return_paths.expected
run_stdout 29_generic_loops tests/29_generic_loops.zov tests/29_generic_loops.expected
run_stdout 30_slice_index_once tests/30_slice_index_once.zov tests/30_slice_index_once.expected
run_stdout 31_int_ranges tests/31_int_ranges.zov tests/31_int_ranges.expected
run_stdout 32_nullable_null tests/32_nullable_null.zov tests/32_nullable_null.expected
run_stdout 33_borrow_view_rebind tests/33_borrow_view_rebind.zov tests/33_borrow_view_rebind.expected
run_stdout 34_owned_nested tests/34_owned_nested.zov tests/34_owned_nested.expected
run_stdout 35_union_payload tests/35_union_payload.zov tests/35_union_payload.expected
run_stdout 36_union_fields tests/36_union_fields.zov tests/36_union_fields.expected
run_stdout 37_type_order tests/37_type_order.zov tests/37_type_order.expected
run_panic 38_div_by_zero tests/38_div_by_zero.zov E1003 "integer division by zero"
run_panic 39_bounds_panic tests/39_bounds_panic.zov E1001 "index 5 is outside 0..2"
run_panic 44_slice_range_panic tests/44_slice_range_panic.zov E1002 "slice range 1..9 is outside 0..4"
run_panic 40_assert_panic tests/40_assert_panic.zov E1006 "assert failed:"
run_panic 41_unwrap_null_panic tests/41_unwrap_null_panic.zov E1004 "unwrap of null"
run_panic_release 38_div_by_zero tests/38_div_by_zero.zov E1003 "integer division by zero"
run_panic_release 39_bounds_panic tests/39_bounds_panic.zov E1001 "index 5 is outside 0..2"
run_panic_release 44_slice_range_panic tests/44_slice_range_panic.zov E1002 "slice range 1..9 is outside 0..4"
LONG_LINE=$(printf '%0300d' 0 | tr '0' 'x')
run_stdin 21_long_stdin tests/21_long_stdin.zov "$LONG_LINE" tests/21_long_stdin.expected

# Test-block runner.  Run from TMP so its temporary binary stays outside the
# repository.
run_limited "$TMP/11.out" "$TMP/11.err" \
    sh -c 'cd "$1" && exec "$2" test "$3"' \
    sh "$TMP" "$ZOV" "$ROOT/tests/11_test_blocks.zov"
status=$?
if [ "$status" -ne 0 ]; then
    fail "11_test_blocks (zovc test exited $status)"
    show_file "$TMP/11.err"
elif cmp -s "$TMP/11.out" "$ROOT/tests/11_test_blocks.expected"; then
    pass "11_test_blocks"
else
    fail "11_test_blocks (output mismatch)"
    diff "$ROOT/tests/11_test_blocks.expected" "$TMP/11.out" | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# Package flow and negative diagnostics
# ---------------------------------------------------------------------------

run_package

for neg in "$ROOT"/tests/neg/*.zov; do
    [ -e "$neg" ] || continue
    run_neg "$neg"
done

# ---------------------------------------------------------------------------
# Frontend, language server, C driver, freestanding, sanitizers
# ---------------------------------------------------------------------------

run_check
run_import_diagnostic
run_diagnostic_registry
run_lsp
run_zovc_cc
run_freestanding
run_freestanding_link
run_strict_c99
run_freestanding_arch
run_freestanding_package
run_sanitizer

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

if [ "$fails" -ne 0 ]; then
    printf '%s test(s) failed (%s passed, %s skipped); artifacts: %s\n' \
        "$fails" "$passed" "$skipped" "$TMP"
    exit 1
fi

if [ "$skipped" -ne 0 ]; then
    printf 'all executed tests passed (%s skipped); artifacts: %s\n' \
        "$skipped" "$TMP"
else
    printf 'all tests passed; artifacts: %s\n' "$TMP"
fi
exit 0
