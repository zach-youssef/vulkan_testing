# Building and testing

## Requirements

The Vulkan SDK, `glslc`, GLFW and GLM for the wrapper; Racket 8 (`racket` and `raco`, with
Scribble, as in the main distribution) for ZGAL; Node 24 (which runs TypeScript) and npm for ZGAL's
web backend, plus `curl` and `tar`, with which the build downloads the pinned
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) the first time it builds the web runtime's image
decoder (see below). Development happens on an Apple M4 with MoltenVK
([wrapper.md](wrapper.md#the-development-device) has what that means for code).

`zgal.mk` finds the Vulkan headers and loader in `$VULKAN_SDK` when it's set (the SDK's
`setup-env.sh` sets it), else in the system's paths: on macOS `/usr/local` (where the SDK's
installer puts them) and Homebrew's for GLFW and GLM; on Linux the default ones (e.g. Ubuntu's
`libvulkan-dev`, `libglfw3-dev`, `libglm-dev`, plus `mesa-vulkan-drivers` for lavapipe). The
compiler is clang, the one tested (`c++` on macOS, with libc++; `clang++` on Linux, with
libstdc++), unless `CXX` is given (`make CXX=g++`).

The web runtime's image decoder is stb_image built to WebAssembly
([web.md](web.md#the-image-decoder)), which needs a clang with a wasm32 target and `wasm-ld`
(Apple's clang has none). The build uses wasi-sdk, whose version and tarballs' SHA-256 are pinned
in `.github/actions/setup-zgal/wasi-sdk.sh` (setup-zgal installs the same in CI). `WASI_SDK_PATH`
names an installed one; without it, `web/decode/decode.mk` downloads and checks the pinned release
into `.cache/wasi-sdk-<version>` (git-ignored, about 600 MB unpacked, kept by `make clean`; each
worktree has its own, so point `WASI_SDK_PATH` at one to share it). Nothing is installed
system-wide.

## Make targets

- `make`: builds the examples into `build/examples/<name>/` (shaders into
  `build/examples/<name>/shaders`).
- `make shaders`: rebuilds the examples' SPIR-V (running examples reload it).
- `make test-cpu`, `make test-gpu`, `make test`: build and run the tests (doctest). Forward
  arguments with `TEST_ARGS`, e.g. `make test-gpu TEST_ARGS='-tc="RenderGraph*"'`.
- Test targets print one line each, `PASS test-gpu (212 cases)` or `FAIL test-gpu (212 cases,
  exit 1)` with the failing cases' names and the log's last 30 lines (`TEST_TAIL=n` for more), and
  write their whole output to `build/logs/<target>.log`. `TEST_VERBOSE=1` shows the output as it
  runs too. `tools/run-test.sh <name> <log> -- <command>` does it, keeping the command's exit
  status; it counts doctest's, `raco test`'s and `node --test`'s (spec reporter) cases, and those of
  nested runs: targets running others (`test-examples` its smoke tests, `test-web` `test-pages`)
  have those targets' lines in their logs, and list their failing cases as `<target>: <case>`. Lines
  starting `note: ` (a skipped browser) show under the summary. Under `make -n` the targets only
  print their commands.
- `make test-examples`: runs every example headless with validation and synchronization
  validation; any message fails it. `make test-example-<name>` runs one.
- `make approve-screenshots`: accepts the screenshots of the last failed GPU run
  (`tests/gpu/approved/<device>/`). Never approve a changed screenshot without understanding why
  it changed.
- `make test-dsl`: builds ZGAL's documentation and runs its compiler's tests (`make test` includes
  it), installing the web backend's npm packages (`web/`) first. `make docs` only builds the
  documentation: `build/docs/zgal.html`, and `build/docs/zgal.txt` to read in a terminal.
- `make test-web` and `make test-pages`: the web backend's tests ([web.md](web.md#tests); `make
  test` includes them), with pages in headless Chromium only unless `BROWSERS="chromium firefox"`
  (Firefox opens windows). A browser that isn't installed is skipped with a note under the
  summary, or fails the run when `BROWSERS` names it. `make web` builds the web examples.
- `make test-decode`: checks the web runtime's image decoder under Node against native stb_image
  (no GPU; [web.md](web.md#the-image-decoder)); `make test-web` runs it first. `make
  build/web-decode/stb_image.wasm` builds the decoder, and `make wasi-sdk` only installs its
  compiler.
- `make test-tools`: tests the pre-push hook and the GPU lock (`tests/tools/hook-test.sh`, no GPU
  needed; `make test` includes it).
- `make run-bench`: times ZGAL's library kernels (`tools/bench`; see "Profiling").
- `zgal.mk`: the make rules building ZGAL programs (`zgal-rule`, `zgal-program`), which the Makefile
  and other projects' Makefiles include.

## GPU tests

GPU tests run headless on a shared `VulkanContext` with validation and synchronization validation
enabled, and fail when either is unavailable rather than run unchecked:
`VK_TEST_DISABLE_VALIDATION=1` or `VK_TEST_DISABLE_SYNC_VALIDATION=1` runs them without (the only
opt-outs). The `GpuTest` fixture (`tests/gpu/VkTestContext.h`) fails a test that produces any
validation message (warnings too; tests making their own contexts record them with
`ValidationMessages`), leaks managed resources or `PerFrame` collections, or leaves the number of
frames in flight changed. Programs run with `--validation` likewise fail without either. Test
shaders live in `tests/gpu/shaders`; tests also use the examples' shaders, and `testShader(ctx,
"name.comp")` loads them.

Gotchas:

- doctest's message macros (`*_MESSAGE`, `INFO`, `FAIL_CHECK`) apply the message with `*`, so `+`
  in it doesn't compile, and they print a `const char*` operand (e.g. a `?:` of two literals) as
  `1`: wrap C strings in `std::string`, or build the string first and pass that.
- `VK_LOADER_LAYERS_DISABLE='*validation*'` makes the loader hide the validation layer from the
  application, to check that a run lacking it fails.
- Graph buffers start zeroed on every backend (WebGPU guarantees it; the Vulkan graph fills them
  when it creates or recreates them), so a test reading elements nothing writes expects zeros
  without a `clear`. Device memory outside the graph (e.g. `Buffer::create`) still starts
  undefined: MoltenVK's happens to be zero, lavapipe's doesn't.
- Several `vkCmdFillBuffer`s on one buffer inside one graph node trip synchronization validation
  (write-after-write): fill it once, or split the fills across nodes.

## CI

GitHub Actions (`.github/workflows/ci.yml`) checks every pushed commit, not only the tip: the
commits a working branch has beyond `main`, or those a push to `main` brings (at most the newest
20). Each one is built on Ubuntu 24.04 with clang++ and lavapipe (`make examples` and both test
programs), then runs `make test-cpu`, `make test-dsl` and `make test-tools`, and gets a `cpu` commit status. Each one also
runs `make test-web BROWSERS=chromium` (`.github/workflows/web.yml`): Dawn under Node on lavapipe,
and the pages in Playwright's Chromium headless shell on SwiftShader (`CHROMIUM` names it, so a
missing one fails), and gets a `web` status. Firefox is skipped there, as it has no WebGPU on
Linux; commits from before `web.yml` existed get no `web` check (the commits job leaves them out).
Each one also runs `make test-gpu` and `make test-examples` on lavapipe
(`.github/workflows/gpu.yml`), with validation and synchronization validation required, and gets
a `gpu` status (commits from before `gpu.yml` existed get none). The toolchain (apt packages, the
LunarG SDK, ccache, Racket, Node) comes from `.github/actions/setup-zgal`, whose `toolchain` input
is `native` or `web` (no SDK or ccache, but wasi-sdk, as `WASI_SDK_PATH`, for the image decoder,
and clang for its native test helper).

lavapipe has one queue in one family, so CI never runs the multi-queue paths (async compute,
cross-queue ownership and timeline waits; their tests skip), nor the windowed tests (no display),
nor the web pages in Firefox: the [pre-push hook](#pre-push-hook) runs those on the Mac for each
pushed tip.

lavapipe's screenshots are approved in `tests/gpu/approved/llvmpipe/` (its device name without the
LLVM build). A failed gpu job uploads its received screenshots, FLIP error maps, the examples'
captures and the logs as the artifact `gpu-<sha>`: `gh run download <run id> -n gpu-<sha>`, look at
each received image, and only then copy it over its `.approved.png`. Failed cpu and web jobs upload
their test logs (`build/logs`) as `cpu-<sha>` and `web-<sha>`.

Gotchas:

- make predefines `CXX` (as `g++`), so `CXX ?= ...` never applies: the Makefile checks
  `$(origin CXX)` (`default`, or `undefined` under `make -R`) instead.
- ZGAL's toolchain tests run `ZGAL_CXX` as a single program, so CI puts ccache first on `PATH` as a
  `clang++` symlink rather than setting `CXX="ccache clang++"`.
- Each commit's job takes its actions from the pushed tip's `.github` (checked out into `.ci`), as
  older commits have none or older ones.
- Only jobs that run no code from the commits, npm or third-party actions hold a token that can post
  statuses, as a job's steps share one user and file system (a build could plant a `gh` that a
  later step runs with the token). The cpu and web jobs get `contents: read`; ci.yml's `pending`
  job marks their checks pending and its `statuses` job, needing them all, posts each commit's
  status from its job's conclusion in the run's jobs API, matched by the name `<context> <sha>`
  (anything but success or failure is an error, so none stays pending). Those two check nothing
  out, and no checkout persists its credentials.
- Ubuntu 24.04's AppArmor forbids the unprivileged user namespaces Chromium's sandbox needs: the
  web job runs `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` first.
- `cancelled()` and `failure()` work only in `if:`; in `env:` the workflow fails to parse. Use
  `job.status` there.
- LunarG's SDK download can crawl (once for 21 minutes): setup-zgal abandons an attempt below
  1 MB/s for 30 s or after 5 minutes and retries any failure, starting none after 5 minutes, so it
  gives up after about 10.
- Jobs that build no C++ use setup-zgal's `web` toolchain, which saves no ccache: saving it under
  the commit's key before the cpu job does would make the cpu job's save, with its new objects,
  fail. The gpu job builds C++ alongside the cpu job, so it only restores the ccache
  (`save-ccache: false`).
- GitHub scopes caches per branch: a branch restores only its own caches or the default branch's,
  and what it saves never reaches the default branch, so a working branch's jobs can't poison
  `main`'s caches (no further hardening needed).
- make's `$(if)` splits its arguments on commas, so flags like `-Wl,-rpath,...` inside it use
  zgal.mk's `$(ZGAL_COMMA)`.
- A sub-make gets make's jobserver (so `-j` reaches it, and `make -n` still runs it) only when its
  recipe line contains `$(MAKE)` itself (as `$(RUN_TEST) $(MAKE) ...` does), not a variable whose
  value runs make.
- macOS's make 3.81 compares mtimes to the second: editing a file, building and reverting it within
  one second leaves a stale object. Touch the file or wait a second.
- `MAKEFLAGS` differs by level under make 3.81: the top-level make's recipes see the single-letter
  options as the first word without a dash (`nk`), but a sub-make's see them after the long ones,
  each with a dash (` --no-print-directory -n`). A script detecting `make -n` must handle both.
  (`run-test.sh` checks only the first word, so a run-test target inside a sub-make, such as
  `test-pages` under `make -n test-web`, isn't detected as a dry run.)
- doctest's `--minimal` prints no summary when every test passes, so the test targets run without
  it to count cases. A case's `MESSAGE` or `WARN` output prints a `TEST CASE:` block too, so only
  a block with `: ERROR:` (or `: FATAL ERROR:`) is a failure.

## Pre-push hook

`tools/git-hooks/pre-push` tests each pushed ref's commit on the machine's GPU (MoltenVK on the
Mac), covering what lavapipe can't. Install it once per checkout, for all its worktrees:

    git config core.hooksPath tools/git-hooks

It builds the pushed commit (`make -j build/tests/gpu_tests examples web`, summarized as `build`),
then runs `make -k test-gpu test-examples test-web BROWSERS="chromium firefox"` (Firefox opens
windows), printing only the summaries; any failure blocks the push. `git push --no-verify` skips
it. It skips with a notice a push that only deletes refs, commits the remote already has (under its
remote-tracking refs or the refs the push updates, e.g. a new branch at `main`), and a machine
without `vulkaninfo` or whose `vulkaninfo --summary` lists no non-CPU device.

It tests the pushed commit, not the pushing worktree's files: `tools/test-commit.sh` checks it out,
detached, into the checkout's own worktree `<git common dir>/zgal-pre-push` (`.git/zgal-pre-push`;
listed by `git worktree list`), kept between pushes so builds are incremental, and made again
when it can't be checked out (e.g. after the checkout moved). Its `build/logs`
hold the full output, and a failed screenshot test's received images stay in its
`tests/gpu/approved/<device>/`: look at them there and approve them in your own worktree.

It holds the machine-wide GPU lock while it builds and tests: `tools/gpu-lock.sh <command>` runs a
command holding an flock on `/tmp/zgal-gpu.lock` (released when its holder exits, however), and
waits, naming the holder, while another run has it. So pushes from several worktrees or checkouts
take turns; other GPU runs (`make test-gpu`, `verify_stack.sh`) don't take it. It isn't
re-entrant (a run taking it inside the hook's would wait for itself), so making them take it needs
a marker the outer run exports.

## Verifying a stack

`tools/verify_stack.sh <repo> <base> [head]` builds and tests every commit in a range (builds and
CPU and ZGAL tests in parallel; GPU tests, example smoke tests and web tests one at a time),
printing each commit's build and test summaries as the test targets do (older commits too, with
its own `run-test.sh`) and keeping the logs in the temporary directory it names:
`<dir>/<commit>/logs/<name>.log`, and for the make targets of commits with these summaries, their
full output in `<dir>/<commit>/build/logs/<target>.log`. Check its exit codes, not filtered
output. A branch CI has checked only needs the GPU, example and web
tests locally, which the [pre-push hook](#pre-push-hook) runs on its tip.

When folding fixes into earlier commits, note that `git commit --fixup=X` commits the whole index,
including earlier `git rm`s: pass the paths, then check each rewritten commit's tree.

## Verifying ports

Verify a port or refactor that shouldn't change output by pixel identity against captures taken
before the change: the example smoke tests write `build/examples/*/smoke.png`, and `image_diff`
(`examples/image_filter/tests/image_diff.cpp`, built by `make test-main`) compares two PNGs. Use it
rather than reading PNGs by hand, which needs their row filters decoded.

## Profiling

- Per-node GPU timings (a program's `--timings`) and their caveats are in
  [wrapper.md](wrapper.md#render-graphs). For a library node family's own cost, use
  `tools/bench --only K` (`make run-bench BENCH_ARGS='--only K'`).
- For a whole-frame view, record a Metal trace with
  `xcrun xctrace record --template 'Metal System Trace'` and export its `metal-gpu-intervals`
  table; the graph's node names are in the `metal-object-label` column.
- On the M4, per-tile workgroup barriers cost more than uncoalesced writes: measure before
  optimizing memory access patterns.
