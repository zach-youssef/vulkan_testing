#!/bin/bash
# Builds a commit and runs its GPU, example and web tests in the checkout's pre-push worktree, for
# the pre-push hook (tools/git-hooks/pre-push; the "Pre-push hook" section of
# docs/building-and-testing.md says what it runs). Callers hold the GPU lock (tools/gpu-lock.sh).
#   tools/test-commit.sh <git common dir> <commit> <device name>
#
# Exits 1 if the build or a test fails, 2 if the worktree can't be set up. A worktree that can't
# be checked out (its checkout moved, or an interrupted checkout left a lock) is made again, once.
set -u

if [ $# -ne 3 ]; then
    echo "usage: $0 <git common dir> <commit> <device name>" >&2
    exit 2
fi
common=$1 commit=$2 device=$3
worktree=$common/zgal-pre-push

# recreate: the worktree from scratch (a deleted or moved one leaves its registration behind).
# Only this worktree's registration is dropped (not git worktree prune's, which takes every
# worktree whose directory is missing): the one named for it, and any whose gitdir names it.
recreate() {
    rm -rf "$worktree"
    rm -rf "$common/worktrees/zgal-pre-push"
    for admin in "$common"/worktrees/*/; do
        [ "$(cat "${admin}gitdir" 2>/dev/null)" = "$worktree/.git" ] && rm -rf "$admin"
    done
    git --git-dir="$common" worktree add -q --detach "$worktree" "$commit"
}
if ! { [ -e "$worktree/.git" ] && git -C "$worktree" checkout -q --detach --force "$commit"; }; then
    [ ! -e "$worktree" ] || echo "pre-push: making $worktree again"
    recreate || { echo "pre-push: can't check out $commit into $worktree"; exit 2; }
fi
cd "$worktree" || exit 2

echo "pre-push: testing $(git log --format='%h %s' -1 "$commit" | cut -c1-72) on $device"
echo "  in $worktree (logs in build/logs)"
# The build first, in parallel; the tests then run one at a time, as they share the GPU
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
tools/run-test.sh build build/logs/build.log -- \
    make -j"$jobs" build/tests/gpu_tests examples web || exit 1
make -k --no-print-directory test-gpu test-examples test-web BROWSERS="chromium firefox" || exit 1
