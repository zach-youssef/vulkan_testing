# vulkan_testing

[![ci](https://github.com/zach-youssef/vulkan_testing/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/zach-youssef/vulkan_testing/actions/workflows/ci.yml?query=branch%3Amain)

A header-only C++20 Vulkan 1.3 wrapper, and ZGAL (Zach's Graphics Application Language), a language
for writing GPU programs whole: shaders, the render graph that orders and synchronizes them, and the
app that feeds them input, all in one file. ZGAL is embedded in Racket, and compiles a program to
GLSL and C++ for the wrapper, or to WGSL and TypeScript for WebGPU.

- [ZGAL's documentation](https://zyoussef.com/zgal/docs/), which `make docs` also builds into
  `build/docs/zgal.html`
- [The particles example running in the browser](https://zyoussef.com/projects/zgal), beside its
  source (needs WebGPU)

## Building

`make` builds the examples (`examples/`) into `build/examples/<name>/`, `make web` builds the
particles, image_filter and scene examples for the web, and `make test` runs every test. Building needs the Vulkan SDK,
`glslc`, GLFW, GLM, Racket 8 with Scribble, and Node 24 with npm. [`docs/`](docs/) describes the
targets, the wrapper and ZGAL's development in detail
([building and testing](docs/building-and-testing.md), [the wrapper](docs/wrapper.md)); CI runs on
Linux with lavapipe, and a pre-push hook on the Mac's GPU ([CI](docs/building-and-testing.md#ci)).
