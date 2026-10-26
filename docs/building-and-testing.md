# Building and testing

## Requirements

The Vulkan SDK, `glslc`, GLFW and GLM for the wrapper; Racket 8 (`racket` and `raco`, with
Scribble, as in the main distribution) for ZGAL; Node 24 (which runs TypeScript) and npm for ZGAL's
web backend. Development happens on an Apple M4 with MoltenVK
([wrapper.md](wrapper.md#the-development-device) has what that means for code).

`zgal.mk` finds the Vulkan headers and loader in `$VULKAN_SDK` when it's set (the SDK's
`setup-env.sh` sets it), else in the system's paths: on macOS `/usr/local` (where the SDK's
installer puts them) and Homebrew's for GLFW and GLM; on Linux the default ones (e.g. Ubuntu's
`libvulkan-dev`, `libglfw3-dev`, `libglm-dev`, plus `mesa-vulkan-drivers` for lavapipe). The
compiler is clang, the one tested (`c++` on macOS, with libc++; `clang++` on Linux, with
libstdc++), unless `CXX` is given (`make CXX=g++`).

## Make targets

- `make`: builds the examples into `build/examples/<name>/` (shaders into
  `build/examples/<name>/shaders`).
- `make shaders`: rebuilds the examples' SPIR-V (running examples reload it).
- `make test-cpu`, `make test-gpu`, `make test`: build and run the tests (doctest). Forward
  arguments with `TEST_ARGS`, e.g. `make test-gpu TEST_ARGS='-tc="RenderGraph*"'`.
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
  (Firefox opens windows). `make web` builds the web examples.
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
- ZGAL graph buffers start with undefined contents: MoltenVK's happen to be zero, lavapipe's
  aren't. A test graph must `clear` what it reads before anything writes it.

## CI

GitHub Actions (`.github/workflows/ci.yml`) checks every pushed commit, not only the tip: the
commits a working branch has beyond `main`, or those a push to `main` brings (at most the newest
20). Each one is built on Ubuntu 24.04 with clang++ and lavapipe (`make examples` and both test
programs), then runs `make test-cpu` and `make test-dsl`, and gets a `cpu` commit status. Each one also
runs `make test-web BROWSERS=chromium` (`.github/workflows/web.yml`): Dawn under Node on lavapipe,
and the pages in Playwright's Chromium headless shell on SwiftShader (`CHROMIUM` names it, so a
missing one fails), and gets a `web` status. Firefox is skipped there, as it has no WebGPU on
Linux; commits from before `web.yml` existed get no `web` check (the commits job leaves them out).
Each one also runs `make test-gpu` and `make test-examples` on lavapipe
(`.github/workflows/gpu.yml`), with validation and synchronization validation required, and gets
a `gpu` status (commits from before `gpu.yml` existed get none). The toolchain (apt packages, the
LunarG SDK, ccache, Racket, Node) comes from `.github/actions/setup-zgal`, whose `toolchain` input
is `native` or `web` (no SDK, clang or ccache).

lavapipe has one queue in one family, so CI never runs the multi-queue paths (async compute,
cross-queue ownership and timeline waits; their tests skip), nor the windowed tests (no display),
nor the web pages in Firefox: run `make test-gpu test-examples` locally on the Mac too, one GPU run
at a time on a machine.

lavapipe's screenshots are approved in `tests/gpu/approved/llvmpipe/` (its device name without the
LLVM build). A failed gpu job uploads its received screenshots, FLIP error maps, the examples'
captures and the logs as the artifact `gpu-<sha>`: `gh run download <run id> -n gpu-<sha>`, look at
each received image, and only then copy it over its `.approved.png`.

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

## Verifying a stack

`tools/verify_stack.sh <repo> <base> [head]` builds and tests every commit in a range (builds and
CPU and ZGAL tests in parallel; GPU tests, example smoke tests and web tests one at a time). Check
its exit codes, not filtered output. A branch CI has checked only needs the GPU, example and web
tests locally.

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
