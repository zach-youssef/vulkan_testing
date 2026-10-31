# The Vulkan wrapper

How to build applications with the header-only C++20 Vulkan 1.3 wrapper (`include/`), and how to
work on it. Code written against the pre-refactor API is ported with [MIGRATING.md](MIGRATING.md).

## Headers

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

`include/zgal` holds what code generated from ZGAL uses; ZGAL's reference maps its files ([zgal.md](zgal.md)).

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
in ZGAL ([zgal.md](zgal.md)), and the others declare their graphs there too.
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
  `nonuniformEXT`, and a node using the material uses every element. `array->setElements(...)`
  changes them (any number; buffers grow, and graphs declare the new uses before their next frame),
  and `graph.setElementSource(array, fn)` chooses them each frame.
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
  changes the graph recreates the buffer, discarding its contents, and its entries follow. Created
  buffers start zeroed, like WebGPU's: the first submission of the next frame in each frame slot
  fills a new device-local copy once (no extra submission or wait), and host-visible ones are
  zeroed when created. A copy that code outside the graph gets (`createdBuffer`, `buffer`) before
  its first frame, e.g. to upload into it, is zeroed then instead, waiting, so the upload stays.
  A failed immediate fill isn't reported, and the frame's fill still zeroes that copy, so an upload
  made after it may be overwritten. Nodes write graph buffers through their declared uses: a CPU
  node's own `submitImmediate` writes before the frame's first GPU submission are overwritten by
  the fill.
  `graph.sampledEntry(id)`, `storageEntry(id)` and `storageBufferEntry(id)` give materials entries
  for graph resources that follow recreation (and give them the usage).
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
  dependencies the graph can't see. Cycles, two surface writers, missing image (or imported buffer) usage and
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
  finished copy without waiting, for polling between frames (`ReadbackNode` likewise). A finished
  copy whose frame slot is reused before it's taken is kept aside until it is (`FinishedCopies`),
  so polling before each frame never starves, however far the GPU lags.
- Profiling: `graph.setTimingEnabled(true)` writes timestamps around every GPU node, and
  `graph.timings()` returns the last timed frame's per-node GPU milliseconds (waiting for it).
  Timestamps serialize work around timed nodes on their queue, so compare timings with each other;
  other queues' work runs alongside and inflates a node's time, so time a node alone (disable the
  others) for its own cost. The graph
  labels each node's commands with its name (`ctx.beginLabel`/`endLabel`, VK_EXT_debug_utils), so
  GPU captures and Instruments' Metal System Trace show node names
  ([building-and-testing.md](building-and-testing.md#profiling) has the tools).
- `VulkanApp` owns graphs: `createGraph()`, `setActiveGraph()`, `destroyGraph()`. Headless code can
  construct `RenderGraph(ctx)` directly and call `execute()`.
- Input: `app.setInputHandlers({.key = ..., .mouseButton = ..., .cursorPos = ..., .scroll = ...})`
  receives window events during `runFrame()`; `app.window()` gives the `GLFWwindow*` for polling
  (`glfwGetKey`). Don't set GLFW callbacks or the user pointer yourself: the app owns them.

### Shaders

- The wrapper doesn't compile GLSL. Compile to SPIR-V (`glslc -I include/glsl --target-env=vulkan1.3`;
  the Makefile has rules) and load with `ctx.shaders().get("name.stage")`, which finds
  `<name.stage>.spv` in the search paths. The stage comes from the extension. Shaders with ZGAL
  bodies are named `<Material>.<stage>` (e.g. `Lit.frag`; GLSL bodies keep their file's name), and
  their generated classes load them.
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
  `compareOp` for shadow maps, anisotropy, which only samplers filtering linearly between texels and
  mip levels use); `createWithAddressMode` is shorthand.
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

## The development device

Development happens on an Apple M4 with MoltenVK, which exposes four queue families of one queue
each. What it can't do shapes the wrapper and the shaders written for it:

- No `drawIndirectCount` (the wrapper adapts; see "Render graphs"). Subgroups are 32 wide (not in
  vertex shaders), and subgroup min/max *scans* fail pipeline creation (reductions and shuffles
  work). 32-bit float atomics work on buffers and shared memory; there are no image atomics.
- glslc (targeting Vulkan 1.3) turns `discard` into demotion, so the wrapper always enables
  `shaderDemoteToHelperInvocation`.
- A shader module using a capability the device didn't enable is a validation error, so materials
  generated from ZGAL check the context's features and subgroup support in `loadShaders`
  (`include/zgal/Material.h`) before creating modules.
- MoltenVK can't alias a `readonly` buffer block with a writable one, or compile `.length()` on
  bindless buffers (see "Descriptors and materials" and "Shaders").

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
- Each commit must build and pass all tests on its own; verify a stack with `tools/verify_stack.sh`
  ([building-and-testing.md](building-and-testing.md#verifying-a-stack)).
- [MIGRATING.md](MIGRATING.md) describes the old API for consumers; update it if public APIs change.
