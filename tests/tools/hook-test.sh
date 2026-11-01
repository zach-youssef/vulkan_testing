#!/bin/bash
# Tests the pre-push hook (tools/git-hooks/pre-push, tools/test-commit.sh) and the GPU lock
# (tools/gpu-lock.sh) without a GPU: pushes to a local bare repo from a scratch repo whose Makefile
# stands in for the real targets, with a stub vulkaninfo first on PATH. Prints "PASS <case>" or
# "FAIL <case>" (with the reason) per case, the format tools/run-test.sh counts; exits 1 if any
# case fails.
set -u

src=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hook-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
failures=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; echo "  $2"; failures=$((failures + 1)); }

export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com
export GIT_CONFIG_NOSYSTEM=1 HOME=$tmp/home
mkdir -p "$HOME"
export ZGAL_GPU_LOCK=$tmp/gpu.lock
# What the stand-in targets saw: the commit tested and BROWSERS
export RECORD=$tmp/record

# vulkaninfo stand-ins: "gpu" or "cpu" (lavapipe only)
mkdir -p "$tmp/bin"
cat > "$tmp/bin/vulkaninfo" << 'EOF'
#!/bin/sh
case $(cat "$STUB_DEVICE") in
gpu) printf 'GPU0:\n\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU\n\tdeviceName         = llvmpipe (LLVM 20.1.2, 256 bits)\nGPU1:\n\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU\n\tdeviceName         = Apple M4\n' ;;
cpu) printf 'GPU0:\n\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU\n\tdeviceName         = llvmpipe (LLVM 20.1.2, 256 bits)\n' ;;
esac
EOF
chmod +x "$tmp/bin/vulkaninfo"
export STUB_DEVICE=$tmp/device PATH=$tmp/bin:$PATH
device() { echo "$1" > "$STUB_DEVICE"; }

# The pushing repo: the real run-test.sh, and a Makefile whose tests pass when state.txt says "good"
git init -q --bare "$tmp/remote.git"
git init -q -b main "$tmp/repo"
repo=$tmp/repo
mkdir -p "$repo/tools"
cp "$src/tools/run-test.sh" "$repo/tools/"
cat > "$repo/Makefile" << 'EOF'
build/tests/gpu_tests examples web:
	@echo building $@
test-gpu test-examples test-web:
	@tools/run-test.sh $@ build/logs/$@.log -- sh -c \
	  'echo "$$(git rev-parse HEAD) $@ $(BROWSERS)" >> "$$RECORD"; test "$$(cat state.txt)" = good'
.PHONY: build/tests/gpu_tests examples web test-gpu test-examples test-web
EOF
echo good > "$repo/state.txt"
git -C "$repo" add -A && git -C "$repo" commit -q -m good
good=$(git -C "$repo" rev-parse HEAD)
echo bad > "$repo/state.txt"
git -C "$repo" commit -q -am bad
bad=$(git -C "$repo" rev-parse HEAD)
git -C "$repo" branch -q bad && git -C "$repo" reset -q --hard "$good"
git -C "$repo" remote add origin "$tmp/remote.git"
git -C "$repo" config core.hooksPath "$src/tools/git-hooks"

# push_to <remote> <args>: runs git push in the repo, its output in $out and its status in $status
push_to() {
    rm -f "$RECORD"
    out=$(git -C "$repo" push -q "$@" 2>&1)
    status=$?
}
push() { push_to origin "$@"; }
# fresh <good|bad>: a new commit (on no branch) with that commit's files. Called in $(...), so its
# count is kept in a file.
fresh() {
    local base n
    if [ "$1" = good ]; then base=$good; else base=$bad; fi
    n=$(($(cat "$tmp/count" 2>/dev/null || echo 0) + 1))
    echo $n > "$tmp/count"
    git -C "$repo" commit-tree -p "$base" -m "$1 $n" "$base^{tree}"
}
remote_sha() { git -C "$tmp/remote.git" rev-parse -q --verify "refs/heads/$1"; }
recorded() { cat "$RECORD" 2>/dev/null; }

device gpu
push main
expected="$good test-gpu chromium firefox
$good test-examples chromium firefox
$good test-web chromium firefox"
if [ $status -eq 0 ] && [ "$(recorded)" = "$expected" ] && [ "$(remote_sha main)" = "$good" ] &&
    echo "$out" | grep -q '^PASS test-gpu$' && echo "$out" | grep -q '^PASS build$' &&
    echo "$out" | grep -q 'on Apple M4$'; then
    pass push-runs-the-tests
else
    fail push-runs-the-tests "status $status, recorded: $(recorded), output: $out"
fi

# The pushed commit is tested, not the pushing worktree's (good) files; a failure blocks the push
push bad
if [ $status -ne 0 ] && [ -z "$(remote_sha bad)" ] && [ "$(recorded | cut -d' ' -f1 | sort -u)" = "$bad" ] &&
    echo "$out" | grep -q '^FAIL test-gpu (exit 1)$' && echo "$out" | grep -q '^FAIL test-web (exit 1)$'; then
    pass push-tests-the-pushed-commit
else
    fail push-tests-the-pushed-commit "status $status, recorded: $(recorded), output: $out"
fi

push --no-verify bad
if [ $status -eq 0 ] && [ "$(remote_sha bad)" = "$bad" ] && [ -z "$(recorded)" ]; then
    pass no-verify-skips-the-tests
else
    fail no-verify-skips-the-tests "status $status, recorded: $(recorded), output: $out"
fi

push :bad
if [ $status -eq 0 ] && [ -z "$(remote_sha bad)" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q 'only deletes refs'; then
    pass deleting-push-skips-the-tests
else
    fail deleting-push-skips-the-tests "status $status, recorded: $(recorded), output: $out"
fi

# Two refs at one commit are tested once
c=$(fresh good)
push "$c:refs/heads/a" "$c:refs/heads/b"
if [ $status -eq 0 ] && [ "$(recorded | wc -l | tr -d ' ')" = 3 ]; then
    pass one-run-per-commit
else
    fail one-run-per-commit "status $status, recorded: $(recorded), output: $out"
fi

# Commits the remote has are not tested again: under its remote-tracking refs (a new branch at
# main), or, pushing to a URL (no remote-tracking refs), under a ref the push updates (a rewind)
push main:refs/heads/copy
if [ $status -eq 0 ] && [ "$(remote_sha copy)" = "$good" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q "already on origin"; then
    pass known-commit-skipped
else
    fail known-commit-skipped "status $status, recorded: $(recorded), output: $out"
fi
c=$(fresh good)
push "$c:refs/heads/rewind"
push_to "$tmp/remote.git" "+$good:refs/heads/rewind"
if [ $status -eq 0 ] && [ "$(remote_sha rewind)" = "$good" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q "already on"; then
    pass rewound-commit-skipped
else
    fail rewound-commit-skipped "status $status, recorded: $(recorded), output: $out"
fi

device cpu
c=$(fresh bad)
push "$c:refs/heads/cpu-only"
if [ $status -eq 0 ] && [ "$(remote_sha cpu-only)" = "$c" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q 'no GPU Vulkan device found'; then
    pass cpu-only-skips-the-tests
else
    fail cpu-only-skips-the-tests "status $status, recorded: $(recorded), output: $out"
fi
device gpu

# Without vulkaninfo: PATH without the directories that have one
no_vulkaninfo=""
old_ifs=$IFS; IFS=:
for d in $PATH; do [ -x "$d/vulkaninfo" ] || no_vulkaninfo="$no_vulkaninfo${no_vulkaninfo:+:}$d"; done
IFS=$old_ifs
c=$(fresh bad)
PATH=$no_vulkaninfo push "$c:refs/heads/no-vulkaninfo"
if [ $status -eq 0 ] && [ "$(remote_sha no-vulkaninfo)" = "$c" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q 'vulkaninfo not found'; then
    pass no-vulkaninfo-skips-the-tests
else
    fail no-vulkaninfo-skips-the-tests "status $status, recorded: $(recorded), output: $out"
fi

# A pre-push worktree deleted by hand is created again
rm -rf "$repo/.git/zgal-pre-push"
push "$(fresh good):refs/heads/deleted"
if [ $status -eq 0 ] && [ -n "$(recorded)" ]; then
    pass deleted-worktree-is-recreated
else
    fail deleted-worktree-is-recreated "status $status, recorded: $(recorded), output: $out"
fi

# So is one left locked by an interrupted checkout
touch "$repo/.git/worktrees/zgal-pre-push/index.lock"
push "$(fresh good):refs/heads/locked"
if [ $status -eq 0 ] && [ -n "$(recorded)" ] && echo "$out" | grep -q 'making .* again'; then
    pass locked-worktree-is-recreated
else
    fail locked-worktree-is-recreated "status $status, recorded: $(recorded), output: $out"
fi

# And one whose checkout moved (its .git file names the old path)
mv "$repo" "$tmp/moved"
repo=$tmp/moved
push "$(fresh good):refs/heads/moved"
if [ $status -eq 0 ] && [ -n "$(recorded)" ] && echo "$out" | grep -q 'making .* again'; then
    pass moved-worktree-is-recreated
else
    fail moved-worktree-is-recreated "status $status, recorded: $(recorded), output: $out"
fi

# Making it again leaves other worktrees' registrations alone, even one whose directory is missing
git -C "$repo" worktree add -q --detach "$tmp/other" HEAD
rm -rf "$tmp/other" "$repo/.git/zgal-pre-push"
push "$(fresh good):refs/heads/others"
if [ $status -eq 0 ] && [ -n "$(recorded)" ] && [ -d "$repo/.git/worktrees/other" ]; then
    pass recreating-keeps-other-worktrees
else
    fail recreating-keeps-other-worktrees "status $status, recorded: $(recorded), output: $out"
fi

# A worktree that can't be made blocks the push, saying so rather than that the tests failed
rm -rf "$repo/.git/zgal-pre-push"
chmod 555 "$repo/.git/worktrees"
push "$(fresh good):refs/heads/unwritable"
chmod 755 "$repo/.git/worktrees"
if [ $status -ne 0 ] && [ -z "$(remote_sha unwritable)" ] && [ -z "$(recorded)" ] &&
    echo "$out" | grep -q "can't check out" && echo "$out" | grep -q "couldn't run the tests" &&
    ! echo "$out" | grep -q 'tests failed'; then
    pass setup-failure-blocks-the-push
else
    fail setup-failure-blocks-the-push "status $status, recorded: $(recorded), output: $out"
fi

# A push waits while another run holds the GPU lock (the holder writes the file once it has it)
rm -f "$ZGAL_GPU_LOCK"
"$src/tools/gpu-lock.sh" sleep 2 &
holder=$!
tries=0
while [ ! -s "$ZGAL_GPU_LOCK" ] && [ $tries -lt 100 ]; do sleep 0.1; tries=$((tries + 1)); done
push "$(fresh good):refs/heads/waited"
wait $holder
if [ $status -eq 0 ] && [ -n "$(recorded)" ] && echo "$out" | grep -q 'Waiting for the GPU lock'; then
    pass push-waits-for-the-gpu-lock
else
    fail push-waits-for-the-gpu-lock "status $status, recorded: $(recorded), output: $out"
fi

# The lock: the command's status, and no lock descriptor (9) left to the command
"$src/tools/gpu-lock.sh" sh -c 'exit 7'
status=$?
if [ $status -eq 7 ]; then pass lock-keeps-the-status; else fail lock-keeps-the-status "status $status"; fi
if "$src/tools/gpu-lock.sh" sh -c ': >&9' 2>/dev/null; then
    fail lock-descriptor-not-inherited "fd 9 is open in the command"
else
    pass lock-descriptor-not-inherited
fi

[ $failures -eq 0 ]
