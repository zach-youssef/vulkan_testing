#!/bin/bash
# Runs a test command with its output in a log, printing only a summary, and exits with the
# command's status.
#   tools/run-test.sh <name> <log> -- <command> [args...]
#
# Prints "PASS <name> (<n> cases)" on success. On a failure, prints "FAIL <name> (<n> cases,
# exit <status>)", the failing cases' names ("  failed: <name>"), then the log's last lines
# (indented) and its path. The case count and names come from the log: doctest's summary and the
# "TEST CASE:" blocks with errors, raco test's "<n> tests passed" or "<m>/<n> test failures" and
# its failures' names, node --test's spec reporter ("ℹ tests <n>", "✖ failing tests:"), and the
# summaries of nested runs of this script (the failing ones' names; one case each unless they give
# a count, and their failing cases as "<name>: <case>"). Without any, the count is left out. Lines
# starting "note: " (e.g. a skipped browser) are repeated after the summary, as "  note: ...".
# Under make -n it only runs the command.
#
# TEST_VERBOSE=1 also copies the output to the terminal as it runs. TEST_TAIL sets how many of the
# log's last lines a failure prints (30 by default).
#
# Written for macOS's bash 3.2 and awk as well as Linux's.
set -u

if [ $# -lt 4 ] || [ "$3" != "--" ]; then
    echo "usage: $0 <name> <log> -- <command> [args...]" >&2
    exit 2
fi
name=$1 log=$2
shift 3

# Under make -n (a dry run still runs the recipes that call $(MAKE)), runs the command as it is:
# the sub-make only prints its commands. Only the first word of MAKEFLAGS holds the single-letter
# options (with or without a dash); later words are long options, "--", variables, and (make 3.81)
# the arguments of options such as -I.
dry_run() {
    local w
    set -f
    set -- ${MAKEFLAGS:-}
    set +f
    [ $# -gt 0 ] || return 1
    w=${1#-}
    case $w in *[!a-zA-Z]* | "") return 1 ;; *n*) return 0 ;; esac
    return 1
}
if dry_run; then "$@"; exit; fi

mkdir -p "$(dirname "$log")" || exit 2
if [ -n "${TEST_VERBOSE:-}" ] && [ "${TEST_VERBOSE}" != 0 ]; then
    "$@" 2>&1 | tee "$log"
    status=${PIPESTATUS[0]}
else
    "$@" > "$log" 2>&1
    status=$?
fi

# "C <count>" (none if the log has no count), "F <failing case>" and "N <note>" lines
parsed=$(awk '
    function add(n) { if (!(n in seen)) { seen[n] = 1; names[++count] = n } }
    function note(n) { if (!(n in noted)) { noted[n] = 1; notes[++note_count] = n } }
    # The end of a nested failure summary: its name stands for its cases if it listed none
    function end_nested() { if (nested != "" && !nested_cases) add(nested); nested = "" }
    nested != "" && /^  failed: / {
        n = $0; sub(/^  failed: /, "", n); add(nested ": " n); nested_cases = 1; next
    }
    nested != "" { end_nested() }
    # Notes a passing run must still show (e.g. a skipped browser): "note: ..." lines, and those of
    # nested summaries
    /^note: / || /^  note: / { n = $0; sub(/^ *note: /, "", n); note(n) }
    # doctest: "[doctest] test cases: 212 | 212 passed | 0 failed | 0 skipped"
    /^\[doctest\] test cases:/ { split($0, f, /[:|]/); cases += f[2] + 0; counted = 1 }
    # doctest prints a block per case with output, up to a "===" line: "TEST CASE:", then its
    # errors (": ERROR:" or ": FATAL ERROR:", crashes too), or only MESSAGE and WARNING lines
    /^TEST CASE:/ { tc = $0; sub(/^TEST CASE: */, "", tc); next }
    /^=+$/ { tc = "" }
    tc != "" && /: (FATAL )?ERROR:/ { add(tc); tc = "" }
    # raco test: "12 tests passed", "1 test passed" or "3/12 test failures"; a failure is
    # "--------------------", its test-case name (if any), "FAILURE" or "ERROR", then its location
    /^[0-9]+ tests? passed$/ { cases += $1; counted = 1 }
    /^[0-9]+\/[0-9]+ tests? failures?$/ { split($1, f, "/"); cases += f[2]; counted = 1 }
    /^(FAILURE|ERROR)$/ { pending = (prev ~ /^-+$/) ? "" : prev; failure = 1 }
    failure && /^location:/ {
        sub(/^location: */, ""); add(pending != "" ? pending " (" $0 ")" : $0); failure = 0
    }
    failure && /^-+$/ { if (pending != "") add(pending); failure = 0 }
    # node --test --test-reporter=spec: "ℹ tests 42"; the failures follow "✖ failing tests:"
    /^ℹ tests [0-9]+$/ { cases += $3; counted = 1 }
    /^✖ failing tests:/ { node_failures = 1 }
    node_failures && /^✖ / && !/^✖ failing tests:/ {
        n = $0; sub(/^✖ /, "", n); sub(/ \([0-9.]+m?s\)$/, "", n); add(n)
    }
    # Nested runs of this script: "PASS name (n cases)" or "FAIL name (n cases, exit s)", whose
    # "  failed: <case>" lines become "<name>: <case>"
    /^(PASS|FAIL) [^ ]+( \(|$)/ {
        if (match($0, /\([0-9]+ cases?/)) cases += substr($0, RSTART + 1) + 0; else cases += 1
        counted = 1
        if ($1 == "FAIL") { nested = $2; nested_cases = 0 }
    }
    { prev = $0 }
    END {
        end_nested()
        if (counted) print "C " cases
        for (i = 1; i <= count; i++) print "F " names[i]
        for (i = 1; i <= note_count; i++) print "N " notes[i]
    }
' "$log")
cases=$(printf '%s\n' "$parsed" | sed -n 's/^C //p')
failed=$(printf '%s\n' "$parsed" | sed -n 's/^F //p')
notes=$(printf '%s\n' "$parsed" | sed -n 's/^N //p')

if [ -n "$cases" ]; then
    if [ "$cases" = 1 ]; then counted="1 case"; else counted="$cases cases"; fi
else
    counted=""
fi

if [ "$status" -eq 0 ]; then
    echo "PASS $name${counted:+ ($counted)}"
    [ -z "$notes" ] || printf '%s\n' "$notes" | sed 's/^/  note: /'
else
    echo "FAIL $name (${counted:+$counted, }exit $status)"
    if [ -n "$failed" ]; then
        printf '%s\n' "$failed" | head -n 50 | sed 's/^/  failed: /'
        total=$(printf '%s\n' "$failed" | wc -l | tr -d ' ')
        [ "$total" -le 50 ] || echo "  ... and $((total - 50)) more"
    fi
    # After the failed list: a nested run's awk block ends at its first line that is not "  failed:"
    [ -z "$notes" ] || printf '%s\n' "$notes" | sed 's/^/  note: /'
    lines=${TEST_TAIL:-30}
    echo "  last $lines lines of $log:"
    tail -n "$lines" "$log" | sed 's/^/    /'
fi
exit "$status"
