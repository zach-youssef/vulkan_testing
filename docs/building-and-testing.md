# Building and testing

## Requirements

The Vulkan SDK (path in the Makefile), `glslc`, GLFW and GLM for the wrapper; Racket 8 (`racket`
and `raco`, with Scribble, as in the main distribution) for ZGAL; Node 24 (which runs TypeScript)
and npm for ZGAL's web backend. Development happens on an Apple M4 with MoltenVK
([wrapper.md](wrapper.md#the-development-device) has what that means for code).

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
  test` includes them). `make web` builds the web examples.
- `make run-bench`: times ZGAL's library kernels (`tools/bench`; see "Profiling").
- `zgal.mk`: the make rules building ZGAL programs (`zgal-rule`, `zgal-program`), which the Makefile
  and other projects' Makefiles include.

## GPU tests

GPU tests run headless on a shared `VulkanContext` with validation and synchronization validation
enabled. The `GpuTest` fixture (`tests/gpu/VkTestContext.h`) fails a test that produces validation
errors, leaks managed resources or `PerFrame` collections, or leaves the number of frames in
flight changed. Test shaders live in `tests/gpu/shaders`; tests also use the examples' shaders, and
`testShader(ctx, "name.comp")` loads them.

## Verifying a stack

`tools/verify_stack.sh <repo> <base> [head]` builds and tests every commit in a range (builds and
CPU and ZGAL tests in parallel; GPU tests, example smoke tests and web tests one at a time). Check
its exit codes, not filtered output.

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
