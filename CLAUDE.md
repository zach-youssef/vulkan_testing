# vulkan_test

A header-only C++20 Vulkan 1.3 wrapper (`include/`), examples (`examples/`) and its tests
(`tests/`). This file is for agents building applications with the wrapper or working on it.
Agents migrating code written against the pre-refactor wrapper (the `main` branch API, e.g. in
`~/code/ppu`) should read `docs/MIGRATING.md` first.

## Building and testing

- `make`: builds the examples into `build/examples/<name>/` (shaders into `build/examples/<name>/shaders`)
- `make shaders`: rebuilds the examples' SPIR-V (running examples reload it)
- `make test-cpu`, `make test-gpu`, `make test`: build and run the tests (doctest). Forward
  arguments with `TEST_ARGS`, e.g. `make test-gpu TEST_ARGS='-tc="RenderGraph*"'`.
- `make test-examples`: runs every example headless with validation and
  synchronization validation; any message fails it. `make test-example-<name>` runs one.
- `make approve-screenshots`: accept the screenshots of the last failed GPU run
  (`tests/gpu/approved/<device>/`). Never approve a changed screenshot without understanding why
  it changed.
- `make test-dsl`: builds ZGAL's documentation and runs its compiler's tests (`make test` includes
  it). `make docs` only builds the documentation: `build/docs/zgal.html`, and
  `build/docs/zgal.txt` to read in a terminal.
- `tools/verify_stack.sh <repo> <base> [head]`: builds and tests every commit in a range (builds and
  CPU and ZGAL tests in parallel, GPU tests and example smoke tests one at a time). Check its exit
  codes, not filtered output.

Requirements: the Vulkan SDK (path in the Makefile), `glslc`, GLFW, GLM and Racket 8 (`racket` and
`raco`, with Scribble, as in the main distribution) for ZGAL. Development happens on an
Apple M4 with MoltenVK, which exposes four queue families of one queue each.

GPU tests run headless on a shared `VulkanContext` with validation and synchronization validation
enabled. The `GpuTest` fixture (`tests/gpu/VkTestContext.h`) fails a test that produces validation
errors, leaks managed resources or `PerFrame` collections, or leaves the number of frames in
flight changed. Test shaders live in `tests/gpu/shaders`; tests also use the example's shaders, and
`testShader(ctx, "name.comp")` loads them.

## Layout

| Directory | Contents |
|---|---|
| `include/core` | `VulkanContext` (instance, device, queues, timelines, frames, deferred destruction), `ResourceManager`/`Handle`, `PerFrame`/`FrameBinding`, Vulkan object wrappers, device feature requirements |
| `include/resources` | `Buffer<T>`, `Image` |
| `include/descriptors` | `DescriptorHeap`: the universal descriptor set and `DescriptorEntry` |
| `include/shaders` | `ShaderLibrary` and `Shader` (SPIR-V loading and reloading), `SpirvReflection` (what shaders write) |
| `include/materials` | `Material`, `ComputeMaterial`, `GraphicsMaterial`, `MaterialBinding`, `Renderable`, `Mesh`, `MeshRenderable`, `AttachmentFormats` |
| `include/graph` | `RenderGraph` (runtime), `Node`/`NodeBuilder`, the pure compiler (`GraphCompiler.h`) and synchronization state machine (`GraphSync.h`), `nodes/` (`RenderablesNode`, `DispatchNode`, `ReadbackNode`) |
| `include/targets` | `RenderTarget`, `Swapchain` |
| `include/app` | `VulkanApp` (the only GLFW user) |
| `include/glsl` | `bindless.glsl`, included by shaders (`glslc -I include/glsl`) |
| `include/util` | `StbImageWrite.h`: `stb_image_write` without its deprecation warnings |
| `include/zgal` | `Runtime.h`: what code generated from ZGAL uses (`zgal::FrameInfo`, `FrameSource`, `Uniform`, material instances) |
| `dsl` | ZGAL (Zach's Graphics Application Language), declaring shader interfaces and render graphs: its compiler (Racket), documentation (`dsl/docs`) and tests (`dsl/tests`) |

Include headers relative to `include/`, e.g. `#include "graph/RenderGraph.h"`. Exactly one
translation unit must `#define VK_WRAP_UTIL_IMPL` before including the wrapper and
`#define STB_IMAGE_IMPLEMENTATION` before including `<stb_image.h>` (see the example). To write
images, define `STB_IMAGE_WRITE_IMPLEMENTATION` in one translation unit and include
`"util/StbImageWrite.h"` rather than `<stb_image_write.h>`.

## Writing an application

Read `examples/simple/simple_example.cpp` first: it uses every major feature of the C++ API. The
other examples declare their shader interfaces and graphs in ZGAL (see "Shaders and graphs in
ZGAL"), and show common kinds of application:

| Example | Shows |
|---|---|
| `examples/image_filter` | Headless use without `VulkanApp`: a chain of compute passes, `#:enabled` passes chosen by options, push constant parameters, an imported image, readback |
| `examples/particles` | Compute writing a graph buffer that a graphics pass reads (across queues), per-frame parameters, a CPU node, input, changing frames in flight at runtime |
| `examples/scene` | A 3D renderer: shadow map, HDR target that follows window resizes, tonemapping, orbit camera, shader hot reload, window or headless targets chosen by a parameter |

```cpp
// Fallible calls return Result<T>; TRY / TRY_ASSIGN return errors to the caller
Result<void> runApp() {
    AppConfig config;
    config.window = WindowConfig{"My app", 1280, 720};   // std::nullopt for headless
    config.enableValidation = true;
    config.shaderSearchPaths = {MY_SHADER_DIR};            // where <name>.spv files are
    config.autoReloadShaders = true;                       // optional
    VulkanApp app;
    TRY(app.init(config));
    VulkanContext& ctx = app.getContext();

    // Resources are owned by the context's ResourceManager and shared through Handle<T>
    TRY_ASSIGN(auto texture, Image::createFromFile(ctx, path));
    TRY_ASSIGN(auto sampler, VulkanSampler::createWithAddressMode(ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT));
    TRY_ASSIGN(auto copyShader, ctx.shaders().get("copy.comp"));

    // A graph per scene/mode; the app executes the active one each frame
    RenderGraph& graph = app.createGraph();
    graph.importImage("texture", texture, ImageState::shaderReadOnly());
    ImageId target = graph.createImage("result", FrameMode::PerFrame, VK_FORMAT_R16G16B16A16_SFLOAT, FixedExtent{{w, h}});
    RenderTarget screen = graph.surfaceTarget();   // the swapchain image plus a depth image

    // Materials reference resources through descriptor entries
    auto& heap = ctx.descriptors();
    auto compute = ctx.resources().create<MyComputeMaterial>(ctx, std::vector<MaterialBinding>{
        MaterialBinding::sampled(heap.sampledImage(texture)),
        MaterialBinding::sampler(heap.sampler(sampler)),
        MaterialBinding::storageImage(graph.storageEntry(target), Access::StorageWrite),
    }, copyShader);

    graph.add<DispatchNode>("copy", compute);              // uses are derived from the material
    graph.add<RenderablesNode>("draw", renderables, screen);
    app.setActiveGraph(graph);
    return app.run();                                      // the error that stopped it, if any
}

int main() { return exitStatus(runApp()); }            // prints the error
```

### Core rules

- **Errors:** the wrapper doesn't throw. Runtime errors (Vulkan failures, device loss, out of
  memory, missing files and shaders, unsuitable devices, graphs that don't compile) are returned as
  `Result<T>` (`core/Result.h`, shaped like `std::expected<T, Error>`); propagate them with `TRY`,
  `TRY_ASSIGN` and `VK_TRY`, and add context with `.withContext("...")`. Programming errors (misuse,
  bad ids, running out of descriptor slots) are contract violations (`core/Contract.h`): the default
  handler prints and aborts. `Result::value()` on an error is one too.
- **Ownership:** create resources through their factories (or `ctx.resources().create<T>(...)`) and
  pass `Handle<T>`s around. Handles are reference counted and thread safe. Every handle must be
  released before the context is destroyed. Never `delete` or `std::make_unique` managed types.
- **The context owns everything shared:** queues, timelines, the universal descriptor set
  (`ctx.descriptors()`), the shader library (`ctx.shaders()`) and frames in flight. Anything created
  from a context must be destroyed before it.
- **Thread safety:** queues are locked by the context (`submit`, `present`, `waitIdle`). Command
  pools are per thread or workload (`ctx.createCommandPool`) and are not thread safe; the caller is
  responsible. `ResourceManager`, `DescriptorHeap`, `ShaderLibrary` and `PerFrame` creation and
  destruction are thread safe. `RenderGraph` and `VulkanApp` are used from one thread.
- **One-off GPU work** (uploads, readbacks) uses `ctx.submitImmediate(fn)`, which blocks.
- **Creation:** fallible types have static factories returning `Result<Handle<T>>` (`Image::create*`,
  `Buffer<T>::create`, `Mesh::create`, `VulkanSampler::create`; `VulkanContext::create` returns a
  `std::unique_ptr`). Materials and nodes construct infallibly; see "Descriptors and materials".
- **Device features:** the wrapper enables what it needs (`DeviceFeatures::required()`). Request
  anything else with `AppConfig::features` (`VulkanContextConfig::features`), e.g.
  `config.features.vulkan12.storageBuffer8BitAccess = VK_TRUE` for 8-bit types in storage buffers.
  Devices lacking a requested feature are rejected by name; `ctx.enabledFeatures()` lists what's on.

### Frames in flight

- The count is a runtime property: `ctx.framesInFlight()`, `ctx.setFramesInFlight(n)` (applied at
  the next frame boundary), `ctx.currentFrame()`. There are no `MAX_FRAMES` template parameters.
- Anything duplicated per frame is a `PerFrame<T>` (or `PerFrameHandles<T>` for handles): a factory
  creates element `i` for frame `i`, and the collection grows and shrinks with the count. Factories
  that can fail return `Result<T>` and go through `PerFrame<T>::create` (or `createManaged` for a
  handle); if one fails while growing, `advanceFrame` returns the error and every collection keeps
  the previous count. Collections grow in creation order and shrink in reverse, so a factory may use
  collections created before it (e.g. per-frame buffers, then their mappings, then their descriptor
  entries).
- `FrameBinding<T>` is either one handle shared by every frame or a `PerFrameHandles<T>`. Use shared
  bindings for static data and per-frame ones for data the CPU rewrites every frame.
- Frames are paced with timelines: `RenderGraph::execute` calls `ctx.beginFrame()` (waits only for
  the reused slot's previous work) and `ctx.advanceFrame()`. Changing the count never waits for the
  whole device. `ctx.retire(fn)` defers destruction until in-flight work finishes.

### Descriptors and materials (bindless)

- There is one descriptor set for everything (`DescriptorHeap`): arrays of sampled images,
  samplers, storage images and storage buffers, plus one pipeline layout with 128 bytes of push
  constants. There are no uniform buffer descriptors: put uniform data in storage buffers
  (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`).
- `heap.sampledImage(image)`, `heap.storageImage(image)`, `heap.sampler(sampler)` and
  `heap.storageBuffer(buffer)` return `Handle<DescriptorEntry>`. An entry owns a slot and keeps its
  resource alive. `entry->index()` is what shaders index with.
- A material lists `MaterialBinding`s. Binding a material pushes their indices as `uint32` push
  constants in binding order, followed by anything `writePushConstants` appends. Each shader
  declares the matching block, e.g.
  ```glsl
  #include "bindless.glsl"
  layout(push_constant) uniform Indices { uint ubo; uint texture; uint textureSampler; } indices;
  BINDLESS_BUFFER(readonly, Ubos, MyUbo, ubos);
  ... texture(sampler2D(bindlessTextures[indices.texture], bindlessSamplers[indices.textureSampler]), uv)
  ... ubos[indices.ubo].data ...
  ```
  `sampler` is a GLSL keyword, so don't name a member that. Storage images are declared without a
  format (`bindlessStorageImages[]`). Vertex and fragment shaders of one material share the block.
  Give every `BINDLESS_BUFFER` in a shader the same qualifiers: MoltenVK can't alias a `readonly`
  block with a writable one (pipeline creation fails).
- **Declare every binding's access accurately.** The graph synchronizes only on declared accesses,
  and synchronization validation can't see accesses through the bindless arrays, so a write
  declared as a read is a silent race. The graph fails to compile when a material's shaders write
  storage images or buffers and none of its bindings declares such a write (reflected from the
  SPIR-V), but it can't tell which binding a write goes through.
- Subclass `ComputeMaterial` (implement `getDispatchDimensions`, `update`). Use or subclass
  `GraphicsMaterial` (vertex and fragment shaders, `AttachmentFormats` of the target and
  `GraphicsPipelineOptions`: vertex layout or none, topology, culling, blend modes, depth test and
  bias). Graphics materials must be built for the formats of the target they draw into:
  `graph.formats(renderTarget)`. A derived constructor may set `bindings_` before the material is
  used (see `TutorialMaterial`).
- Material constructors can't fail: pipelines are built when a graph compiles a plan with a node
  using the material (`NodeBuilder::uses`, which the built-in nodes call), or by
  `material->ensurePipeline()` outside a graph, so pipeline errors come back from
  `compile()`/`execute()`. Binding a material whose pipeline isn't built is a contract violation. A material owning fallible resources (per-frame uniform buffers) gets a
  static `create()` that makes them and passes them to its constructor (`TutorialMaterial::create`).
- `update(frameIndex, surfaceExtent)` is called by the graph once per frame per distinct material,
  just before the first submission recording it. Write per-frame data (e.g. a uniform buffer per
  frame) there. A CPU node ordered before the material's node (by declared uses or
  `addDependency(cpuNode, node)`) runs first, so it can feed `update()` in the same frame. Materials
  of disabled nodes aren't updated.

### Render graphs

- **Resources:** `importImage(name, FrameBinding<Image>, ImageState resting)` for images the app
  owns. The graph returns them to their resting state at the end of every frame, so code outside the
  graph knows their layout. `createImage(name, FrameMode, format, FixedExtent | SurfaceRelative)` and
  `createBuffer(...)` create graph-owned resources. The frame mode is required, and usage is derived
  from the declared uses. `graph.sampledEntry(id)`, `storageEntry(id)` and `storageBufferEntry(id)`
  give materials entries for graph resources that follow recreation.
- **Nodes** declare what they use. `RenderablesNode`, `DispatchNode` and `ReadbackNode` cover common
  cases. Per-frame work that can fail (creating resources) goes in `Node::prepareFrame`, which runs
  before anything is acquired or submitted; `record` and `run` can't fail. For one-offs, use `addGraphics`, `addCompute`, `addTransfer` or `addCpu(name, setup, record/run)`.
  Subclass `GraphicsNode` / `ComputeNode` / `TransferNode` / `CpuNode` for reusable nodes.
  `NodeBuilder::uses(material)` declares the material's resource uses automatically. Declare
  anything else with `read`/`write(id, Access, shaderStages)`, and `renderTo(target)` for attachments.
  Never touch a resource you haven't declared.
- The graph derives order (insertion order is program order for conflicting uses), queues,
  barriers and semaphores, so there are no edges to add. `addDependency(before, after)` exists for
  dependencies the graph can't see. Cycles, two surface writers, missing image usage and
  material/target format mismatches fail `compile()`/`execute()` with `ErrorCode::InvalidGraph`.
  Errors before submission leave the frame unstarted, so it can be retried; errors after (a lost
  device, a failed present) are fatal.
- **Queues:** graphics nodes run on the primary queue. Independent compute and transfer work is
  spread over the other queues automatically (`onPrimaryQueue()` pins a node).
- **The surface:** `graph.surfaceTarget()` writes the swapchain image. Acquire and present are
  automatic, at most one node may write it, and nodes may read it afterwards. A graph without a
  surface use is headless.
- Headless graphs can still have `SurfaceRelative` images: `graph.setSurfaceExtent(extent)` gives
  them (and materials' `update()`) an extent, and changing it recreates them like a window resize,
  so resize handling can be tested without a window.
- `setEnabled(node, bool)` toggles nodes. Plans are cached per set of enabled nodes. CPU nodes
  replace per-frame callbacks.
- `VulkanApp` owns graphs: `createGraph()`, `setActiveGraph()`, `destroyGraph()`. Headless code can
  construct `RenderGraph(ctx)` directly and call `execute()`.
- Input: `app.setInputHandlers({.key = ..., .mouseButton = ..., .cursorPos = ..., .scroll = ...})`
  receives window events during `runFrame()`; `app.window()` gives the `GLFWwindow*` for polling
  (`glfwGetKey`). Don't set GLFW callbacks or the user pointer yourself: the app owns them.

### Shaders and graphs in ZGAL

A material's interface (its bindings, push constants, vertex input, varyings and outputs, and the
structs its buffers hold) can be declared once in a `.zgal` file instead of in both the shaders and
C++, and so can a render graph using those materials. The compiler (`dsl/zgal.rkt`, run by the
Makefile's `zgal-rule`) generates a header with a class per material and a struct per graph, and the
complete shaders: each shader body (GLSL without `#version`, bindings or inputs) preceded by its
material's interface. Prefer it for new applications; the image_filter, particles and scene
examples use it. Read `build/docs/zgal.txt` (from `make docs`) before writing ZGAL: it has the
language, what's generated and every error. In short:

- Bodies use bindings and push constants through accessors, `name()`, e.g.
  `texture(sampler2D(hdr(), hdrSampler()), uv) * exposure()`, and vertex inputs as `inName`.
- A `(graph Name ...)` declares parameters and inputs, samplers, uniforms (per-frame buffers the
  graph writes from a C++ function, only when the value changes), images, buffers, targets,
  material instances and nodes. Push constants and dispatch sizes (`#:invocations`) are computed
  each frame, so they can follow per-frame inputs and images' current sizes.
  `Name::build(ctx, graph, Name::Inputs{...})` adds them to a `RenderGraph` and returns their ids
  (`nodes.x` for `setEnabled` and readbacks). C++ supplies parameters, meshes, imported images and
  per-frame functions (`zgal::FrameSource<T>`), which must outlive the graph.
- Outside a graph, C++ builds a generated material from `Name::loadShaders(ctx)` and
  `Name::Bindings{.binding = entry, ...}`, and fills push constants by overriding
  `writePush(frameIndex, Push&)`.
- Never edit generated files (under `build/`): change the `.zgal` file or the body.

### Shaders

- The wrapper doesn't compile GLSL. Compile to SPIR-V (`glslc -I include/glsl --target-env=vulkan1.3`;
  the Makefile has rules) and load with `ctx.shaders().get("name.stage")`, which finds
  `<name.stage>.spv` in the search paths. The stage comes from the extension.
- `BINDLESS_BUFFER_ARRAY(qualifiers, Block, Type, name)` declares buffers holding arrays sized by the
  buffer (`name[i].data[j]`); pass element counts yourself, as MoltenVK can't compile `.length()`.
- `app.reloadShaders()` (or `AppConfig::autoReloadShaders`) reloads changed SPIR-V. Materials rebuild
  their pipelines, and failures keep the previous version and are reported.

### Images and formats

- `Image::createFromFile` / `createFromUcharBuffer` / `createEmptyRGBA` make sampled sRGB images.
  `Image::createStorage` makes storage images, and fails (`ErrorCode::Unsupported`) for formats the
  device can't store to (e.g. any sRGB format). `Image::supportsUsage` checks a format first.
- Samplers: `VulkanSampler::create(ctx, SamplerDesc{...})` (filter, address mode, a
  `compareOp` for shadow maps, anisotropy); `createWithAddressMode` is shorthand.
- Meshes: `Mesh::create(ctx, vertices, indices)` (16- or 32-bit indices) holds the
  buffers; `create<MeshRenderable>(mesh, material)` draws one with a material, so passes can share
  geometry.
- Colours: textures are sRGB and sampling decodes them to linear. The swapchain is preferably
  `B8G8R8A8_SRGB`, so shaders output linear values.

## Working on the wrapper

- Keep it header only, C++20, and free of GLFW outside `include/app`.
- Match the surrounding style: classes with `camelCase_` members and comments explaining why.
- Never throw. Return `Result`s for runtime errors (`VK_TRY` for Vulkan results, `TRY`/`TRY_ASSIGN`
  to propagate) and call `contractViolation()` for misuse. Tests install a throwing contract
  handler, so misuse is tested with `CHECK_THROWS_AS(..., ContractViolation)`, and errors with
  `CHECK_ERROR(expr, code, text)` (`tests/common/ResultChecks.h`).
- Every change needs tests. CPU tests cover pure logic (compiler, sync, allocators, frame
  registry); GPU tests cover anything touching the device. Keep synchronization validation clean;
  a hazard is a bug.
- Each commit must build and pass all tests on its own; verify a stack with `tools/verify_stack.sh`.
- `docs/MIGRATING.md` describes the old API for consumers; update it if public APIs change.

## Working on ZGAL

- Its documentation (`dsl/docs/zgal.scrbl`) is part of the language: change it in the same commit
  as the compiler, so it stays accurate. A new form or clause gets a grammar, a description and an
  example; show generated code through the document's helpers (`zgal-example`, `generated-file`),
  which compile it as the document builds, and quote repository files with `file-excerpt`, never by
  pasting.
- Every error the compiler reports has an id in the catalog (`dsl/private/errors.rkt`), with an
  explanation and an example that reports it. The documentation renders the catalog, and the tests
  check the catalog and the compiler agree.
- The compiler's output is checked against golden files (`dsl/tests/golden/expected`). When it
  changes on purpose, regenerate them (`ZGAL_UPDATE_GOLDEN=1 raco test dsl/tests/golden-test.rkt`)
  and review the difference. Changes to generated C++ or GLSL also need GPU tests
  (`tests/gpu/ZgalTest.cpp`, with interfaces in `tests/gpu/zgal`).
- Keep the checked program (`dsl/private/ast.rkt`) independent of backends: GLSL and C++ are
  emitters, and others (e.g. WebGL) are planned.
