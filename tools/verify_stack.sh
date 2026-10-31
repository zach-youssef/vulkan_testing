#!/bin/zsh
# Verifies every commit in a range builds and passes its tests.
#   tools/verify_stack.sh <repo> <base> [head]
# e.g. tools/verify_stack.sh . main HEAD
#
# Prints each commit, then the summary of its build and of each of its tests (tools/run-test.sh's:
# "PASS test-cpu (99 cases)", or FAIL with the failing cases and the log's last lines): the CPU and
# GPU tests, and the ZGAL tests, example smoke tests and web tests where a commit has them. The
# logs are kept in the temporary directory printed at the end: <dir>/<commit>/logs/<name>.log, and
# for the make targets of commits whose Makefile has run-test.sh wrappers, their full output in
# <dir>/<commit>/build/logs/<target>.log (the step log then holds only the targets' summaries).
# Exits 1 if any commit fails to build or fails a test.
# Each commit is exported to its own directory. All builds and CPU test runs happen in
# parallel; GPU tests run one commit at a time (they share the device) on a separate worker,
# starting as soon as each commit's build has finished. This script's run-test.sh runs them all,
# so older commits, which have none, get the same summaries.
set -u
repo=$1; base=$2; head=${3:-HEAD}
run_test=${0:A:h}/run-test.sh
out=$(mktemp -d -t verify_stack)
mkdir -p $out
commits=($(git -C $repo rev-list --reverse $base..$head))

# run <commit dir> <name> <command...>: its summary in <name>.summary, its status in <name>.exit
run() {
    local d=$1 name=$2; shift 2
    (cd $d && $run_test $name $d/logs/$name.log -- "$@" > $d/$name.summary); echo $? > $d/$name.exit
}

build_and_cpu_test() {
    local c=$1 d=$out/$1
    mkdir -p $d && git -C $repo archive $c | tar -x -C $d
    run $d build sh -c 'make && make build/tests/cpu_tests build/tests/gpu_tests'
    if [[ $(cat $d/build.exit) == 0 ]]; then
        run $d test-cpu ./build/tests/cpu_tests
        # ZGAL's compiler tests and documentation; commits before ZGAL have none
        if grep -q '^test-dsl:' $d/Makefile; then run $d test-dsl make test-dsl; fi
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
            run $d test-gpu ./build/tests/gpu_tests
            # The examples' smoke tests use the GPU too; commits before they existed have none
            if grep -q '^test-examples:' $d/Makefile; then run $d test-examples make test-examples; fi
            # The web runtime's tests run on the GPU through Dawn
            if grep -q '^test-web:' $d/Makefile; then run $d test-web make test-web; fi
        fi
    done
}

for c in $commits; do build_and_cpu_test $c & done
gpu_worker &
wait

failed=0
for c in $commits; do
    d=$out/$c
    git -C $repo log --oneline -1 $c | cut -c1-80
    for name in build test-cpu test-dsl test-gpu test-examples test-web; do
        [[ -f $d/$name.exit ]] || continue
        [[ $(cat $d/$name.exit) == 0 ]] || failed=1
        sed 's/^/  /' $d/$name.summary
    done
done
echo "Logs: $out"
exit $failed
