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
  and errors appear in `#zgal-message` ([consumers.md](consumers.md) depends on both). For
  programs without a window, `runHeadless` runs one as under Node, giving its captures to a
  callback (each run has its own device), and `mountHeadless` builds a form for it from the
  program's option metadata (`ProgramOption`'s `value`, `default`, `range` and `output`, which
  `ts-main.rkt` emits). `main.ts`'s `runHeadlessFrames` is the frame loop both share with
  `main-node.ts`, and `parseCommandLine`'s `where` (`node`, `browser` or `canvas`) is what differs.
- `stream.ts` has streams (`Stream`), bytes a program reads as they arrive (a stream-buffer's file),
  with native `zgal::Stream`'s semantics. `read(maxBytes)` never waits: it takes what has arrived
  (an empty array if nothing has), and gives null once the stream has ended and been read.
  `ready(minBytes)` resolves once that many bytes are waiting or the stream has ended. A source that
  fails (or gives chunks other than `Uint8Array`s) rejects `ready` and makes every later `read`
  throw, its message prefixed with the stream's name, even with bytes from before the failure
  waiting (the graph then fails, as natively with InvalidData). A `StreamOpener`,
  `(path, context) => Promise<Stream>`, rejects when it can't open, failing the build as a missing
  buffer-file does. Every reference to a stream shares its read position, as copies of a native one
  do. `readableStream(source, name)` wraps any `ReadableStream` of bytes, reading it as fast as it
  gives bytes and keeping them until they're read (a stream-buffer's buffer holds the whole stream
  anyway). `cancel()` (required of every `Stream`) stops one: waiting bytes are dropped, waits
  reject, later reads throw and the source, unless it has ended, is cancelled; graphs cancel their streams when destroyed
  (so a page stopped mid-stream stops fetching) and when one fails. `peekStream(stream, count)`
  reads a stream's first bytes and gives them back with a stream replaying them before the rest
  (its first read may give them and more, so a first frame can take more than the header): for an
  opener that checks a header (e.g. its magic) before a stream-buffer reads the stream whole.
  `openStream` is what a graph's build does for a stream-buffer, as native `zgal::openStream`: the
  opener gets the path unresolved, its failure rejects the build, and a short or failing header
  leaves the header zeroed and the stream failing the first frame.
- `RenderGraph.createStreamBuffer(name, stream, words, bytesPerFrame)` is a stream-buffer's buffer,
  with native `RenderGraph::createStreamBuffer`'s semantics: before its nodes each frame writes
  (`queue.writeBuffer`) at most `bytesPerFrame` bytes of what has arrived, whole words only, and a
  stream that fails, gives too much or ends early fails that frame and every later one, with native
  messages. Unlike native, a frame never waits for the stream: it takes what has arrived, possibly
  nothing. `readStreams()` takes the coming frame's bytes once per frame (`execute()` calls it), so
  an app calls it in `beginFrame()` and compares `streamArrived()` with the bytes its last
  `streamed` event gave, before the frame handler, as native apps do; a graph not drawn reads again
  only when next drawn (natively an idle graph keeps topping up, as its epoch is the context's frame
  number). A frame failing after its writes (e.g. recording) keeps them; one failing before them
  (e.g. sizing a buffer) leaves its bytes for the next frame that writes. A buffer larger than the device's `maxBufferSize` fails its first frame
  (the app's 4 GiB refusal at `create()` comes first). `web/tests/streaming.test.ts` tests stream
  buffers with uneven chunks and the generated streamed events under Node.
- `context.loadAssetStream(path)`, the streaming sibling of `loadAsset`, opens an asset through
  `ContextOptions.assetStreams`: browsers stream fetch bodies (and the `Blob`s given to
  `runHeadless`), and `createNodeContext` streams the file, opened (in the first asset directory
  that has it) before `loadAssetStream` resolves. Without `assets` or `assetStreams`,
  `Context.create` fetches both from `assets/` beside the page; given only one, the other's loads
  fail rather than quietly fetching from elsewhere. `web/tests/streams.test.ts` tests streams under
  Node.
- `png-write.ts` writes PNGs in any environment (`encodePngStream`, with `CompressionStream`);
  `png.ts` (Node only) uses zlib.
- The web keeps Vulkan's clip space (the WGSL negates y), so web pixel expectations match native
  ones.

## The image decoder

`image-decode.ts`'s `ImageDecoder` decodes image files with stb_image built to WebAssembly, so the
web gets exactly the RGBA8 bytes native `stbi_load(..., 4)` gives (PNG, JPEG, BMP, TGA, GIF, PSD, PIC
and PNM; HDR files fail, where native converts them). Image loading doesn't use it yet.

- `web/decode/stb_image_wasm.c` builds the vendored `third_party/stb/stb_image.h` freestanding: no
  libc, a bump allocator reset after each decode, and the few functions stb_image calls, so the
  module (`build/web-decode/stb_image.wasm`, about 90 KB, built by `web/decode/decode.mk` and never
  committed) imports nothing. `STBI_NO_HDR` and `STBI_NO_LINEAR` leave out what needs libm, whose
  rounding needn't match native's.
- `ImageDecoder.load(url)` reads a `file:` URL under Node and fetches others; `ImageDecoder.create`
  takes the module's bytes or the compiled module. `decode(bytes)` throws stb_image's failure
  reason.
- Its compiler is wasi-sdk's clang, pinned and downloaded as
  [building-and-testing.md](building-and-testing.md#requirements) says.
- `make test-decode` (Node, no GPU) decodes the repository's PNGs and JPEG, BMP and TGA files it
  makes, and damaged files, with the wasm and with `web/decode/tests/stb_dump.cpp` (native
  `stbi_load`), and requires the same bytes or the same failure. CI's runner is x86-64, so the
  native helper takes stb_image's SSE2 JPEG path, which CI checks too.
- `make test-pages` loads it in browsers: `web/tests/image-decode.html` fetches the wasm and checks
  the JPEG and two PNGs against native decodes `stb_dump` saves beside them.

## Tests

`make test-web` (`TEST_ARGS` go to each `node --test` run, test-decode's included, so a name pattern
selects from all of them) runs `make test-decode` ([the image decoder](#the-image-decoder)) first,
then:

1. compiles `web/tests/*.zgal` (and the image_filter example, which `image-filter.test.ts` runs
   without the `texture-formats-tier2` feature) to TypeScript and WGSL in `build/web-tests`, beside
   a copy of the runtime, type checks it and runs `web/tests/*.test.ts` on Dawn under Node;
2. builds the particles, image_filter and scene examples for the web (`make web`, into
   `build/web/<name>`) and runs them under Node, comparing image_filter's outputs with
   `web/tools/image-diff.ts` (which compares PNGs as `image_diff.cpp` does), and runs
   `tests/main/painter.zgal` as `make test-main` runs it natively (its TypeScript giving its
   `MainInputs`), and `tests/stream/streamer.zgal` streaming a file (its opener in
   `streamer.inputs.ts`) (`headless.test.ts` needs no GPU: the PNG encoders and
   `parseCommandLine`'s `where`);
3. runs `make test-pages`: the particles and scene pages, image_filter's page (run from its query
   string, its capture saved with the checkers' `--save-capture` and compared with
   `blur_edges.approved.png`), `web/tests/mount.html` (mounts two programs and stops one),
   `web/tests/held-keys.html` (held keys and mouse buttons released once each, also when focus
   leaves),
   `web/tests/image-file.html`, `web/tests/image-decode.html`, the kernels, indirect and models
   pages, `web/tests/headless.html` (`runHeadless` and the form, through image_filter's main),
   `web/tests/stamps.html` (functions storing into the storage images they're given) and
   `web/tests/streams.html` (assets streamed from fetch bodies, and Blobs' streams),
   `web/tests/stream-buffers.html` (`tests/stream/streamer.zgal`'s stream-buffer filled from a
   served file and from a `Blob` through `runHeadless`, its failures, and runs stopped mid-stream
   cancelling their streams),
   in each browser of `BROWSERS`: by default only headless Chromium. Firefox opens windows, so it
   runs only when asked, `make test-web BROWSERS="chromium firefox"` (as before pushing a stack's
   top), and has no WebGPU on Linux (CI skips it). A browser found nowhere is skipped (Chromium is
   Playwright's headless shell, Firefox the macOS application or `firefox` on `PATH`), with a
   `note:` under the summary, unless `make` was given `BROWSERS`: then it fails, as it does when the
   browser `CHROMIUM` or `FIREFOX` names is missing.

Not every device has WebGPU's optional features: Dawn on lavapipe and Chromium's SwiftShader lack
`texture-formats-tier2`, so programs that should run everywhere (the examples) don't bind
half-float storage images read-write. `Context.create`'s `withoutFeatures` lets a test check a
program on a device without a feature its own adapter has.

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
- **Firefox** compiles WGSL with naga, not Tint, which differs from Dawn and Chromium both ways: it
  refuses storage textures as function parameters (why ZGAL inlines functions taking `image2D`s),
  but has no uniformity analysis and lets depth textures use filtering samplers. Passing in Firefox
  doesn't prove a page portable.
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
