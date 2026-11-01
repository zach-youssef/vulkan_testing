#!/bin/bash
# Runs a command holding the machine-wide GPU lock, so local GPU runs (the pre-push hook's tests,
# from any checkout or worktree) don't overlap: while another run holds it, says who and waits.
#   tools/gpu-lock.sh <command> [args...]
#
# The lock is an flock(2) lock on /tmp/zgal-gpu.lock (ZGAL_GPU_LOCK names another file, for
# tests), which the kernel releases when its holder exits, so a killed run leaves no stale lock.
# The file holds the current holder's description. The command doesn't inherit the lock's file
# descriptor, so processes it leaves running don't keep the lock. Exits with the command's status,
# or 2 if the lock can't be taken.
#
# Only runs that take the lock are serialized: so far the pre-push hook's. It isn't re-entrant: a
# command that runs this script again (e.g. a make target taking it under the hook) waits for
# itself forever, so making other GPU runs take it needs a marker the outer run exports.
#
# Written for macOS's bash 3.2 and lockf(1) as well as Linux's flock(1).
set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi
lock=${ZGAL_GPU_LOCK:-/tmp/zgal-gpu.lock}

# fd 9 stays open (and locked) until this script exits
exec 9>>"$lock" || exit 2
if command -v flock > /dev/null; then
    try_lock() { flock -n 9; }
    wait_lock() { flock 9; }
elif command -v lockf > /dev/null; then
    try_lock() { lockf -s -t 0 9; }
    wait_lock() { lockf -s 9; }
else
    echo "$0: needs flock (Linux) or lockf (macOS)" >&2
    exit 2
fi

if ! try_lock; then
    echo "Waiting for the GPU lock ($lock), held by: $(cat "$lock" 2>/dev/null)" >&2
    wait_lock || exit 2
fi
echo "pid $$ in $PWD: $*" > "$lock"
"$@" 9>&-
