# ZGAL

ZGAL (Zach's Graphics Application Language) declares materials, their shaders, the render graphs
using them and the apps running a graph, in `.zgal` files that compile to C++ and GLSL for the
wrapper ([wrapper.md](wrapper.md)) or to TypeScript and WGSL for WebGPU ([web.md](web.md)). Its
compiler is Racket (`dsl/`).

## The reference

ZGAL's reference is `build/docs/zgal.txt` (from `make docs`; also `build/docs/zgal.html`, and
online at <https://zyoussef.com/zgal/docs/>), built from `dsl/docs/zgal.scrbl`. Read it before
writing ZGAL or working on the compiler: it has the language, what's generated for each form, the
command line and Makefile rules, the web backend, every error, the limitations, and a "Working on
ZGAL" section with the compiler's file map, how to keep the reference accurate and what each test
covers. This page holds only what isn't there.

## Writing ZGAL

- Prefer ZGAL for new applications; every example uses it. Prefer a program (`(main ...)`, built by
  `zgal.mk`'s `zgal-program`) to a handwritten `main`, the standard library to your own functions,
  and loops of nodes to `extern` nodes where ZGAL has the nodes.
- Never edit generated files (under `build/`): change the `.zgal` file (or a GLSL body).
- Check a name before using it. Many short names are reserved (GLSL and C++ keywords and built-ins:
  `step`, `log`, `sample`, `out`, `main`, ...), and a file's name is its C++ namespace, so it must
  be a valid name too. `#f` means usable:
  ```sh
  racket -e '(require (file "dsl/private/names.rkt")) (for ([n (list "out" "amount")]) (printf "~a: ~a\n" n (reserved-name-reason n)))'
  ```
- Variables of a `(range ...)` over literals are `int`s; compare them with `uint`s through
  `(uint d)`.
- Targets' colour attachments clear by default (the reference's `target`), so each of several draws
  into one target, looped or not, clears what the others drew unless it loads:
  `(color image #:load load)`.

## Working on the compiler

- Changes to generated C++ or GLSL need GPU tests (`tests/gpu/ZgalTest.cpp`, with interfaces in
  `tests/gpu/zgal`), besides the golden files. Changes to generated WGSL or TypeScript, or to the
  runtime (`web/zgal`), need web tests (`web/tests`, `make test-web`).
- The compute library's materials (`dsl/lib/kernels`) are templates: `(%if flag ...)` and
  `(%unless flag ...)` choose forms by flags. Each kernel material is instantiated with and without
  `subgroups`; the variant without (`program-fallbacks` in `dsl/private/ts.rkt`, the web
  description's `withoutSubgroups`) runs on WebGPU devices lacking subgroups, and
  `ContextOptions.withoutFeatures` forces it in `web/tests/kernels.test.ts`.
- `raco test` runs with the working directory set to the test file's directory, so golden files'
  `#line` paths read `../lib/...`.
- After changing a struct in `dsl/private/ast.rkt`, stale `.zo` files fail with
  `instantiate-linklet: mismatch`: run `raco make dsl/tests/*.rkt`.
- Don't name a Racket local after a form (`when`, `values`, `match`): later uses of the form
  silently become function applications.
