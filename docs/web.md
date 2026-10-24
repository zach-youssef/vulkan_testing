# ZGAL's web backend

ZGAL compiles programs to WGSL and TypeScript for WebGPU (`--web-out`). What the backend generates,
and what doesn't work on the web, is in ZGAL's reference ("The web (WebGPU)" in
`build/docs/zgal.txt`, from `make docs`); its "Working on ZGAL" section maps the runtime's files.
This page has what the reference doesn't: the runtime's conventions and how to test on the web.

## The runtime (`web/zgal`)

- `model.ts` loads glTF and OBJ models with its own parsers, mirroring `include/resources/Model.h`.
- `png-pixels.ts` reads PNGs in any environment: Firefox's `createImageBitmap` premultiplies alpha
  whatever it's asked, so pixels can't come from it.
- `main-browser.ts`: `mountProgram` embeds a program in a page, and `runPage`, which a program's
  generated page calls, wraps it. A page's state is in `<html data-zgal="running|stopped|failed">`
  and errors appear in `#zgal-message` ([consumers.md](consumers.md) depends on both).
- The web keeps Vulkan's clip space (the WGSL negates y), so web pixel expectations match native
  ones.

## Tests

`make test-web` (`TEST_ARGS` go to `node --test`):

1. compiles `web/tests/*.zgal` to TypeScript and WGSL in `build/web-tests`, beside a copy of the
   runtime, type checks it and runs `web/tests/*.test.ts` on Dawn under Node;
2. builds the particles, image_filter and scene examples for the web (`make web`, into
   `build/web/<name>`) and runs them under Node, comparing image_filter's outputs with
   `web/tools/image-diff.ts` (which compares PNGs as `image_diff.cpp` does), and runs
   `tests/main/painter.zgal` as `make test-main` runs it natively (its TypeScript giving its
   `MainInputs`);
3. runs `make test-pages`: the particles and scene pages, `web/tests/mount.html` (mounts two
   programs and stops one), `web/tests/image-file.html` and the kernels, indirect and models pages,
   in each browser of `BROWSERS`: by default only headless Chromium. Firefox opens windows, so it
   runs only when asked, `make test-web BROWSERS="chromium firefox"` (as before pushing a stack's
   top), and has no WebGPU on Linux (CI skips it). A browser found nowhere is skipped (Chromium is
   Playwright's headless shell, Firefox the macOS application or `firefox` on `PATH`), but one
   `CHROMIUM` or `FIREFOX` names must exist.

`web/tools/check-wgsl.ts` compiles WGSL with Dawn.

## Running pages in browsers

`web/tools/firefox.ts` and `web/tools/chromium.ts` run a page (`FILE.zgal.html`), print its console
and messages, and compare its canvas with an approved PNG; `web/tools/pages.ts` has what they share.
Their header comments list the options (`--serve DIR`, `--compare FILE`, ...). Both exit with 0 when the
page ran (and matched), 1 when it failed (or the browser `--chromium`/`CHROMIUM` or
`--firefox`/`FIREFOX` names doesn't exist), 2 for a bad command line and 3 when they found no
browser where they look by default.

- **Firefox** needs a window: headless, it has no WebGPU adapter. Its startup can flake while the
  user's own Firefox is launching; rerun before investigating.
- **Chromium** runs headless on SwiftShader, the same on macOS and Linux. Canvases of a SwiftShader
  device need all of `--use-angle=swiftshader --enable-features=Vulkan --use-vulkan=swiftshader`:
  without the Vulkan ones they fail ("A valid external Instance reference no longer exists"), and
  with Vulkan on the system's driver (lavapipe) they stay blank, so check screenshots too.
  `--hardware` uses the GPU (Metal) instead, without those switches (`--use-angle=swiftshader`
  overrides `--enable-gpu`). `navigator.gpu` needs a secure context, which `--serve`'s `127.0.0.1`
  is. Dawn's npm package, which runs the Node tests, uses the machine's GPU (Metal on the Mac), or
  lavapipe where that's the only Vulkan driver (in CI).
- Check a page's outcome through `data-zgal` and `#zgal-message`, not only its pixels.
- Test pages need fixed layouts (`flex: none`): Firefox's window is 1024 pixels wide.
