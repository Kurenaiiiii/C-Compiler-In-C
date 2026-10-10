#!/bin/sh
# run.sh - test runner for ccc.
# For each tests/test-*.c: compile with ccc and with gcc, run both, and
# check results against the expectation:
#   - a `// exit: N` marker in the source means exit code N (default 0);
#   - a tests/<name>.expected file means stdout must match it exactly.
# The gcc build must satisfy the same expectation (validates the test),
# and ccc's behavior must agree with gcc's.
# For each tests/err-*.c: ccc must exit non-zero and print // ERROR: <sub>.
CCC=${CCC:-./ccc}
TMP=${TMPDIR:-tests/tmp}
mkdir -p "$TMP"
pass=0
fail=0

report_fail() {
    echo "FAIL $1: $2"
    fail=$((fail + 1))
}

# --- valid programs ---
for t in tests/test-*.c; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .c)
    exp=$(sed -n 's|^// *exit: *\([0-9][0-9]*\).*|\1|p' "$t" | head -n 1)
    [ -z "$exp" ] && exp=0

    if ! "$CCC" "$t" -o "$TMP/$name" 2>"$TMP/$name.ccc.err"; then
        report_fail "$name" "ccc failed to compile: $(head -n 1 "$TMP/$name.ccc.err")"
        continue
    fi
    "$TMP/$name" >"$TMP/$name.out" 2>&1
    got=$?
    if ! gcc -w "$t" -o "$TMP/$name.gcc" 2>"$TMP/$name.gcc.err"; then
        report_fail "$name" "gcc failed to compile test (bad test)"
        continue
    fi
    "$TMP/$name.gcc" >"$TMP/$name.gcc.out" 2>&1
    gccgot=$?

    if [ -f "tests/$name.expected" ]; then
        if ! cmp -s "$TMP/$name.out" "tests/$name.expected"; then
            report_fail "$name" "stdout mismatch (ccc)"
            continue
        fi
        if ! cmp -s "$TMP/$name.gcc.out" "tests/$name.expected"; then
            report_fail "$name" "gcc disagrees with .expected (bad test)"
            continue
        fi
        if [ "$got" != "$gccgot" ]; then
            report_fail "$name" "exit $got, gcc exit $gccgot"
            continue
        fi
    else
        if [ "$got" != "$exp" ]; then
            report_fail "$name" "exit $got, expected $exp"
            continue
        fi
        if [ "$gccgot" != "$exp" ]; then
            report_fail "$name" "gcc exit $gccgot, expected $exp (bad test)"
            continue
        fi
    fi
    pass=$((pass + 1))
done

# --- error cases: clean diagnostic, non-zero exit, no crash ---
# NOTE: error exits intentionally skip leak cleanup (like gcc on fatal
# errors), so leak detection stays off for these invocations.
for t in tests/err-*.c; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .c)
    sub=$(sed -n 's|^// *ERROR: *\(.*\)|\1|p' "$t" | head -n 1)
    if ASAN_OPTIONS=detect_leaks=0 "$CCC" "$t" -o "$TMP/$name" 2>"$TMP/$name.err"; then
        report_fail "$name" "ccc accepted invalid program"
        continue
    fi
    rc=$?
    if [ "$rc" -gt 128 ]; then
        report_fail "$name" "ccc crashed (signal $((rc - 128)))"
        continue
    fi
    if [ -n "$sub" ] && ! grep -q -F "$sub" "$TMP/$name.err"; then
        report_fail "$name" "stderr lacks '$sub': $(head -n 1 "$TMP/$name.err")"
        continue
    fi
    if ! grep -q "error:" "$TMP/$name.err"; then
        report_fail "$name" "stderr has no 'error:' diagnostic"
        continue
    fi
    pass=$((pass + 1))
done

total=$((pass + fail))
nvalid=$(ls tests/test-*.c 2>/dev/null | wc -l)
echo "tests: $pass passed, $fail failed, $total total ($nvalid valid programs)"
if [ "$nvalid" -lt 150 ]; then
    echo "FAIL: need at least 150 valid test programs, have $nvalid"
    exit 1
fi
[ "$fail" -eq 0 ]
