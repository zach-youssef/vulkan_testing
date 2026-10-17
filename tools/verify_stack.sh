#!/bin/zsh
# Verifies every commit in a range builds and passes its tests.
#   tools/verify_stack.sh <repo> <base> [head]
# e.g. tools/verify_stack.sh . main HEAD
#
# Prints one line per commit with the CPU and GPU test exit codes and summaries (and those of the
# ZGAL tests and example smoke tests where a commit has them); the per-commit
# build and test logs are kept in the temporary directory printed at the end.
# Each commit is exported to its own directory. All builds and CPU test runs happen in
# parallel; GPU tests run one commit at a time (they share the device) on a separate worker,
# starting as soon as each commit's build has finished.
set -u
repo=$1; base=$2; head=${3:-HEAD}
out=$(mktemp -d -t verify_stack)
mkdir -p $out
commits=($(git -C $repo rev-list --reverse $base..$head))

build_and_cpu_test() {
    local c=$1 d=$out/$1
    mkdir -p $d && git -C $repo archive $c | tar -x -C $d
    if (cd $d && make >build.log 2>&1 && make build/tests/cpu_tests build/tests/gpu_tests >>build.log 2>&1); then
        (cd $d && ./build/tests/cpu_tests >cpu.log 2>&1); echo $? > $d/cpu.exit
        # ZGAL's compiler tests and documentation; commits before ZGAL have none
        if grep -q '^test-dsl:' $d/Makefile; then
            (cd $d && make test-dsl >dsl.log 2>&1); echo $? > $d/dsl.exit
        fi
        echo ok > $d/build.status
    else
        echo failed > $d/build.status
    fi
}

gpu_worker() {
    for c in $commits; do
        local d=$out/$c
        while [[ ! -f $d/build.status ]]; do sleep 1; done
        if [[ $(cat $d/build.status) == ok ]]; then
            (cd $d && ./build/tests/gpu_tests >gpu.log 2>&1); echo $? > $d/gpu.exit
            # The examples' smoke tests use the GPU too; commits before they existed have none
            if grep -q '^test-examples:' $d/Makefile; then
                (cd $d && make test-examples >examples.log 2>&1); echo $? > $d/examples.exit
            fi
        fi
    done
}

for c in $commits; do build_and_cpu_test $c & done
gpu_worker &
wait

summary() { grep -h 'test cases' $1 2>/dev/null | sed 's/.*test cases: *//; s/ | 0 skipped//'; }
for c in $commits; do
    d=$out/$c
    line=$(git -C $repo log --oneline -1 $c | cut -c1-64)
    if [[ $(cat $d/build.status) != ok ]]; then echo "$line: BUILD FAILED ($d/build.log)"; continue; fi
    examples=""
    if [[ -f $d/examples.exit ]]; then examples=", examples exit $(cat $d/examples.exit)"; fi
    dsl=""
    if [[ -f $d/dsl.exit ]]; then dsl=", zgal exit $(cat $d/dsl.exit)"; fi
    echo "$line: cpu exit $(cat $d/cpu.exit) [$(summary $d/cpu.log)], gpu exit $(cat $d/gpu.exit) [$(summary $d/gpu.log)]$dsl$examples"
done
echo "Logs: $out"
