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
SKLAD=$ROOT/sklad
ZOV_STD_PATH=${ZOV_STD_PATH:-$ROOT/lib/std}
export ZOV_STD_PATH
ZOVC=$ZOV
export ZOVC
PATH="$ROOT:$PATH"
export PATH

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
    if [ "$(uname -s)" = "Darwin" ]; then
        sed 's|/private/tmp|/tmp|g' "$TMP/$name.out" > "$TMP/$name.out.tmp" && mv "$TMP/$name.out.tmp" "$TMP/$name.out"
    fi
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
    if ! printf '%b\n' "$input" >"$TMP/$name.stdin"; then
        fail "$name (cannot create stdin fixture)"
        return
    fi
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" "$TMP/$name.stdin"
    status=$?
    check_program_status "$name" "$status" 0 || return
    if [ "$(uname -s)" = "Darwin" ]; then
        sed 's|/private/tmp|/tmp|g' "$TMP/$name.out" > "$TMP/$name.out.tmp" && mv "$TMP/$name.out.tmp" "$TMP/$name.out"
    fi
    if ! cmp -s "$TMP/$name.out" "$expected"; then
        fail "$name (stdout mismatch)"
        diff "$expected" "$TMP/$name.out" | sed 's/^/    /'
        show_file "$TMP/$name.err"
        return
    fi
    pass "$name"
}

run_stdio() { # name source expected_stdout expected_stderr
    name=$1
    source=$2
    expected_out=$3
    expected_err=$4
    require_file "$expected_out" || return
    require_file "$expected_err" || return
    output=$TMP/$name
    compile_zov "$name" "$source" "$output" || return
    run_program "$name" "$output" /dev/null
    status=$?
    check_program_status "$name" "$status" 0 || return
    if ! cmp -s "$TMP/$name.out" "$expected_out"; then
        fail "$name (stdout mismatch)"
        diff "$expected_out" "$TMP/$name.out" | sed 's/^/    /'
        return
    fi
    if ! cmp -s "$TMP/$name.err" "$expected_err"; then
        fail "$name (stderr mismatch)"
        diff "$expected_err" "$TMP/$name.err" | sed 's/^/    /'
        return
    fi
    pass "$name"
}

check_panic_report() { # name code message log [file:line:col]
    name=$1
    code=$2
    message=$3
    log=$4
    site=$5
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
    if [ -n "$site" ] && ! grep -qF "  --> $site" "$log"; then
        fail "$name (runtime panic reported the wrong source location, want $site)"
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

run_panic() { # name source expected_code expected_stderr_text [line:col]
    name=$1
    source=$2
    code=$3
    expected=$4
    site=${5:-}
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
    check_panic_report "$name" "$code" "$expected" "$TMP/$name.err" \
        "${site:+$source:$site}" || return
    pass "$name"
}

run_panic_release() { # name source expected_code expected_stderr_text [line:col]
    label=$1
    source=$2
    code=$3
    expected=$4
    site=${5:-}
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
        "$TMP/$label-release.err" "${site:+$source:$site}" || return
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
        sh -c 'PATH="$4:$PATH" && cd "$1" && exec python3 "$2" "$3" "$5" "$6"' \
        sh "$TMP" "$ROOT/tests/lsp_protocol.py" "$TMP" "$ROOT" \
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

    # pure std::math and std::str stay available without a hosted libc
    printf 'use std::math as m;\nexport fn kmain() i32 = {\n    let r: f64 = m::sqrt(9.0f64);\n    return m::min(1, 2) + m::popcnt(7u32) + m::nextpow2(9u32) as i32 + r as i32;\n};\n' \
        >"$TMP/fs_math_pure.zov"
    printf 'use std::str as strlib;\nexport fn kmain() i32 = {\n    let t: str = strlib::trim(" hi ");\n    if (!strlib::startswith(t, "h")) { return 1; };\n    let r = strlib::findbyte(t, 105u8);\n    let n: usize = match (r) {\n        case let i: usize => i,\n        case strlib::NotFound => 9usize,\n    };\n    return n as i32;\n};\n' \
        >"$TMP/fs_str_pure.zov"
    for fs_case in fs_math_pure fs_str_pure; do
        run_limited "$TMP/$fs_case.c" "$TMP/$fs_case.err" \
            "$ZOV" --freestanding --emit-c "$TMP/$fs_case.zov"
        fs_status=$?
        if [ "$fs_status" -ne 0 ] \
           || grep -qE 'math\.h|stdlib\.h|stdio\.h' "$TMP/$fs_case.c" \
           || ! grep -q "zov_kmain" "$TMP/$fs_case.c"; then
            fail "freestanding $fs_case emission (status $fs_status)"
            show_file "$TMP/$fs_case.err"
            return
        fi
        if ! run_limited "$TMP/$fs_case.cc.out" "$TMP/$fs_case.cc.err" \
            cc -ffreestanding -nostdlib -mno-red-zone -fno-pie -std=c99 \
            -pedantic-errors -Wall -Wextra -Werror -c "$TMP/$fs_case.c" \
            -o "$TMP/$fs_case.o"; then
            fail "freestanding $fs_case object compile"
            show_file "$TMP/$fs_case.cc.err"
            return
        fi
        # memcmp is the documented kernel-provided primitive; zov_panic is
        # the platform hook.  Anything else is an unexpected hosted symbol.
        fs_unexpected=$(nm -u "$TMP/$fs_case.o" \
            | grep -vE 'zov_panic$|memcmp$' || true)
        if [ -n "$fs_unexpected" ]; then
            fail "freestanding $fs_case has hosted symbol references"
            printf '%s\n' "$fs_unexpected" | sed 's/^/    /'
            return
        fi
    done
    pass "freestanding pure math and str"

    # hosted-only standard modules must be rejected at the user's call site
    fs_reject_ok=0
    printf 'use std::fs as fs;\nexport fn kmain() void = {\n    let r = fs::readstr("x");\n};\n' \
        >"$TMP/fs_fs.zov"
    printf 'use std::env as env;\nexport fn kmain() void = {\n    let n = env::argscount();\n};\n' \
        >"$TMP/fs_env.zov"
    printf 'use std::time as time;\nexport fn kmain() void = {\n    time::sleepms(1u32);\n};\n' \
        >"$TMP/fs_time.zov"
    printf 'use std::vec as vec;\nexport fn kmain() void = {\n    let v = vec::new[u8]();\n};\n' \
        >"$TMP/fs_vec.zov"
    printf 'use std::math as math;\nexport fn kmain() void = {\n    let r = math::pow(2.0f64, 3.0f64);\n};\n' \
        >"$TMP/fs_math.zov"
    printf 'use std::strsplit as strlib;\nexport fn kmain() void = {\n    let p = strlib::splitbyte("a,b", 44u8);\n    p.free();\n};\n' \
        >"$TMP/fs_str.zov"
    printf 'use std::proc as proc;\nexport fn kmain() void = {\n    let a: [1]str = [""];\n    let r = proc::run("/bin/true", a[0..0]);\n};\n' \
        >"$TMP/fs_proc.zov"
    for fs_case in fs_fs fs_env fs_time fs_vec fs_math fs_str fs_proc; do
        run_limited "$TMP/$fs_case.c" "$TMP/$fs_case.err" \
            "$ZOV" --freestanding --check "$TMP/$fs_case.zov"
        fs_status=$?
        if [ "$fs_status" -ne 1 ] \
           || ! grep -q "E0306" "$TMP/$fs_case.err" \
           || ! grep -q "$TMP/$fs_case.zov" "$TMP/$fs_case.err"; then
            fail "freestanding $fs_case diagnostic (status $fs_status)"
            show_file "$TMP/$fs_case.err"
            return
        fi
        fs_reject_ok=$((fs_reject_ok + 1))
    done
    pass "freestanding hosted-module diagnostics ($fs_reject_ok modules)"
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

# The minimal runtime defines no allocator symbols: linking generated
# code against it must still succeed, so those hooks stay optional.
run_freestanding_alloc_hook() {
    require_cmd cc || return
    source=$ROOT/tests/16_freestanding_alloc_hook.zov
    require_file "$source" || return
    cat >"$TMP/rt_hook.c" <<'EOF'
#include "zov_rt.h"
#include <stdio.h>
#include <stdlib.h>

static int frees;

void zov_panic(const char *code, const char *msg, const char *file,
               int line, int col)
{
    fprintf(stderr, "panic %s %s at %s:%d:%d\n", code, msg, file, line, col);
    exit(101);
}

void *zov_rt_realloc(void *ptr, size_t size)
{
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}

void zov_rt_free(void *ptr)
{
    if (ptr) frees++;
}

int main(void)
{
    zov_kmain();
    if (frees != 1) {
        fprintf(stderr, "zov_rt_free calls: %d\n", frees);
        return 3;
    }
    printf("hook ok\n");
    return 0;
}
EOF
    run_limited "$TMP/hook.c" "$TMP/hook.emit.err" \
        "$ZOV" --freestanding --emit-c "$source"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding alloc hook (emit exited $status)"
        show_file "$TMP/hook.emit.err"
        return
    fi
    if ! cc -std=c99 -pedantic-errors -Wall -Wextra -Werror -ffreestanding \
        -I"$ROOT/runtime" "$TMP/hook.c" "$TMP/rt_hook.c" \
        -o "$TMP/hook.bin" >"$TMP/hook.cc.out" 2>"$TMP/hook.cc.err"; then
        fail "freestanding alloc hook (build failed)"
        show_file "$TMP/hook.cc.err"
        return
    fi
    run_program "freestanding_alloc_hook" "$TMP/hook.bin" /dev/null
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding alloc hook (run exited $status)"
        show_file "$TMP/freestanding_alloc_hook.err"
        return
    fi
    if ! grep -q "hook ok" "$TMP/freestanding_alloc_hook.out"; then
        fail "freestanding alloc hook (zov_rt_free was not called)"
        show_file "$TMP/freestanding_alloc_hook.out"
        return
    fi
    pass "freestanding allocator hook"
}

run_freestanding_minimal_runtime() {
    require_cmd cc || return
    require_file "$ROOT/runtime/example/minimal_hosted_runtime.c" || return

    run_limited "$TMP/rt_min.c" "$TMP/rt_min.emit.err" \
        "$ZOV" --freestanding --emit-c \
        "$ROOT/tests/16_freestanding_smoke.zov"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding minimal runtime (emit exited $status)"
        show_file "$TMP/rt_min.emit.err"
        return
    fi
    run_limited "$TMP/rt_min.cc.out" "$TMP/rt_min.cc.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        -ffreestanding -c "$TMP/rt_min.c" -o "$TMP/rt_min.o"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding minimal runtime (object compile exited $status)"
        show_file "$TMP/rt_min.cc.err"
        return
    fi
    run_limited "$TMP/rt_min.link.out" "$TMP/rt_min.link.err" \
        cc -std=c99 -pedantic-errors -Wall -Wextra -Werror \
        "$ROOT/runtime/example/minimal_hosted_runtime.c" "$TMP/rt_min.o" \
        -o "$TMP/rt_min"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding minimal runtime (link exited $status)"
        show_file "$TMP/rt_min.link.err"
        return
    fi
    run_limited "$TMP/rt_min.out" "$TMP/rt_min.err" \
        sh -c 'exec "$1" < /dev/null' sh "$TMP/rt_min"
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "freestanding minimal runtime (program exited $status)"
        show_file "$TMP/rt_min.err"
        return
    fi
    pass "freestanding minimal runtime link"
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
            -Wno-parentheses-equality -Wno-switch-bool \
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
            -Wno-parentheses-equality -Wno-switch-bool \
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
            -Wno-parentheses-equality -Wno-switch-bool \
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

# Opt-in full sweep: ZOV_SANITIZE=1 runs every executable fixture under
# ASan+UBSan and fails on any sanitizer report.  The default suite keeps
# only the smoke test above so an ordinary run stays fast.
run_sanitizer_sweep() {
    if [ "${ZOV_SANITIZE:-0}" != "1" ]; then
        return 0
    fi
    sweep_cc=
    for sweep_candidate in clang gcc cc; do
        command -v "$sweep_candidate" >/dev/null 2>&1 || continue
        printf 'int main(void){return 0;}\n' >"$TMP/sweep_probe.c"
        if "$sweep_candidate" -std=c99 -fsanitize=address,undefined \
            "$TMP/sweep_probe.c" -o "$TMP/sweep_probe" >/dev/null 2>&1; then
            sweep_cc=$sweep_candidate
            break
        fi
    done
    if [ -z "$sweep_cc" ]; then
        fail "sanitizer sweep (no toolchain with -fsanitize=address,undefined)"
        return
    fi

    sweep_ok=0
    sweep_bad=0
    sweep_skip=0
    for sweep_src in "$ROOT"/tests/[0-9][0-9]_*.zov; do
        [ -f "$sweep_src" ] || continue
        sweep_base=${sweep_src##*/}
        sweep_base=${sweep_base%.zov}
        case $sweep_base in
            16_*) sweep_skip=$((sweep_skip + 1)); continue ;;
        esac
        sweep_c=$TMP/san-$sweep_base.c
        if [ "$sweep_base" = "11_test_blocks" ]; then
            run_limited "$sweep_c" "$TMP/san-$sweep_base.emit.err" \
                "$ZOV" --test --emit-c "$sweep_src"
        else
            run_limited "$sweep_c" "$TMP/san-$sweep_base.emit.err" \
                "$ZOV" --emit-c "$sweep_src"
        fi
        if [ $? -ne 0 ]; then
            fail "sanitizer sweep: $sweep_base (emit failed)"
            show_file "$TMP/san-$sweep_base.emit.err"
            sweep_bad=$((sweep_bad + 1))
            continue
        fi
        if ! "$sweep_cc" -std=c99 -O1 -g -fsanitize=address,undefined \
            -fno-omit-frame-pointer "$sweep_c" \
            -o "$TMP/san-$sweep_base" >"$TMP/san-$sweep_base.cc.out" \
            2>"$TMP/san-$sweep_base.cc.err"; then
            fail "sanitizer sweep: $sweep_base (sanitized build failed)"
            show_file "$TMP/san-$sweep_base.cc.err"
            sweep_bad=$((sweep_bad + 1))
            continue
        fi
        sweep_dir=$TMP/san-run-$sweep_base
        rm -rf "$sweep_dir"
        mkdir -p "$sweep_dir"
        sweep_stdin=
        sweep_has_stdin=0
        case $sweep_base in
            17_std_io) sweep_stdin="first line"; sweep_has_stdin=1 ;;
            51_shell_workload)
                sweep_stdin="echo hello world\necho again\nexit"
                sweep_has_stdin=1 ;;
            53_zovsh_overwrite)
                sweep_stdin='echo hello world\nexport FOO=bar\necho $FOO\necho $NOPE\ncd /tmp\npwd\necho done\nexit'
                sweep_has_stdin=1 ;;
            56_zovsh_proc_fs)
                sweep_stdin='echo $GREETING\necho $NOPE\nruntrue\nexit'
                sweep_has_stdin=1 ;;
            21_long_stdin) sweep_stdin="$LONG_LINE"; sweep_has_stdin=1 ;;
        esac
        if [ "$sweep_has_stdin" -eq 1 ]; then
            printf '%b\n' "$sweep_stdin" >"$sweep_dir/stdin"
            sweep_input=$sweep_dir/stdin
        else
            sweep_input=/dev/null
        fi
        run_limited "$sweep_dir/stdout" "$sweep_dir/stderr" \
            sh -c 'cd "$1" && exec "$2" < "$3"' sh "$sweep_dir" \
            "$TMP/san-$sweep_base" "$sweep_input"
        sweep_status=$?
        if [ "$sweep_status" -ge 128 ]; then
            fail "sanitizer sweep: $sweep_base (terminated by signal)"
            show_file "$sweep_dir/stderr"
            sweep_bad=$((sweep_bad + 1))
            continue
        fi
        if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' \
            "$sweep_dir/stderr"; then
            fail "sanitizer sweep: $sweep_base (sanitizer report)"
            show_file "$sweep_dir/stderr"
            sweep_bad=$((sweep_bad + 1))
            continue
        fi
        if [ "$sweep_has_stdin" -eq 1 ]; then
            if [ "$sweep_status" -ne 0 ]; then
                fail "sanitizer sweep: $sweep_base (exited $sweep_status)"
                show_file "$sweep_dir/stderr"
                sweep_bad=$((sweep_bad + 1))
                continue
            fi
            if [ "$(uname -s)" = "Darwin" ] && [ -f "$sweep_dir/stdout" ]; then
                sed 's|/private/tmp|/tmp|g' "$sweep_dir/stdout" > "$sweep_dir/stdout.tmp" && mv "$sweep_dir/stdout.tmp" "$sweep_dir/stdout"
            fi
            if [ -f "$ROOT/tests/$sweep_base.expected" ] \
                && ! cmp -s "$sweep_dir/stdout" \
                    "$ROOT/tests/$sweep_base.expected"; then
                fail "sanitizer sweep: $sweep_base (stdout mismatch)"
                diff "$ROOT/tests/$sweep_base.expected" \
                    "$sweep_dir/stdout" | sed 's/^/    /'
                sweep_bad=$((sweep_bad + 1))
                continue
            fi
        fi
        sweep_ok=$((sweep_ok + 1))
    done
    if [ "$sweep_bad" -ne 0 ]; then
        return
    fi
    pass "sanitizer sweep ($sweep_cc, $sweep_ok fixtures, $sweep_skip skipped)"
}

run_zovc_offline() {
    # 1. zovc init in an empty directory
    offdir=$TMP/offline_app
    rm -rf "$offdir"
    mkdir -p "$offdir"
    run_limited "$TMP/init.out" "$TMP/init.err" "$ZOV" init -C "$offdir"
    if [ $? -eq 0 ] && [ -f "$offdir/zov.toml" ] && [ -f "$offdir/src/main.zov" ]; then
        if grep -q 'edition = "2026"' "$offdir/zov.toml" && grep -q 'zovc = "0.11.0"' "$offdir/zov.toml"; then
            pass "zovc init offline manifest"
        else
            fail "zovc init generated manifest missing expected metadata"
        fi
    else
        fail "zovc init failed"
        show_file "$TMP/init.err"
    fi

    # 2. zovc check, build, run in offdir
    run_limited "$TMP/off_check.out" "$TMP/off_check.err" "$ZOV" check -C "$offdir"
    if [ $? -eq 0 ]; then
        pass "zovc check offline project"
    else
        fail "zovc check offline project"
        show_file "$TMP/off_check.err"
    fi

    run_limited "$TMP/off_build.out" "$TMP/off_build.err" "$ZOV" build -C "$offdir"
    if [ $? -eq 0 ] && [ -x "$offdir/target/offline_app" ]; then
        pass "zovc build offline project"
    else
        fail "zovc build offline project"
        show_file "$TMP/off_build.err"
    fi

    run_limited "$TMP/off_run.out" "$TMP/off_run.err" "$ZOV" run -C "$offdir"
    if [ $? -eq 0 ] && grep -iq "hello from offline_app" "$TMP/off_run.out"; then
        pass "zovc run offline project"
    else
        fail "zovc run offline project"
        show_file "$TMP/off_run.err"
    fi

    # 3. Piped stdin pass-through with io::readline
    pipe_src=$TMP/pipe_stdin.zov
    cat >"$pipe_src" <<'EOF'
use std::io;
export fn main() i32 = {
    let r = io::readline();
    let line: String = match (r) {
        case let s: String => s,
        case Eof => String::new(),
        case IoError => String::new(),
    };
    io::println("received:" + line.view());
    line.free();
    return 0;
};
EOF
    run_limited "$TMP/pipe.out" "$TMP/pipe.err" \
        sh -c 'printf "zov_stream_ok\n" | "$1" run "$2"' \
        sh "$ZOV" "$pipe_src"
    if [ $? -eq 0 ] && grep -q "received:zov_stream_ok" "$TMP/pipe.out"; then
        pass "zovc run with piped stdin"
    else
        fail "zovc run with piped stdin"
        show_file "$TMP/pipe.err"
    fi

    # 4. Edition validation: 2026 vs invalid edition
    run_limited "$TMP/ed2026.out" "$TMP/ed2026.err" \
        "$ZOV" check --edition 2026 "$pipe_src"
    if [ $? -eq 0 ]; then
        pass "zovc --edition 2026 accepted"
    else
        fail "zovc --edition 2026 rejected"
        show_file "$TMP/ed2026.err"
    fi

    run_limited "$TMP/ed1999.out" "$TMP/ed1999.err" \
        "$ZOV" check --edition 1999 "$pipe_src"
    if [ $? -ne 0 ] && grep -q "E0903" "$TMP/ed1999.err"; then
        pass "zovc --edition 1999 rejected with E0903"
    else
        fail "zovc --edition 1999 did not report E0903"
        show_file "$TMP/ed1999.err"
    fi

    # 5. Local --pkg resolution for file and directory
    pkg_file=$TMP/mymath.zov
    cat >"$pkg_file" <<'EOF'
export fn add_nums(a: i32, b: i32) i32 = { return a + b; };
EOF
    pkg_dir=$TMP/pkg_dir_mod
    mkdir -p "$pkg_dir/src"
    cat >"$pkg_dir/src/lib.zov" <<'EOF'
export fn mul_nums(a: i32, b: i32) i32 = { return a * b; };
EOF
    main_pkg=$TMP/main_pkg_test.zov
    cat >"$main_pkg" <<'EOF'
use mymath;
use dirmod;
use std::io;
export fn main() i32 = {
    io::println("add=" + mymath::add_nums(10, 20));
    io::println("mul=" + dirmod::mul_nums(6, 7));
    return 0;
};
EOF
    run_limited "$TMP/pkgrun.out" "$TMP/pkgrun.err" \
        "$ZOV" run --pkg mymath="$pkg_file" --pkg dirmod="$pkg_dir" "$main_pkg"
    if [ $? -eq 0 ] && grep -q "add=30" "$TMP/pkgrun.out" && grep -q "mul=42" "$TMP/pkgrun.out"; then
        pass "zovc --pkg file and directory resolution"
    else
        fail "zovc --pkg resolution"
        show_file "$TMP/pkgrun.err"
    fi

    # 6. Negative offline manifest validations
    # 6a. Remote git dep without local mapping fails offline
    net_app=$TMP/net_app
    rm -rf "$net_app"
    mkdir -p "$net_app/src"
    cat >"$net_app/zov.toml" <<'EOF'
[package]
name = "net_app"
version = "0.1.0"
edition = "2026"

[dependencies]
remote_git = { git = "https://example.com/repo.git" }
EOF
    cat >"$net_app/src/main.zov" <<'EOF'
use remote_git;
fn main() void = {};
EOF
    run_limited "$TMP/net.out" "$TMP/net.err" "$ZOV" build -C "$net_app"
    if [ $? -ne 0 ] && grep -q "requires network access. Run 'sklad fetch'" "$TMP/net.err"; then
        pass "zovc offline rejection of unmapped network dependency"
    else
        fail "zovc did not reject unmapped network dependency"
        show_file "$TMP/net.err"
    fi

    # 6b. Future zovc version requirement fails
    future_app=$TMP/future_app
    rm -rf "$future_app"
    mkdir -p "$future_app/src"
    cat >"$future_app/zov.toml" <<'EOF'
[package]
name = "future_app"
version = "0.1.0"
zovc = "99.0.0"
edition = "2026"
EOF
    cat >"$future_app/src/main.zov" <<'EOF'
fn main() void = {};
EOF
    run_limited "$TMP/future.out" "$TMP/future.err" "$ZOV" build -C "$future_app"
    if [ $? -ne 0 ] && grep -q "package requires zovc >= 99.0.0" "$TMP/future.err"; then
        pass "zovc manifest compiler version check enforcement"
    else
        fail "zovc manifest compiler version check"
        show_file "$TMP/future.err"
    fi
}

run_staged_install() {
    staged_prefix=$TMP/staged_root
    rm -rf "$staged_prefix"
    run_limited "$TMP/install.out" "$TMP/install.err" \
        make install PREFIX="$staged_prefix"
    if [ $? -ne 0 ]; then
        fail "staged make install execution"
        show_file "$TMP/install.err"
        return
    fi

    if [ -x "$staged_prefix/bin/zovc" ] \
       && [ -x "$staged_prefix/bin/sklad" ] \
       && [ -x "$staged_prefix/bin/zov-lsp" ] \
       && [ -f "$staged_prefix/lib/std/io.zov" ] \
       && [ -f "$staged_prefix/include/runtime/zov_rt.h" ]; then
        pass "staged make install file layout"
    else
        fail "staged make install missing expected files"
        return
    fi

    iso_dir=$TMP/iso_stage_dir
    rm -rf "$iso_dir"
    mkdir -p "$iso_dir"
    run_limited "$TMP/staged_zovc.out" "$TMP/staged_zovc.err" \
        sh -c 'cd "$1" && exec "$2" --version' sh "$iso_dir" "$staged_prefix/bin/zovc"
    if [ $? -eq 0 ] && grep -q "zovc 0.11.0" "$TMP/staged_zovc.out"; then
        pass "staged zovc binary execution"
    else
        fail "staged zovc binary execution"
        show_file "$TMP/staged_zovc.err"
    fi

    run_limited "$TMP/staged_sklad.out" "$TMP/staged_sklad.err" \
        sh -c 'cd "$1" && exec "$2" version' sh "$iso_dir" "$staged_prefix/bin/sklad"
    if [ $? -eq 0 ] && grep -q "sklad 1.9.0" "$TMP/staged_sklad.out"; then
        pass "staged sklad binary execution"
    else
        fail "staged sklad binary execution"
        show_file "$TMP/staged_sklad.err"
    fi
}

run_sklad_integration() {
    skhome=$TMP/sklad_home
    rm -rf "$skhome"
    mkdir -p "$skhome"
    SKLAD_HOME=$skhome
    export SKLAD_HOME

    # 1. Local Git repository fixture
    git_repo=$TMP/git_fixture
    rm -rf "$git_repo"
    mkdir -p "$git_repo/src"
    cat >"$git_repo/zov.toml" <<'EOF'
[package]
name = "gitdep"
version = "1.0.0"
edition = "2026"
zovc = "0.11.0"
EOF
    cat >"$git_repo/src/lib.zov" <<'EOF'
export fn git_answer() i32 = {
    return 42;
};
EOF
    (
        cd "$git_repo" && \
        git init -q && \
        git config user.name "Zov Tester" && \
        git config user.email "test@zov.lang" && \
        git add . && \
        git commit -q -m "Initial gitdep"
    )
    if [ $? -ne 0 ]; then
        fail "git fixture initialization"
        return
    fi

    # 2. Test sklad add + fetch + run with Git dependency
    sklad_git_app=$TMP/sklad_git_app
    rm -rf "$sklad_git_app"
    mkdir -p "$sklad_git_app"
    run_limited "$TMP/sk_init.out" "$TMP/sk_init.err" \
        sh -c 'cd "$1" && exec "$2" init' sh "$sklad_git_app" "$SKLAD"
    if [ $? -ne 0 ] || [ ! -f "$sklad_git_app/zov.toml" ]; then
        fail "sklad init"
        show_file "$TMP/sk_init.err"
        return
    fi

    cat >"$sklad_git_app/src/main.zov" <<'EOF'
use gitdep;
use std::io;
export fn main() i32 = {
    io::println("git_ans=" + gitdep::git_answer());
    return 0;
};
EOF

    run_limited "$TMP/sk_add.out" "$TMP/sk_add.err" \
        sh -c 'cd "$1" && exec "$2" add gitdep --git "file://'"$git_repo"'"' sh "$sklad_git_app" "$SKLAD"
    if [ $? -ne 0 ]; then
        fail "sklad add gitdep"
        show_file "$TMP/sk_add.err"
        return
    fi

    run_limited "$TMP/sk_fetch.out" "$TMP/sk_fetch.err" \
        sh -c 'cd "$1" && exec "$2" fetch' sh "$sklad_git_app" "$SKLAD"
    if [ $? -eq 0 ] && [ -f "$sklad_git_app/sklad.lock" ] && grep -q 'name = "gitdep"' "$sklad_git_app/sklad.lock"; then
        pass "sklad fetch git dependency & lock generation"
    else
        fail "sklad fetch git dependency"
        show_file "$TMP/sk_fetch.err"
    fi

    run_limited "$TMP/sk_run.out" "$TMP/sk_run.err" \
        sh -c 'cd "$1" && exec "$2" run' sh "$sklad_git_app" "$SKLAD"
    if [ $? -eq 0 ] && grep -q "git_ans=42" "$TMP/sk_run.out"; then
        pass "sklad run with git dependency"
    else
        fail "sklad run with git dependency"
        show_file "$TMP/sk_run.err"
    fi

    # 3. sklad tree
    run_limited "$TMP/sk_tree.out" "$TMP/sk_tree.err" \
        sh -c 'cd "$1" && exec "$2" tree' sh "$sklad_git_app" "$SKLAD"
    if [ $? -eq 0 ] && grep -q "gitdep" "$TMP/sk_tree.out"; then
        pass "sklad tree output"
    else
        fail "sklad tree output"
        show_file "$TMP/sk_tree.err"
    fi

    # 4. sklad build --offline uses cached dependency
    run_limited "$TMP/sk_off.out" "$TMP/sk_off.err" \
        sh -c 'cd "$1" && exec "$2" build --offline' sh "$sklad_git_app" "$SKLAD"
    if [ $? -eq 0 ]; then
        pass "sklad build --offline cached"
    else
        fail "sklad build --offline"
        show_file "$TMP/sk_off.err"
    fi

    # 5. URL tarball dependency fixture
    tar_pkg=$TMP/tar_fixture
    rm -rf "$tar_pkg"
    mkdir -p "$tar_pkg/src"
    cat >"$tar_pkg/zov.toml" <<'EOF'
[package]
name = "tardep"
version = "2.0.0"
edition = "2026"
zovc = "0.11.0"
EOF
    cat >"$tar_pkg/src/lib.zov" <<'EOF'
export fn tar_val() i32 = {
    return 777;
};
EOF
    tar_file=$TMP/tardep-2.0.0.tar.gz
    (cd "$tar_pkg" && tar -czf "$tar_file" .)
    tar_sha=$(sha256sum "$tar_file" | cut -c 1-64)

    sklad_tar_app=$TMP/sklad_tar_app
    rm -rf "$sklad_tar_app"
    mkdir -p "$sklad_tar_app"
    run_limited "$TMP/sk_tar_init.out" "$TMP/sk_tar_init.err" \
        sh -c 'cd "$1" && exec "$2" init' sh "$sklad_tar_app" "$SKLAD"
    cat >"$sklad_tar_app/src/main.zov" <<'EOF'
use tardep;
use std::io;
export fn main() i32 = {
    io::println("tar_ans=" + tardep::tar_val());
    return 0;
};
EOF
    run_limited "$TMP/sk_tar_add.out" "$TMP/sk_tar_add.err" \
        sh -c 'cd "$1" && exec "$2" add tardep --url "file://'"$tar_file"'" --sha256 '"$tar_sha"'' sh "$sklad_tar_app" "$SKLAD"
    run_limited "$TMP/sk_tar_run.out" "$TMP/sk_tar_run.err" \
        sh -c 'cd "$1" && exec "$2" run' sh "$sklad_tar_app" "$SKLAD"
    if [ $? -eq 0 ] && grep -q "tar_ans=777" "$TMP/sk_tar_run.out"; then
        pass "sklad run with verified URL tarball dependency"
    else
        fail "sklad run with verified URL tarball dependency"
        show_file "$TMP/sk_tar_run.err"
    fi

    # 6. Checksum tamper test: mismatched SHA256 must be rejected
    sklad_tamper_app=$TMP/sklad_tamper_app
    rm -rf "$sklad_tamper_app"
    mkdir -p "$sklad_tamper_app"
    run_limited "$TMP/sk_tamp_init.out" "$TMP/sk_tamp_init.err" \
        sh -c 'cd "$1" && exec "$2" init' sh "$sklad_tamper_app" "$SKLAD"
    bad_sha="0000000000000000000000000000000000000000000000000000000000000000"
    run_limited "$TMP/sk_tamp_add.out" "$TMP/sk_tamp_add.err" \
        sh -c 'cd "$1" && exec "$2" add tardep --url "file://'"$tar_file"'" --sha256 '"$bad_sha"'' sh "$sklad_tamper_app" "$SKLAD"
    run_limited "$TMP/sk_tamp_fetch.out" "$TMP/sk_tamp_fetch.err" \
        sh -c 'cd "$1" && exec "$2" fetch' sh "$sklad_tamper_app" "$SKLAD"
    if [ $? -ne 0 ] && grep -q "checksum mismatch" "$TMP/sk_tamp_fetch.out"; then
        pass "sklad rejects tampered archive checksum"
    else
        fail "sklad did not reject tampered archive checksum"
        show_file "$TMP/sk_tamp_fetch.out"
        show_file "$TMP/sk_tamp_fetch.err"
    fi
}

run_packaging_and_installer() {
    # 1. Run make dist
    run_limited "$TMP/dist.out" "$TMP/dist.err" make dist
    if [ $? -ne 0 ]; then
        fail "make dist execution"
        show_file "$TMP/dist.err"
        return
    fi
    pass "make dist generates release archives"

    # 2. Test install.sh --zovc-only
    zovc_prefix=$TMP/inst_zovc_only
    rm -rf "$zovc_prefix"
    run_limited "$TMP/inst_zovc.out" "$TMP/inst_zovc.err" \
        ./install.sh --zovc-only --from-dir dist --prefix "$zovc_prefix"
    if [ $? -eq 0 ] \
       && [ -x "$zovc_prefix/bin/zovc" ] \
       && [ -x "$zovc_prefix/bin/zov-lsp" ] \
       && [ -f "$zovc_prefix/lib/std/io.zov" ] \
       && [ ! -e "$zovc_prefix/bin/sklad" ]; then
        pass "install.sh --zovc-only layout"
    else
        fail "install.sh --zovc-only layout"
        show_file "$TMP/inst_zovc.err"
    fi

    # Verify standalone execution of zovc from isolated directory
    iso_dir=$TMP/iso_zovc_only
    rm -rf "$iso_dir"
    mkdir -p "$iso_dir"
    run_limited "$TMP/zovc_only_run.out" "$TMP/zovc_only_run.err" \
        sh -c 'cd "$1" && exec "$2" --version' sh "$iso_dir" "$zovc_prefix/bin/zovc"
    if [ $? -eq 0 ] && grep -q "zovc 0.11.0" "$TMP/zovc_only_run.out"; then
        pass "install.sh --zovc-only binary execution"
    else
        fail "install.sh --zovc-only binary execution"
        show_file "$TMP/zovc_only_run.err"
    fi

    # 3. Test install.sh --sklad-only
    sklad_prefix=$TMP/inst_sklad_only
    rm -rf "$sklad_prefix"
    run_limited "$TMP/inst_sklad.out" "$TMP/inst_sklad.err" \
        ./install.sh --sklad-only --from-dir dist --prefix "$sklad_prefix"
    if [ $? -eq 0 ] \
       && [ -x "$sklad_prefix/bin/sklad" ] \
       && [ ! -e "$sklad_prefix/bin/zovc" ] \
       && [ ! -e "$sklad_prefix/lib/std" ]; then
        pass "install.sh --sklad-only layout"
    else
        fail "install.sh --sklad-only layout"
        show_file "$TMP/inst_sklad.err"
    fi

    # 4. Test install.sh default (both zovc and sklad)
    both_prefix=$TMP/inst_both
    rm -rf "$both_prefix"
    run_limited "$TMP/inst_both.out" "$TMP/inst_both.err" \
        ./install.sh --from-dir dist --prefix "$both_prefix"
    if [ $? -eq 0 ] \
       && [ -x "$both_prefix/bin/zovc" ] \
       && [ -x "$both_prefix/bin/sklad" ] \
       && [ -f "$both_prefix/lib/std/io.zov" ]; then
        pass "install.sh both packages layout"
    else
        fail "install.sh both packages layout"
        show_file "$TMP/inst_both.err"
    fi

    # Test that zovc and sklad work together from both_prefix
    iso_both=$TMP/iso_both_dir
    rm -rf "$iso_both"
    mkdir -p "$iso_both"
    run_limited "$TMP/both_run.out" "$TMP/both_run.err" \
        sh -c 'cd "$1" && exec "$2" version' sh "$iso_both" "$both_prefix/bin/sklad"
    if [ $? -eq 0 ] && grep -q "sklad 1.9.0" "$TMP/both_run.out" && grep -q "zovc 0.11.0" "$TMP/both_run.out"; then
        pass "install.sh both packages execution together"
    else
        fail "install.sh both packages execution together"
        show_file "$TMP/both_run.err"
    fi
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

require_exec "$ZOV" || {
    echo "FAIL: build zovc first (make zovc)" >&2
    exit 1
}
require_exec "$SKLAD" || {
    echo "FAIL: build sklad first (make sklad)" >&2
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
run_stdout 52_owned_overwrite tests/52_owned_overwrite.zov tests/52_owned_overwrite.expected
run_stdout 54_branch_ownership tests/54_branch_ownership.zov tests/54_branch_ownership.expected
run_stdout 55_map_string_keys tests/55_map_string_keys.zov tests/55_map_string_keys.expected
run_stdout 57_array_index_ownership tests/57_array_index_ownership.zov tests/57_array_index_ownership.expected
run_stdout 58_map_str_lookup tests/58_map_str_lookup.zov tests/58_map_str_lookup.expected
run_stdout 59_intrinsic_names tests/59_intrinsic_names.zov tests/59_intrinsic_names.expected
run_exit  60_private_module_types tests/60_private_module_types.zov 0
run_stdin 51_shell_workload tests/51_shell_workload.zov "echo hello world\necho again\nexit" tests/51_shell_workload.expected
run_stdin 53_zovsh_overwrite tests/53_zovsh_overwrite.zov 'echo hello world\nexport FOO=bar\necho $FOO\necho $NOPE\ncd /tmp\npwd\necho done\nexit' tests/53_zovsh_overwrite.expected
run_stdin 56_zovsh_proc_fs tests/56_zovsh_proc_fs.zov 'echo $GREETING\necho $NOPE\nruntrue\nexit' tests/56_zovsh_proc_fs.expected
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
run_stdout 46_resource_destruction tests/46_resource_destruction.zov tests/46_resource_destruction.expected
run_stdout 49_borrow_paths tests/49_borrow_paths.zov tests/49_borrow_paths.expected
run_panic 38_div_by_zero tests/38_div_by_zero.zov E1003 "integer division by zero" 8:18
run_panic 39_bounds_panic tests/39_bounds_panic.zov E1001 "index 5 is outside 0..2" 8:19
run_panic 44_slice_range_panic tests/44_slice_range_panic.zov E1002 "slice range 1..9 is outside 0..4" 6:20
run_panic 40_assert_panic tests/40_assert_panic.zov E1006 "assert failed:" 4:5
run_panic 41_unwrap_null_panic tests/41_unwrap_null_panic.zov E1004 "unwrap of null" 4:19
run_panic 45_generic_bounds_panic tests/45_generic_bounds_panic.zov E1001 "index 5 is outside 0..2" 3:13
run_panic 47_alloc_overflow tests/47_alloc_overflow.zov E1006 "assert failed:"
run_panic 48_vec_capacity_overflow tests/48_vec_capacity_overflow.zov E1006 "assert failed:"
run_panic 50_arena_capacity tests/50_arena_capacity.zov E1006 "assert failed:"
run_panic_release 38_div_by_zero tests/38_div_by_zero.zov E1003 "integer division by zero" 8:18
run_panic_release 39_bounds_panic tests/39_bounds_panic.zov E1001 "index 5 is outside 0..2" 8:19
run_panic_release 44_slice_range_panic tests/44_slice_range_panic.zov E1002 "slice range 1..9 is outside 0..4" 6:20
LONG_LINE=$(printf '%0300d' 0 | tr '0' 'x')
run_stdin 21_long_stdin tests/21_long_stdin.zov "$LONG_LINE" tests/21_long_stdin.expected
run_stdio 61_io_stderr tests/61_io_stderr.zov tests/61_io_stderr.stdout tests/61_io_stderr.stderr

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
run_freestanding_alloc_hook
run_freestanding_minimal_runtime
run_strict_c99
run_freestanding_arch
run_freestanding_package
run_sanitizer
run_sanitizer_sweep
run_zovc_offline
run_staged_install
run_sklad_integration
run_packaging_and_installer

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
