# vulkan_test

A header-only C++20 Vulkan 1.3 wrapper (`include/`), ZGAL, a language compiling shaders, render
graphs and apps to it and to WebGPU (`dsl/`, with its web runtime in `web/`), examples
(`examples/`), tests (`tests/`) and tools (`tools/`).

Read the docs for the area you touch:

- [docs/building-and-testing.md](docs/building-and-testing.md): before building, testing, verifying a stack or profiling.
- [docs/wrapper.md](docs/wrapper.md): before writing an application with the wrapper or changing `include/`.
- [docs/zgal.md](docs/zgal.md): before writing ZGAL or changing its compiler (`dsl/`); it points to the reference.
- [docs/web.md](docs/web.md): before changing the web backend (`web/`) or testing pages in browsers.
- [docs/consumers.md](docs/consumers.md): before changing public APIs, web output, page markup or the docs.
- [docs/MIGRATING.md](docs/MIGRATING.md): when porting code written against the pre-refactor wrapper.
