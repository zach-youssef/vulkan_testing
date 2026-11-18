# ZGAL's web backend

ZGAL compiles programs to WGSL and TypeScript for WebGPU (`--web-out`). What the backend generates,
and what doesn't work on the web, is in ZGAL's reference ("The web (WebGPU)" in
`build/docs/zgal.txt`, from `make docs`); its "Working on ZGAL" section maps the runtime's files.
This page has what the reference doesn't: the runtime's conventions and how to test on the web.

## The runtime (`web/zgal`)

- `model.ts` loads glTF and OBJ models with its own parsers, mirroring `include/resources/Model.h`.
- Image files (`image-file`, models' textures) are decoded by `image-decode.ts`'s `decodeImage`, in
  every environment, never by the browser: Firefox's `createImageBitmap` premultiplies alpha
  whatever it's asked, and browsers' decoders needn't give native's bytes
  ([the image decoder](#the-image-decoder)).
- `main-browser.ts`: `mountProgram` embeds a program in a page, and `runPage`, which a program's
  generated page calls, wraps it. A page's state is in `<html data-zgal="running|stopped|failed">`
  and errors appear in `#zgal-message` ([consumers.md](consumers.md) depends on both). For
  programs without a window, `runHeadless` runs one as under Node, giving its captures to a
  callback (each run has its own device), and `mountHeadless` builds a form for it from the
  program's option metadata (`ProgramOption`'s `value`, `default`, `range` and `output`, which
  `ts-main.rkt` emits). `main.ts`'s `runHeadlessFrames` is the frame loop both share with
  `main-node.ts`, and `parseCommandLine`'s `where` (`node`, `browser` or `canvas`) is what differs.
  A program's downloads end when it does, never holding it up: `stop()` aborts its context's loads
  (see `Context.signal` below) before waiting for anything, so it settles promptly even while an
  asset or a stream stalls, and a mounted program ending by itself (it quits, runs its `--frames`
  or fails) ends them, removes its listeners and frees its graphs at once, leaving only its device
  and the canvas's configuration (the last frame shown) for `stop()`. `runHeadless` (which
  `mountHeadless` runs) ends its loads when its frames end, however they end. Errors raised
  once `stop()` has begun (the loads it ended, but also any create or frame failure racing it) aren't
  failures: the state goes to `stopped`, as before.
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
  fail rather than quietly fetching from elsewhere. `Context.signal` is aborted when the context's
  loads end (`ContextOptions.signal` aborted, or `destroy()`): loaders get it (the defaults and
  pages' give it to `fetch`), a load waiting rejects at once whatever its loader does, later ones
  reject, and the streams `loadAssetStream` opened that are still downloading are cancelled with their
  sources (a finished one has nothing to cancel).
  `web/tests/streams.test.ts` tests streams under Node, and `web/tests/stop.test.ts` ending loads
  (a context's, and `runHeadless` stopped or quitting while a served stream stalls).
- `types.ts` encodes values at their std430 layout (`array`, `struct`), and at a uniform buffer's
  (`uniformArray`, `uniformStruct`: elements and structs 16 bytes apart), which a material
  description's push fields use when its push constants hold arrays or structs: the shaders then
  read a mirror laid out that way (`ZgalPushUniform`) and convert it to the std430 shape the body
  reads (`zgal_push()`; the ZGAL docs' web section). `web/tests/push.test.ts` checks every value
  arrives.
- A material description's `glsl` marks a material whose GLSL bodies glslc and naga translated
  (the ZGAL docs' GLSL bodies section): its bindings are one per ZGAL binding and its samplers
  WebGPU's own, so `MaterialInstance` sets no overridable constants for it and `MaterialBinder`
  refuses a sampler clamping to a border colour (WebGPU's have none).
- `png-write.ts` writes PNGs in any environment (`encodePngStream`, with `CompressionStream`);
  `png.ts` (Node only) uses zlib. Reading PNGs is the image decoder's.
- The web keeps Vulkan's clip space (the WGSL negates y), so web pixel expectations match native
  ones.

## Writing runtime code and tests

- Node's type stripping and `erasableSyntaxOnly` reject TypeScript parameter properties: assign
  fields explicitly. `Uint8Array<ArrayBuffer>` and `<ArrayBufferLike>` are different types, which
  bites `Blob` parts and `AssetLoader` returns; wrap a Node `Buffer` in `new Uint8Array(...)`
  before `deepStrictEqual`.
- A `ReadableStream` that may already have errored (e.g. an aborted fetch body wrapped by
  `readableStream`) makes a later `cancel()` reject with the abort reason: cancel it with a
  `.catch`.
- Under Node, `runHeadless` makes command-line paths absolute, so a test server sees
  `/abs/path/scene.bin`: match requests by suffix. In a page test, a `Blob` subclass overriding
  `stream()` gives `runHeadless` a slow or unbounded source.
- A program that has a window needs `--frames` to run headless, so adding a window to a test
  program breaks callers that don't pass it.
- WebGPU validation: a filtering `GPUSampler` in a non-filtering layout slot is a bind-group error
  on Dawn and Chromium. `textureGather` on a `texture_depth_2d` works with a non-filtering sampler
  everywhere (Dawn, SwiftShader, Firefox) and honours address modes.

## The image decoder

`image-decode.ts`'s `ImageDecoder` decodes image files with stb_image built to WebAssembly, so the
web gets exactly the RGBA8 bytes native `stbi_load(..., 4)` gives (PNG, JPEG, BMP, TGA, GIF, PSD, PIC
and PNM; HDR files fail, where native converts them). The runtime loads every image file with it.

- `web/decode/stb_image_wasm.c` builds the vendored `third_party/stb/stb_image.h` freestanding: no
  libc, a bump allocator reset after each decode, and the few functions stb_image calls, so the
  module (`build/web-decode/stb_image.wasm`, about 90 KB, built by `web/decode/decode.mk` and never
  committed) imports nothing. `STBI_NO_HDR` and `STBI_NO_LINEAR` leave out what needs libm, whose
  rounding needn't match native's.
- `ImageDecoder.load(url)` reads a `file:` URL under Node and fetches others; `ImageDecoder.create`
  takes the module's bytes or the compiled module. `decode(bytes)` throws stb_image's failure
  reason.
- `decodeImage(bytes)`, which `assets.ts` and `model.ts` call, finds the module at
  `zgal/stb_image.wasm` beside the runtime (`import.meta.url`), where `zgal.mk`'s
  `zgal-web-program` copies it into every program's output (`<out>/zgal/`, and `<out>/src/zgal/`
  for running the sources). It compiles the module once and instantiates it per decode, so a
  decoded image's memory goes with its instance. A program built without the wasm fails its first
  image load, naming the URL. Callers start `prepareImageDecoder()` before loading an image's
  bytes, so the module downloads alongside them, and pass `context.signal`, so waiting for it
  ends when the context's loads do.
- The tools (`web/tools/image-diff.ts`, `pages.ts`) decode with it too (`web/tools/images.ts`),
  from `ZGAL_DECODE_WASM` (which test-web and test-pages export as the build's) or
  `build/web-decode/stb_image.wasm`.
- Its compiler is wasi-sdk's clang, pinned and downloaded as
  [building-and-testing.md](building-and-testing.md#requirements) says.
- `make test-decode` (Node, no GPU) decodes the repository's PNGs and JPEG, BMP and TGA files it
  makes, and damaged files, with the wasm and with `web/decode/tests/stb_dump.cpp` (native
  `stbi_load`), and requires the same bytes or the same failure. CI's runner is x86-64, so the
  native helper takes stb_image's SSE2 JPEG path, which CI checks too.
- `make test-web` checks the runtime with it under Node: `assets.test.ts` loads a JPEG's texels
  and compares them with `stb_dump`'s, and image_filter runs on a JPEG.
- `make test-pages` loads it in browsers: `web/tests/image-decode.html` decodes the JPEG and two
  PNGs with the runtime's `decodeImage` and checks them against native decodes `stb_dump` saves
  beside them, and `image-file.html` checks a JPEG's texels likewise.

## Tests

`make test-web` (`TEST_ARGS` go to each `node --test` run, test-decode's included, so a name pattern
selects from all of them) runs `make test-decode` ([the image decoder](#the-image-decoder)) first,
then:

1. compiles `web/tests/*.zgal` (and the image_filter example, which `image-filter.test.ts` runs
   without the `texture-formats-tier2` feature, and `tests/stream/streamer.zgal`, which
   `stop.test.ts` runs) to TypeScript and WGSL in `build/web-tests`, beside
   a copy of the runtime, type checks it and runs `web/tests/*.test.ts` on Dawn under Node;
2. builds the particles, image_filter and scene examples for the web (`make web`, into
   `build/web/<name>`) and runs them under Node, comparing image_filter's outputs with
   `web/tools/image-diff.ts` (which compares images as `image_diff.cpp` does; image_filter also
   copies a JPEG, compared with the JPEG), and runs
   `tests/main/painter.zgal` as `make test-main` runs it natively (its TypeScript giving its
   `MainInputs`), `tests/border/border.zgal` (samplers clamping to a border, sampled by hand past
   the edges) compared with `border.approved.png`, captured natively by `make test-main`, and
   `tests/stream/streamer.zgal` streaming a file (its opener in
   `streamer.inputs.ts`) (`headless.test.ts` needs no GPU: the PNG encoders and
   `parseCommandLine`'s `where`);
3. runs `make test-pages`: the particles and scene pages, image_filter's page (run from its query
   string, its capture saved with the checkers' `--save-capture` and compared with
   `blur_edges.approved.png`), the border program's page (its capture compared likewise with
   `border.approved.png`), `web/tests/mount.html` (mounts two programs and stops one),
   `web/tests/held-keys.html` (held keys and mouse buttons released once each, also when focus
   leaves, chorded buttons, cancelled pointers and lost captures, and a finger ignored while the
   mouse is pressed), `web/tests/touch.html` (in Chromium only, touched by
   `chromium.ts --touch`: a one-finger drag is a left-button drag, a second finger is ignored, and
   the canvas has `touch-action: none` until the program ends),
   `web/tests/image-file.html`, `web/tests/image-decode.html`, the kernels, indirect and models
   pages, `web/tests/headless.html` (`runHeadless` and the form, through image_filter's main, and
   `onProgress` for an asset read whole),
   `web/tests/stamps.html` (functions storing into the storage images they're given) and
   `web/tests/streams.html` (assets streamed from fetch bodies, and Blobs' streams),
   `web/tests/stream-buffers.html` (`tests/stream/streamer.zgal`'s stream-buffer filled from a
   served file and from a `Blob` through `runHeadless`, its failures, runs stopped mid-stream
   cancelling their streams, and `onProgress` with and without `Content-Length`),
   `web/tests/stop-streams.html` (streamer mounted on a fetch that stalls part way: `stop()` while
   it starts or runs settles at once and aborts the fetch, and quitting with Escape ends the fetch
   before any `stop()`),
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
- **Chromium** can touch a page's canvas (`--touch`, with the DevTools protocol's
  `Input.dispatchTouchEvent`), which gives the page real touch pointer events; firefox.ts refuses
  it. A page checking touches records what its app gets and checks it itself, as `touch.html` does.
- Check a page's outcome through `data-zgal` and `#zgal-message`, not only its pixels.
- Test pages need fixed layouts (`flex: none`): Firefox's window is 1024 pixels wide.
