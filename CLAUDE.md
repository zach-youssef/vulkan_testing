# vulkan_test

A header-only C++20 Vulkan 1.3 wrapper (`include/`), an example (`examples/simple/`) and its tests
(`tests/`). This file is for agents building applications with the wrapper or working on it.
Agents migrating code written against the pre-refactor wrapper (the `main` branch API, e.g. in
`~/code/ppu`) should read `docs/MIGRATING.md` first.

## Building and testing

- `make`: builds `build/examples/simple/simple_example` (shaders into `build/examples/simple/shaders`)
- `make shaders`: rebuilds the example's SPIR-V (the running example reloads it)
- `make test-cpu`, `make test-gpu`, `make test`: build and run the tests (doctest). Forward
  arguments with `TEST_ARGS`, e.g. `make test-gpu TEST_ARGS='-tc="RenderGraph*"'`.
- `make approve-screenshots`: accept the screenshots of the last failed GPU run
  (`tests/gpu/approved/<device>/`). Never approve a changed screenshot without understanding why
  it changed.
- `tools/verify_stack.sh <repo> <base> [head]`: builds and tests every commit in a range (builds and
  CPU tests in parallel, GPU tests one at a time). Check its exit codes, not filtered output.

Requirements: the Vulkan SDK (path in the Makefile), `glslc`, GLFW and GLM. Development happens on an
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
| `include/shaders` | `ShaderLibrary` and `Shader` (SPIR-V loading and reloading) |
| `include/materials` | `Material`, `ComputeMaterial`, `BasicMaterial`, `MaterialBinding`, `Renderable`, `MeshRenderable`, `AttachmentFormats` |
| `include/graph` | `RenderGraph` (runtime), `Node`/`NodeBuilder`, the pure compiler (`GraphCompiler.h`) and synchronization state machine (`GraphSync.h`), `nodes/` (`RenderablesNode`, `DispatchNode`, `ReadbackNode`) |
| `include/targets` | `RenderTarget`, `Swapchain` |
| `include/app` | `VulkanApp` (the only GLFW user) |
| `include/glsl` | `bindless.glsl`, included by shaders (`glslc -I include/glsl`) |

Include headers relative to `include/`, e.g. `#include "graph/RenderGraph.h"`. Exactly one
translation unit must `#define VK_WRAP_UTIL_IMPL` before including the wrapper and
`#define STB_IMAGE_IMPLEMENTATION` before including `<stb_image.h>` (see the example).

## Writing an application

Read `examples/simple/simple_example.cpp` first: it uses every major feature.

```cpp
AppConfig config;
config.window = WindowConfig{"My app", 1280, 720};   // std::nullopt for headless
config.enableValidation = true;
config.shaderSearchPaths = {MY_SHADER_DIR};            // where <name>.spv files are
config.autoReloadShaders = true;                       // optional
VulkanApp app;
app.init(config);
VulkanContext& ctx = app.getContext();

// Resources are owned by the context's ResourceManager and shared through Handle<T>
Handle<Image> texture;
Image::createFromFile(texture, ctx, path);
Handle<VulkanSampler> sampler;
VulkanSampler::createWithAddressMode(sampler, ctx, VK_SAMPLER_ADDRESS_MODE_REPEAT);

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
}, ctx.shaders().get("copy.comp"));

graph.add<DispatchNode>("copy", compute);              // uses are derived from the material
graph.add<RenderablesNode>("draw", renderables, screen);
app.setActiveGraph(graph);
app.run();
```

### Core rules

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
- **Device features:** the wrapper enables what it needs (`DeviceFeatures::required()`). Request
  anything else with `AppConfig::features` (`VulkanContextConfig::features`), e.g.
  `config.features.vulkan12.storageBuffer8BitAccess = VK_TRUE` for 8-bit types in storage buffers.
  Devices lacking a requested feature are rejected by name; `ctx.enabledFeatures()` lists what's on.

### Frames in flight

- The count is a runtime property: `ctx.framesInFlight()`, `ctx.setFramesInFlight(n)` (applied at
  the next frame boundary), `ctx.currentFrame()`. There are no `MAX_FRAMES` template parameters.
- Anything duplicated per frame is a `PerFrame<T>` (or `PerFrameHandles<T>` for handles): a factory
  creates element `i` for frame `i`, and the collection grows and shrinks with the count.
  Collections grow in creation order and shrink in reverse, so a factory may use collections created
  before it (e.g. per-frame buffers, then their mappings, then their descriptor entries).
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
- Subclass `ComputeMaterial` (implement `getDispatchDimensions`, `update`) or `BasicMaterial<N>`
  (vertex and fragment shaders, `AttachmentFormats` of the target, vertex layout). Graphics
  materials must be built for the formats of the target they draw into:
  `graph.formats(renderTarget)`. A derived constructor may set `bindings_` before the material is
  used (see `TutorialMaterial`).
- `update(frameIndex, surfaceExtent)` is called by the graph once per frame per distinct material.
  Write per-frame data (e.g. a uniform buffer per frame) there.

### Render graphs

- **Resources:** `importImage(name, FrameBinding<Image>, ImageState resting)` for images the app
  owns. The graph returns them to their resting state at the end of every frame, so code outside the
  graph knows their layout. `createImage(name, FrameMode, format, FixedExtent | SurfaceRelative)` and
  `createBuffer(...)` create graph-owned resources. The frame mode is required, and usage is derived
  from the declared uses. `graph.sampledEntry(id)`, `storageEntry(id)` and `storageBufferEntry(id)`
  give materials entries for graph resources that follow recreation.
- **Nodes** declare what they use. `RenderablesNode`, `DispatchNode` and `ReadbackNode` cover common
  cases. For one-offs, use `addGraphics`, `addCompute`, `addTransfer` or `addCpu(name, setup, record/run)`.
  Subclass `GraphicsNode` / `ComputeNode` / `TransferNode` / `CpuNode` for reusable nodes.
  `NodeBuilder::uses(material)` declares the material's resource uses automatically. Declare
  anything else with `read`/`write(id, Access, shaderStages)`, and `renderTo(target)` for attachments.
  Never touch a resource you haven't declared.
- The graph derives order (insertion order is program order for conflicting uses), queues,
  barriers and semaphores, so there are no edges to add. `addDependency(before, after)` exists for
  dependencies the graph can't see. Cycles, two surface writers, missing image usage and
  material/target format mismatches throw `GraphError` when compiling.
- **Queues:** graphics nodes run on the primary queue. Independent compute and transfer work is
  spread over the other queues automatically (`onPrimaryQueue()` pins a node).
- **The surface:** `graph.surfaceTarget()` writes the swapchain image. Acquire and present are
  automatic, at most one node may write it, and nodes may read it afterwards. A graph without a
  surface use is headless.
- `setEnabled(node, bool)` toggles nodes. Plans are cached per set of enabled nodes. CPU nodes
  replace per-frame callbacks.
- `VulkanApp` owns graphs: `createGraph()`, `setActiveGraph()`, `destroyGraph()`. Headless code can
  construct `RenderGraph(ctx)` directly and call `execute()`.
- Input: `app.setInputHandlers({.key = ..., .mouseButton = ..., .cursorPos = ..., .scroll = ...})`
  receives window events during `runFrame()`; `app.window()` gives the `GLFWwindow*` for polling
  (`glfwGetKey`). Don't set GLFW callbacks or the user pointer yourself: the app owns them.

### Shaders

- The wrapper doesn't compile GLSL. Compile to SPIR-V (`glslc -I include/glsl --target-env=vulkan1.3`;
  the Makefile has rules) and load with `ctx.shaders().get("name.stage")`, which finds
  `<name.stage>.spv` in the search paths. The stage comes from the extension.
- `app.reloadShaders()` (or `AppConfig::autoReloadShaders`) reloads changed SPIR-V. Materials rebuild
  their pipelines, and failures keep the previous version and are reported.

### Images and formats

- `Image::createFromFile` / `createFromUcharBuffer` / `createEmptyRGBA` make sampled sRGB images.
  `Image::createStorage` makes storage images, and throws for formats the device can't store to
  (e.g. any sRGB format). `Image::supportsUsage` checks a format first.
- Colours: textures are sRGB and sampling decodes them to linear. The swapchain is preferably
  `B8G8R8A8_SRGB`, so shaders output linear values.

## Working on the wrapper

- Keep it header only, C++20, and free of GLFW outside `include/app`.
- Match the surrounding style: classes with `camelCase_` members, comments explaining why, and
  `VK_SUCCESS_OR_THROW` for Vulkan results.
- Every change needs tests. CPU tests cover pure logic (compiler, sync, allocators, frame
  registry); GPU tests cover anything touching the device. Keep synchronization validation clean;
  a hazard is a bug.
- Each commit must build and pass all tests on its own; verify a stack with `tools/verify_stack.sh`.
- `docs/MIGRATING.md` describes the old API for consumers; update it if public APIs change.
