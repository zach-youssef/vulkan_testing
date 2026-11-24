# ZGAL's web backend

ZGAL compiles programs to WGSL and TypeScript for WebGPU (`--web-out`). What the backend generates,
and what doesn't work on the web, is in ZGAL's reference ("The web (WebGPU)" in
`build/docs/zgal.txt`, from `make docs`); its "Working on ZGAL" section maps the runtime's files.
This page has what the reference doesn't: the runtime's conventions and how to test on the web.

## The runtime (`web/zgal`)

- `model.ts` loads glTF and OBJ models with its own parsers, mirroring `include/resources/Model.h`.
  A model's base colours are a texture per material (`Model.baseColors`, sRGB, one 1x1 white shared
  by materials without one), which `RenderGraph.importImages` imports as `GraphImage`s (a graph's
  `result.images.<base-colors>` is their array), as native's are a vector of images. The decoded
  textures stay alive beside the buckets an array copies them into, so a model's base colours take
  about twice their texture memory while they're bound as an array.
- An `(array texture)` binding is a `TextureArray` (`RenderGraph.textureArray(name, elements)`,
  the same object for the same elements) of `GraphImage`s, each `fixed` or `live` (`Texels`, which
  `importImage` and `importImages` require): when the graph binds it, it copies each distinct fixed
  image (image files, models' base colours: texels set before the graph binds them) once into a
  `texture_2d_array` per distinct size and format (a bucket), makes each distinct live image
  (graph images and imports, which passes or the application write) a bucket of its own, a
  one-layer `2d-array` view of its texture, and writes a uniform table of each element's bucket,
  layer and size. When an element's image is recreated (a surface-relative image after a resize),
  it rebuilds the views and the table, recopying fixed images only if one of them changed; live
  buckets come in the order their images first appear, so the bucket shape, and with it the
  shader, stays the same. The material description's WGSL carries `//zgal-array` and `//zgal-bucket` lines,
  which `material.ts` expands for the bound array's element count and buckets, so the shader and
  layout are made per bucket shape (the ZGAL docs' web section). `web/tests/models.test.ts` checks
  the buckets and the error past `maxSampledTexturesPerShaderStage`, which `Context` requests as
  high as the adapter allows.
- An `(array (storage-image))` binding is a `TextureArray` too, its buckets
  `texture_storage_2d_array<{format}, access>`s (the array's access; `{format}` each bucket's,
  which `expandArrays` fills in), and `store!`, `image-load` and `image-size` on an element switch
  on its bucket as sampling does. Its elements can only be graph images and imports (live: the
  compiler refuses image files, and `MaterialInstance` fixed elements), so each distinct image is a
  bucket of its own, viewing it, and stores reach the image. The buckets count against
  `maxStorageTexturesPerShaderStage` (requested as high as the adapter allows), checked with the
  error naming the array; read-write ones need `texture-formats-tier2` for most formats, as a
  single storage image does. `array-length` touches no element, so it doesn't make a written array
  read-write.
- An `(array (buffer T))` or `(array (buffer-array T))` binding is a `BufferArray`
  (`RenderGraph.bufferArray(name, elements)`, the same object for the same elements) of
  `GraphBuffer`s and `Uniform`s (a uniform element is its storage buffer, updated as the material's
  uniforms are). Each distinct buffer is a bucket of its own, a storage buffer of raw words, in the
  order buffers first appear (WebGPU forbids binding a writable storage buffer twice in a bind
  group, so duplicates share a bucket); the uniform table of each element's bucket is written when
  the array is made, as the graph is built. The WGSL repeats each bucket's load, store and atomic
  helpers on `//zgal-bucket` lines (one line each), and an element's access switches on its
  bucket. The buckets count against `maxStorageBuffersPerShaderStage` with the material's other
  buffers (requested as high as the adapter allows), checked with the error naming the arrays;
  `web/tests/models.test.ts` checks both. Arrays chosen with `#:select` stay refused.
- Every array binding's table, and a material's push constants, is a uniform buffer, counted
  against `maxUniformBuffersPerShaderStage` (requested as high as the adapter allows, 12 by
  default), checked with the error naming the arrays; `web/tests/models.test.ts` checks it.
- `MaterialInstance` refuses a buffer or image that two of a material's bindings bind (two plain
  bindings, an array's element and another binding, or an element of two arrays) where either
  writes it (a buffer or storage image not bound read-only), naming the material, the resource and
  both bindings. WebGPU forbids a writable storage buffer or storage texture aliasing another
  binding of a bind group, which Dawn reports only at the dispatch or draw ("Writable storage
  buffer binding aliasing", or a writable usage beside another "in the same synchronization
  scope"); natively the bindless heap allows it. An element repeated in one array is one bucket,
  so it's allowed, as is a resource every binding only reads. Resources compare by their GPU
  objects too, so two imports of one buffer or texture alias, as does an import of a graph
  resource's buffer or texture (each slot's copy of a per-frame one) beside that resource; an
  array of buffers' buckets are its distinct graph buffers, so two imports of one GPU buffer in a
  written array alias. Both bindings of a per-frame resource bind the frame slot's copy, so they
  alias in every slot; an array of textures' fixed elements are the array's copies, which alias
  nothing. `web/tests/models.test.ts` checks the cases.
- `web/tools/check-wgsl.ts` expands arrays' directive lines for two elements in two `rgba8unorm`
  buckets, so the golden modules with arrays compile.
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
- A material description's `glsl` marks a material glslc and naga translated, as it has GLSL
  bodies or its ZGAL bodies call GLSL functions (the ZGAL docs' GLSL bodies and extern-glsl
  sections): its bindings are one per ZGAL binding and its samplers
  WebGPU's own, so `MaterialInstance` sets no overridable constants for it and `MaterialBinder`
  refuses a sampler clamping to a border colour (WebGPU's have none). `web/tests/glsl.test.ts` runs
  such materials on Dawn, and `web/tests/glsl.html` in browsers; `web/tests/extern_glsl.test.ts`
  runs materials calling GLSL functions on Dawn.
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
- The naga CLI isn't on `PATH` (`make naga` builds it, see
  [building-and-testing.md](building-and-testing.md)). Dawn validates generated WGSL with Tint,
  not with naga's validator, which Firefox's front end is, so running naga on a generated `.wgsl`
  file is a cheap Firefox check.
- `tsc -p web` in the source tree fails on tests importing generated modules, which exist only
  beside the runtime's copy in `build/web-tests`: type check through `make test-web`'s layout.
- Generated WGSL: naga (Firefox, and `--naga`) rejects a function with a return type whose body
  ends in a `loop` (it sees the end of the function reachable), so helpers `break` out of the loop
  and return after it.
- Storage buffers per stage: WebGPU's default `maxStorageBuffersPerShaderStage` is 8 (`Context`
  requests the adapter's, 10 on Dawn on Metal); an array of buffers takes one per distinct
  buffer, so keep test programs within 8.
- Dawn rejects `STORAGE_BINDING` on a `bgra8unorm` texture unless the device has
  `bgra8unorm-storage` (the graph only adds the usage then): storage-image tests use `rgba8unorm`
  or `r32float`.

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

- Frames: `Context` holds the frames in flight (`framesInFlight`; `setFramesInFlight` applies at
  the next frame boundary, `Context.advanceFrame`, which a graph's `execute()` calls once it has
  submitted) and the frame slot (`frameIndex`), as natively `VulkanContext`: a count change
  restarts the slots at 0. `GraphImage`s and `GraphBuffer`s are `shared` or `per-frame` (`Frames`,
  which `createImage` and `createBuffer` require): per-frame ones hold a copy per frame in flight
  (`textures`, `buffers`; `textureOf`, `bufferOf` and `viewOf` give the frame slot's), keeping,
  dropping and adding copies as the count changes (new buffers zeroed), as native `PerFrame`
  does. A `MaterialBinder` keeps a bind group per slot when its material binds a per-frame
  resource, and a `BuiltArray` its bucket views per slot when an element is per-frame (the
  buckets and table are the same for every slot); a `BufferArray`'s buckets are its distinct
  `GraphBuffer`s, each slot's bind group binding the slot's copy of the per-frame ones, under the
  one table. WebGPU's queue runs frames in order anyway, so
  the copies only give a frame reading one before writing it what it reads natively; uniforms,
  uploads of per-frame data, streams and the surface's depth image stay shared.

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
   the edges) compared with `border.approved.png`, captured natively by `make test-main`,
   `tests/arrays/arrays.zgal` (an array of textures of mixed sizes and colour spaces) compared
   exactly with `arrays.approved.png`, likewise, `tests/arrays/targets.zgal` (an array of two
   surface-relative images one pass writes and the next fetches and samples, recreated by
   `resize!` in the frame handler, run for 3 frames) compared exactly with `targets.approved.png`,
   `tests/arrays/storage.zgal` (arrays of storage images of mixed sizes and formats, one
   surface-relative, a dispatch stores to and the next loads from, run for 3 frames) compared
   exactly with `storage.approved.png`,
   `tests/frames/frames.zgal` (a per-frame buffer and image read before they're written, as
   `set-frames-in-flight!` changes 2 to 3 frames in flight and 3 to 2) compared exactly with
   `frames-2-3.approved.png` and `frames-3-2.approved.png`, `tests/arrays/buffers.zgal` (arrays
   of uniforms and buffers a dispatch reads and writes, 16-bit integers and atomics, each
   invocation's element its row's, and the next reads, run for 3 frames) compared exactly with
   `buffers.approved.png`, and
   `tests/stream/streamer.zgal` streaming a file (its opener in
   `streamer.inputs.ts`) (`headless.test.ts` needs no GPU: the PNG encoders and
   `parseCommandLine`'s `where`);
3. runs `make test-pages`: the particles and scene pages, image_filter's page (run from its query
   string, its capture saved with the checkers' `--save-capture` and compared with
   `blur_edges.approved.png`), the border program's page (its capture compared likewise with
   `border.approved.png`), the arrays, targets, storage, frames and buffers programs' pages
   (compared exactly with `arrays.approved.png`, `targets.approved.png`, `storage.approved.png`,
   both frames captures and `buffers.approved.png`),
   `web/tests/mount.html` (mounts two programs and stops one),
   `web/tests/held-keys.html` (held keys and mouse buttons released once each, also when focus
   leaves, chorded buttons, cancelled pointers and lost captures, and a finger ignored while the
   mouse is pressed), `web/tests/touch.html` (in Chromium only, touched by
   `chromium.ts --touch`: a one-finger drag is a left-button drag, a second finger is ignored, and
   the canvas has `touch-action: none` until the program ends),
   `web/tests/image-file.html`, `web/tests/image-decode.html`, the kernels, indirect and models
   pages, `web/tests/headless.html` (`runHeadless` and the form, through image_filter's main, and
   `onProgress` for an asset read whole),
   `web/tests/stamps.html` (functions storing into the storage images they're given),
   `web/tests/glsl.html` (`web/tests/glsl.zgal`'s GLSL bodies, as naga translated them) and
   `web/tests/streams.html` (assets streamed from fetch bodies, and Blobs' streams),
   `web/tests/stream-buffers.html` (`tests/stream/streamer.zgal`'s stream-buffer filled from a
   served file and from a `Blob` through `runHeadless`, its failures, runs stopped mid-stream
   cancelling their streams, and `onProgress` with and without `Content-Length`),
   `web/tests/stop-streams.html` (streamer mounted on a fetch that stalls part way: `stop()` while
   it starts or runs settles at once and aborts the fetch, and quitting with Escape ends the fetch
   before any `stop()`),
   in each browser of `BROWSERS`: by default only headless Chromium. Firefox, also headless but on
   the machine's GPU (lavapipe on Linux), runs only when asked, `make test-web BROWSERS="chromium
   firefox"` (as CI and the pre-push hook do). A browser found nowhere is skipped (Chromium is
   Playwright's headless shell, Firefox the macOS application or `firefox` on `PATH`), with a
   `note:` under the summary, unless `make` was given `BROWSERS`: then it fails, as it does when the
   browser `CHROMIUM` or `FIREFOX` names is missing.

Captures compared exactly across GPUs need texels whose values every GPU rounds alike: an sRGB
texel decodes to a fraction of 255 that, near x.5, rounds differently on different GPUs (sRGB 100
decodes to 32.497, which SwiftShader and MoltenVK round apart), so the arrays tests' sRGB texels
avoid such values, and their written texels are whole multiples of 1/255, which unorm8 stores
exactly.

A texture array's bucket views carry their texture's label, so unit tests can check which
texture or bucket a view is of (`viewOf`'s views have none). `bufferOf` adds missing copies only
when a slot is past the end, while `executeFrame` adds every copy at the start of a frame, so a
unit test calling `bindGroup` outside `execute` should check copy counts after visiting every
slot.

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

- **Firefox** runs headless (`--window` opens one) with `layers.gpu-process.allow-headless` and
  `gfx.webgpu.ignore-blocklist` set in its profile. Headless, Firefox otherwise blocks its GPU
  process, and on macOS and Windows WebGPU is disabled without `allow-headless` (the gfx feature
  log says `FEATURE_WEBGPU_NO_GPU_PROCESS`), so `requestAdapter()` finds nothing; Linux doesn't
  need it. `dom.webgpu.allow-in-parent` would also work on the Mac, but runs WebGPU in the parent
  process, unlike Firefox with a window, and doesn't help on Linux. On Linux Firefox blocklists
  the CPU adapter lavapipe, so without `ignore-blocklist` `requestAdapter()` finds nothing; with
  it Firefox gets lavapipe through wgpu's Vulkan backend (`isFallbackAdapter` true, empty info
  strings), and no adapter at all without a Vulkan driver. The Mac's GPUs aren't blocklisted, so
  it changes nothing there. CI runs the pages in Mozilla's release tarball, version and SHA-256
  pinned in web.yml. Its startup can flake while the user's
  own Firefox is launching; rerun before investigating. `MOZ_LOG` for WebGPU writes empty files in
  release Firefox, so adapter problems are diagnosed from `requestAdapter()` and the gfx feature
  log. In web.yml, bash steps run with `-e -o pipefail`: a loop over pages that carries on past
  failures needs `cmd || status=$?`.
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
- Test pages need fixed layouts (`flex: none`): headless Firefox's window is 1024 pixels wide
  (firefox.ts sets it through `MOZ_HEADLESS_WIDTH` and `MOZ_HEADLESS_HEIGHT`, as Firefox ignores
  `--window-size` there). With `--window` it may ignore `--window-size` too (1280 wide on a Mac).
