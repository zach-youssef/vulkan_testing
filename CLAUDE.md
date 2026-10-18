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
- `zgal.mk`: the make rules building ZGAL programs (`zgal-rule`, `zgal-program`), which the Makefile
  and other projects' Makefiles include.
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
| `include/resources` | `Buffer<T>`, `Image`, `Model` (glTF and OBJ loading: `ModelData`, `uploadModel`) |
| `include/descriptors` | `DescriptorHeap`: the universal descriptor set and `DescriptorEntry` |
| `include/shaders` | `ShaderLibrary` and `Shader` (SPIR-V loading and reloading), `SpirvReflection` (what shaders write) |
| `include/materials` | `Material`, `ComputeMaterial`, `GraphicsMaterial`, `MaterialBinding`, `Renderable`, `Mesh`, `MeshRenderable`, `AttachmentFormats` |
| `include/graph` | `RenderGraph` (runtime), `Node`/`NodeBuilder`, the pure compiler (`GraphCompiler.h`) and synchronization state machine (`GraphSync.h`), `nodes/` (`RenderablesNode`, `DispatchNode`, `DispatchSequenceNode`, `IndirectDrawNode`, `ReadbackNode`, `BufferReadbackNode`) |
| `include/targets` | `RenderTarget`, `Swapchain` |
| `include/app` | `VulkanApp` (the only GLFW user) |
| `include/glsl` | `bindless.glsl`, included by shaders (`glslc -I include/glsl`) |
| `include/util` | `StbImageWrite.h`: `stb_image_write` without its deprecation warnings |
| `include/zgal` | What code generated from ZGAL uses: `Material.h` for materials (checks and shaders every generated material shares), `Runtime.h` for graphs (`zgal::FrameInfo`, `FrameSource`, `Uniform`, material instances), `App.h` for apps (events, `Timestep`, `RunMode`), `Main.h` for generated mains (command line, validation, captures, headless runs), and `Window.h`, running an app in a `VulkanApp` (and windowed programs) |
| `dsl` | ZGAL (Zach's Graphics Application Language), declaring shader interfaces and render graphs: its compiler (Racket), documentation (`dsl/docs`) and tests (`dsl/tests`) |

Include headers relative to `include/`, e.g. `#include "graph/RenderGraph.h"`. Exactly one
translation unit must `#define VK_WRAP_UTIL_IMPL` before including the wrapper and
`#define STB_IMAGE_IMPLEMENTATION` before including `<stb_image.h>` (see the example). That
translation unit must also include `"resources/Model.h"` if any uses it (directly, or through code
generated for a ZGAL `model`), as it compiles cgltf's and tinyobjloader's implementations there. To write
images, define `STB_IMAGE_WRITE_IMPLEMENTATION` in one translation unit and include
`"util/StbImageWrite.h"` rather than `<stb_image_write.h>`.

## Writing an application

Read `examples/simple/simple_example.cpp` first: it builds its graph with the C++ API, using
materials and a small app generated from `simple.zgal`. Every example's shaders and app are written
in ZGAL (see "Shaders, graphs and apps in ZGAL"), and the others declare their graphs there too.
The other three are programs written entirely in ZGAL: their `main` is generated (`(main ...)`),
with their options, window and smoke-test capture, and they have no C++. They show common kinds of
application:

| Example | Shows |
|---|---|
| `examples/image_filter` | Headless use without `VulkanApp` (a generated main without a window): a chain of compute passes enabled from the app's state, push constant parameters from command-line options, an image file named on the command line, readback written to a PNG |
| `examples/particles` | Compute writing a graph buffer that a graphics pass reads (across queues), per-frame parameters computed from app state, input handlers, changing frames in flight at runtime (scripted by the app in its smoke test), a generated main with a window |
| `examples/scene` | A 3D renderer: a glTF model (`assets/scene.gltf`, written by `make_scene.py`), shadow map, HDR target that follows window resizes, tonemapping, an orbit camera whose matrices are ZGAL functions, toggling a pass, shader hot reload, window or headless targets chosen by how it's run, a smoke test scripted by the app (resizes, a pass toggled) |

```cpp
// Fallible calls return Result<T>; TRY / TRY_ASSIGN return errors to the caller
Result<void> runApp() {
    AppConfig config;
    config.window = WindowConfig{"My app", 1280, 720};   // std::nullopt for headless
    config.enableValidation = true;
    // SPIR-V (<name>.<stage>.spv) is found in shaders/ beside the executable by default
    // (config.shaderSearchPaths)
    // Assets (models, images) by relative path are found in assets/ beside it (config.assetSearchPaths)
    config.autoReloadShaders = true;                       // optional
    VulkanApp app;
    TRY(app.init(config));
    VulkanContext& ctx = app.getContext();

    // Resources are owned by the context's ResourceManager and shared through Handle<T>
    TRY_ASSIGN(auto texture, Image::createFromFile(ctx, path, ColorSpace::Srgb));
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
- **Device features:** the wrapper enables what it needs (`DeviceFeatures::required()`), and what it
  can do without when the device has it (`DeviceFeatures::optional()`: `drawIndirectCount`). Request
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
  For a set of entries a shader chooses between (e.g. textures), `DescriptorArray::create(ctx,
  {entry, ...})` keeps their indices in a small buffer per frame (the count, then the indices) and
  `MaterialBinding::array(array, access)` pushes its index; shaders index the universal arrays with
  `nonuniformEXT`, and a node using the material uses every element.
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
  from the declared uses. A buffer's size can be a function of the frame (`createBuffer(name, mode,
  [](const FrameContext&) { return bytes; })`), computed at the start of every frame: when it
  changes the graph recreates the buffer, discarding its contents, and its entries follow. `graph.sampledEntry(id)`, `storageEntry(id)` and `storageBufferEntry(id)`
  give materials entries for graph resources that follow recreation.
- **Nodes** declare what they use. `RenderablesNode`, `DispatchNode` and `ReadbackNode` cover common
  cases. GPU-driven work reads commands shaders wrote: `DispatchNode(material, BufferOffset{buffer,
  offset})` dispatches indirectly, and `IndirectDrawNode` draws `VkDraw(Indexed)IndirectCommand`s
  (from a mesh's buffers for indexed ones), with a count from a buffer where the device has
  `drawIndirectCount` (`ctx.enabledFeatures()`; the context enables it when it can).
  `DispatchSequenceNode` dispatches several materials in order as one node, with barriers between
  them (the steps of an algorithm), skipping steps with no workgroups that frame. Per-frame work that can fail (creating resources) goes in `Node::prepareFrame`, which runs
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
- `setEnabled(node, bool)` toggles nodes; `runOnce(node)` runs one in the next frame only. Plans
  are cached per set of enabled nodes. CPU nodes replace per-frame callbacks. `enableWith(follower,
  leader)` makes a node that only prepares another's input follow its enabled state.
- `BufferReadbackNode::contents<T>()` waits for the latest copy; `takeFinished<T>()` returns the newest
  finished copy without waiting, for polling between frames.
- Profiling: `graph.setTimingEnabled(true)` writes timestamps around every GPU node, and
  `graph.timings()` returns the last timed frame's per-node GPU milliseconds (waiting for it).
  Timestamps serialize work around timed nodes on their queue, so compare timings with each other;
  other queues' work runs alongside and inflates a node's time, so time a node alone (disable the
  others) for its own cost. `make run-bench` times ZGAL's library kernels (`tools/bench`). The graph
  labels each node's commands with its name (`ctx.beginLabel`/`endLabel`, VK_EXT_debug_utils), so
  GPU captures and Instruments' Metal System Trace show node names.
- `VulkanApp` owns graphs: `createGraph()`, `setActiveGraph()`, `destroyGraph()`. Headless code can
  construct `RenderGraph(ctx)` directly and call `execute()`.
- Input: `app.setInputHandlers({.key = ..., .mouseButton = ..., .cursorPos = ..., .scroll = ...})`
  receives window events during `runFrame()`; `app.window()` gives the `GLFWwindow*` for polling
  (`glfwGetKey`). Don't set GLFW callbacks or the user pointer yourself: the app owns them.

### Shaders, graphs and apps in ZGAL

A `.zgal` file declares materials (their interface, declared once instead of in both the shaders
and C++: bindings, push constants, vertex input, varyings, outputs and the structs their buffers
hold), their shaders, in ZGAL's expression language, render graphs using them, and apps running a
graph. The compiler (`dsl/zgal.rkt`, run by `zgal.mk`'s `zgal-rule`, which also compiles the
shaders) generates a header with a class per material, a struct per graph and a class per app (all
in a namespace named after the file: `scene.zgal` gives `scene::SceneApp`), and the complete GLSL
shaders. Prefer it
for new applications; every example uses it. Read `build/docs/zgal.txt` (from `make docs`) before
writing ZGAL: it has the language, what's generated and every error. In short:

- `(import camera)` imports a standard library module (`dsl/lib`: `camera` with `lookAt`,
  `perspective`, `ortho`, `orbit`; `color`; `random`; `math`), and `(import "file.zgal")` another
  file's structs, functions, constants and materials (so programs share materials). Use the library before writing your own.
- For what bodies can't express yet, `(extern-glsl "file.glsl" ((f [x : T]) : R))` declares
  pure GLSL functions bodies can call (shaders only; apps can't). Prefer ZGAL.
- Shaders are `(vertex ...)`, `(fragment ...)` and `(compute ...)` clauses: expressions, with
  immutable `let*` bindings, folds over ranges (`fold`, `sum`, `for`), and effects (`write!` to
  buffers, `store!` to images). They name bindings, push constants, vertex attributes and varyings
  directly (`scene.viewProjection`, `(sample hdr hdrSampler uv)`). A vertex shader gives
  `(results #:position ... #:varying ...)`; a fragment shader its output's value.
- Storage bindings' accesses and graphics bindings' stages are inferred from the shaders, so the
  graph synchronizes what they actually do; don't declare them unless a shader is GLSL.
- Compute materials can declare `(shared (name type) ...)` workgroup variables, written with
  `write!` and synchronized with `(barrier!)` (`(buffer-barrier!)` for buffer writes too), which the
  compiler checks every invocation reaches.
- Also in bodies: atomics giving the previous value (`(atomic-add! (at bins b) 1)`, min, max, and,
  or, xor, exchange, compare-exchange; float adds need `requiredFeatures()`), strided ranges
  `(range from to step)`, `(while ([acc init] ...) condition next)` loops, `(return!)` in compute
  and `(discard!)` in fragment shaders, subgroup built-ins (`subgroupAdd`, `subgroupElect`, ...,
  checked against the device when shaders load) and derivatives (`fwidth`, fragment only).
- `(name (array texture))` (or of storage images or buffers) binds any number of entries:
  bodies choose with `(at name i)` and count with `(array-length name)`, and graphs give a list of
  resources, `(name (a b c))`, or `(import-images name #:resting ...)` images C++ gives.
- `uint8`, `int8`, `uint16` and `int16` pack buffers and shared memory. Bodies read them widened to
  `uint`/`int` and narrow explicitly to write them (`(uint8 x)`). A material using them needs device
  features: add its (or its graph's) `requiredFeatures()` to the context's features.
- Graphs load assets when they're built: `(image-file name "file.png" #:color srgb)` (or `linear`)
  imports an image file found in the asset search paths; `(buffer-file b T "dump.bin" ...)` reads
  binary files of whole `T`s (e.g. memory dumps) into a buffer; `(model m "ship.gltf" Format
  #:base-colors textures #:base-color-factors factors)` loads a glTF or OBJ model as meshes of a
  vertex format, filling its attributes by name (`position`, `normal`, `uv`, `color`, `tangent`;
  others need `#:default`), which `draw-meshes` draws; shaders read a mesh's material as
  `instanceIndex` and index the textures (an `(array texture)`) and factors (a `buffer-array vec4`)
  with it. An asset's path can be a `(input p path)` (`std::filesystem::path`) instead of a string.
- Vertex attributes can be stored packed, `(color vec4 #:as unorm8)` (`float16`, `unorm8/16`,
  `snorm8/16`, `uint8/16`, `int8/16`): shaders read the declared type, and the C++ struct holds
  GLM's sized types (fill them with `glm::packUnorm` etc., as the generated comments say).
- GPU-driven work: shaders write the commands of `(import indirect)` (`DispatchCommand`,
  `DrawCommand`, `DrawIndexedCommand`) into graph buffers, and `(dispatch d m #:indirect (at cmds i))`
  or `(draw d target m #:indirect (at cmds i) #:draws n #:count (at counts j))` read them;
  `draw-meshes` of a model reads `DrawIndexedCommand`s a shader chose from the model's `#:draws` and
  `#:bounds` (model options). Counts
  use `drawIndirectCount` where the device has it, else a generated node first gives the commands
  past the count no instances (MoltenVK lacks it).
- The compute library: `(reduce n source (at dest i) #:op add|min|max #:count c)`,
  `(inclusive-scan n source dest ...)` and `(exclusive-scan ...)` nodes over buffers of uints, ints
  or floats, and `(sort n keys #:values v #:count c)`, a stable radix sort of uint keys (`(import
  sorting)` maps floats and ints to them). `#:count` is an expression or a GPU-written `(at counts i)`. The compiler instantiates
  their materials from `dsl/lib/kernels` (named `Zgal...`) and dispatches them as one
  `DispatchSequenceNode`.
- Node loops: `(for ([b batches] [pass (range 0 n)]) node ...)` repeats nodes when the graph is
  built, over build-time ranges and `(data T)` inputs' elements (`b.first`, `(at batches i)`).
  Dispatches and draws give per-node `#:push ((field v))` (and dispatches `#:invocations`), so
  iterations share a material and its pipeline; a material may leave out push constants every node
  using it gives. A looped node's id is a `std::vector<NodeId>` (readback accessors take an index).
  Loops don't hold extern nodes, library nodes or counted draws yet; draws looping into one target
  need `#:load load`. Prefer loops to `extern` nodes where ZGAL has the nodes.
- Top-level `(define (f [x : vec3]) : vec3 ...)` functions and `(define name value)` constants are
  shared by the shaders, and those without textures, images or effects also compile to C++
  (`sunDirection` in the scene example), so the CPU computes the same thing.
- GLSL bodies (a `(shaders "x.frag" ...)` clause) still work: they use bindings and push constants
  through accessors, `name()`, and vertex inputs as `inName`. Prefer ZGAL bodies.
- A `(graph Name ...)` declares parameters and inputs, samplers, uniforms (per-frame buffers the
  graph writes from a C++ function, only when the value changes), images, buffers, imported images
  and buffers, targets, material instances and nodes: dispatches, draws, readbacks, `cpu` nodes
  (with `#:reads`/`#:writes` of imported buffers they map), `copy` nodes between buffers (or a buffer
  into an image), `clear` nodes, readbacks of images and buffers (read through the generated
  `nameContents(graph)` / `namePixels(graph)`), and `extern` nodes, where a C++ function adds nodes
  that depend on the application's data. Data C++ computes goes in with `(input d (data T))` (a
  `std::vector<T>`) and `(upload buf d)`, which creates and fills a buffer when the graph is built. Push
  constants, dispatch sizes (`#:invocations`) and buffers' counts are computed each frame, so they
  can follow per-frame inputs (e.g. an app's state) and images' current sizes; a buffer whose count
  changes is recreated, its contents discarded. `Name::build(ctx, graph, Name::Inputs{...})` adds
  them to a `RenderGraph` and returns their ids (`nodes.x` for `setEnabled` and readbacks). C++
  supplies parameters, meshes, imported images and buffers, and functions (per-frame values,
  `zgal::FrameSource<T>`; cpu work; extern nodes), which must outlive the graph. Images, buffers,
  targets, materials and nodes take `#:when` (build-time): e.g. a window's surface target, the
  material drawing into it and its draw `#:when (not headless)`, as headless graphs have no surface.
- Outside a graph, C++ builds a generated material from `Name::loadShaders(ctx)` and
  `Name::Bindings{.binding = entry, ...}`, and fills push constants by overriding
  `writePush(frameIndex, Push&)`.
- An `(app Name ...)` declares parameters, an immutable `(state (field type initial) ...)`, and
  `(on event body)` handlers whose value is the next state (usually `(with state #:field v)`):
  events are `key-pressed`/`key-released` (key names like `space`, `a`, `1`), `mouse-pressed`/
  `mouse-released`, `cursor-moved` (positions in [-1, 1]), `scrolled` and `(frame dt)`. Handlers'
  effects are `print!`, `set-frames-in-flight!`, `resize!` (the window, or headless the surface) and
  `quit!`. Bodies read `frameNumber`, and how the app is run: `headless`, `capture` and `frameLimit`
  (C++ gives a `zgal::RunMode` in `Inputs`), e.g. to script a smoke test. `(on (read readback xs) ...)`
  receives a buffer readback's finished copy as `(data T)` (immutable shared elements,
  `zgal::Data<T>`; `count`, `at`, `empty-data`), which state fields, externs and graphs'
  `(input d (per-frame (data T)))` (uploaded again when it changes) take. `(run Graph (input expr) ...
  #:enabled ((node expr) ...))` computes the graph's parameters once and per-frame inputs and node
  enables each frame from the state; C++ gives the graph's other inputs through the app's
  `Inputs`. Several `(run G ... #:when condition)` clauses are phases: each frame the first whose
  condition holds (else the last) is `renderGraph()`, and `create` takes `Name::Graphs` (a
  `RenderGraph*` per graph); nodes with `#:once #t` run in their graph's first frame.
  `(extern (f [x : T]) : R)` declares a function C++ gives (a `std::function` in
  `Inputs`) that bodies call, for what ZGAL can't do (reading datasets, saving files). C++ creates
  it with `Name::create(ctx, graph, Name::Inputs{..., .timestep =
  zgal::Timestep::realTime(max) or fixed(step)})` and runs it with `zgal::run(vulkanApp, *app)`
  (`include/zgal/Window.h`), or headless calls `app->beginFrame()` (a `Result`) and executes `app->renderGraph()` each frame. Tests drive
  it with `handle(zgal::Event{...})`, `state()` and `setState()`.
- A `(main App ...)` makes the file a program: the compiler writes `name.zgal.main.cpp` too, with
  the built-in options (`--frames N`, `--capture FILE`, `--size WxH`, `--window`, `--validation`,
  `--plan`, `--timings` (per-node GPU time), `--capture-frames 1,2,60` (each to `<capture>/<frame>.png`), `--help`) and the app's `(flag p "--x" "help")`, `(option p "--x" "help" #:default v
  #:range (lo hi) #:given flag)` and `(positional p "help")` setting its parameters (`path`
  parameters too, passed to graphs' path inputs), a `(window "title" w h)` (without one, headless
  only), the clock `(timestep #:max s #:fixed s)` and `(capture readback [#:to path-param])` written
  to a PNG. What options can't give (externs, meshes, imports, other parameters) the program's own
  C++ gives by defining `Result<name::MainInputs> name::mainInputs(VulkanContext&)`. `zgal.mk`'s
  `zgal-program` builds one (its own C++ is the `.cpp` files beside the `.zgal` file); other projects'
  Makefiles include `zgal.mk` too. Prefer it to a handwritten `main`.
- Never edit generated files (under `build/`): change the `.zgal` file (or a GLSL body).

### Shaders

- The wrapper doesn't compile GLSL. Compile to SPIR-V (`glslc -I include/glsl --target-env=vulkan1.3`;
  the Makefile has rules) and load with `ctx.shaders().get("name.stage")`, which finds
  `<name.stage>.spv` in the search paths. The stage comes from the extension. Shaders written in
  ZGAL are named after their material, e.g. `Lit.frag`, and their generated classes load them.
- `BINDLESS_BUFFER_ARRAY(qualifiers, Block, Type, name)` declares buffers holding arrays sized by the
  buffer (`name[i].data[j]`); pass element counts yourself, as MoltenVK can't compile `.length()`.
- `app.reloadShaders()` (or `AppConfig::autoReloadShaders`) reloads changed SPIR-V. Materials rebuild
  their pipelines, and failures keep the previous version and are reported.

### Images and formats

- `Image::createFromFile` / `createFromEncoded` / `createFromUcharBuffer` make sampled RGBA8 images
  in the `ColorSpace` given (`Srgb` for colours, `Linear` for data); `createEmptyRGBA` makes sRGB
  ones. `ctx.findAsset(path)` finds files in `assetSearchPaths`.
  `Image::createStorage` makes storage images, and fails (`ErrorCode::Unsupported`) for formats the
  device can't store to (e.g. any sRGB format). `Image::supportsUsage` checks a format first.
- Samplers: `VulkanSampler::create(ctx, SamplerDesc{...})` (filter, address mode, a
  `compareOp` for shadow maps, anisotropy); `createWithAddressMode` is shorthand.
- Meshes: `Mesh::create(ctx, vertices, indices[, instanceIndex])` (16- or 32-bit indices) holds the
  buffers; `create<MeshRenderable>(mesh, material)` draws one with a material, so passes can share
  geometry. Vertex shaders read the mesh's instance index as `gl_InstanceIndex`.
- Models: `ModelData::load(path)` reads a glTF or OBJ file (triangles with transforms baked in, and
  base colour materials); `uploadModel<Vertex>(ctx, data, toVertices)` makes a `Model`: one mesh, a
  draw per primitive (`VkDrawIndexedIndirectCommand`s whose first instance is the material, and
  bounding spheres, for GPU culling), base colour factors and sRGB textures. `model.renderables(ctx,
  material)` draws them with a `RenderablesNode`; `MeshRenderable(mesh, material, MeshRange{...})`
  draws part of a mesh.
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
