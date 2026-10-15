# Migrating from the pre-refactor wrapper

For agents porting a codebase (e.g. `~/code/ppu`) written against the wrapper as it was on the
`main` branch: headers in `vulkan_test/`, `VulkanApp<MAX_FRAMES>`, objects created from
`VkDevice`/`VkPhysicalDevice`/`VkQueue`/`VkCommandPool` arguments and owned by `std::unique_ptr`,
descriptor sets per material, and render graphs wired with explicit edges.

Read the repository's `CLAUDE.md` for the new model, and `examples/simple/simple_example.cpp`, which
is the old example ported. Comparing `git show main:vulkan_test/main.cpp` with it is the fastest way
to see a full migration.

## Approach

1. Update includes and the build first (see "Build"), so the codebase fails to compile only where APIs
   changed.
2. Port bottom up: app and context, then resources, then shaders, then materials (and their GLSL),
   then the render graph.
3. Port shaders to the universal descriptor set at the same time as their materials. Push constant
   order must match the material's bindings.
4. Run with `AppConfig::enableValidation = true` (and `enableSynchronizationValidation = true` while
   porting). The ported code should produce no validation messages. The old code had several
   synchronization bugs (see "Behavior changes"), so don't port its barriers or semaphores.

## Build

- Headers moved from `vulkan_test/` to `include/<area>/`. Add `-I<wrapper>/include` and include e.g.
  `"app/VulkanApp.h"`, `"graph/RenderGraph.h"`, `"materials/GraphicsMaterial.h"`.
- Shaders must be compiled with `-I<wrapper>/include/glsl` and `--target-env=vulkan1.3`.
- Devices must support Vulkan 1.3 plus the features listed in `include/core/DeviceFeatures.h`
  (descriptor indexing, synchronization2, dynamic rendering, timeline semaphores, storage images
  without a format, sampler anisotropy). Selection errors list what a device lacks.
- The old wrapper enabled few features, so shaders may rely on ones a validation layer now reports
  (e.g. 8- and 16-bit types in buffers). Request them with `AppConfig::features`, e.g.
  `config.features.vulkan12.storageBuffer8BitAccess = VK_TRUE`.
- One translation unit still defines `VK_WRAP_UTIL_IMPL` and `STB_IMAGE_IMPLEMENTATION`.
- Only `include/app` uses GLFW. Code that doesn't use `VulkanApp` doesn't need to link it.

## Mapping

### App and context

| Old | New |
|---|---|
| `VulkanApp<MAX_FRAMES> app(height, width); app.init(forceFullFloatColor)` | `VulkanApp app; app.init(AppConfig{...})` with `window = WindowConfig{title, width, height}` |
| `MAX_FRAMES` template argument everywhere | Runtime `ctx.framesInFlight()` (`AppConfig::framesInFlight`, `ctx.setFramesInFlight`). Remove the template arguments from every type. |
| `app.getDevice()`, `getPhysicalDevice()` | `app.getContext().device()`, `.physicalDevice()`. Most APIs now take `VulkanContext&` instead. |
| `app.getGraphicsQueue()`, `getComputeQueue()`, `getPresentQueue()` | Don't use raw queues: `ctx.submit(...)`, `ctx.present(...)`, `ctx.submitImmediate(fn)`. `ctx.queue(QueueType::...)` exists for interop, but host access to a raw queue must be synchronized with the context's own use of it, so prefer the context's functions. |
| `app.getCommandPool()` / `issueSingleTimeCommand(fn, queue, device, pool)` | `ctx.submitImmediate(fn)` for one-off work; `ctx.createCommandPool(...)` for your own recording |
| `app.getGraphicsCommandBuffers()` / `getComputeCommandBuffers()` | Gone: graph nodes record into command buffers the graph provides |
| `app.getSwapchainExtent()` | `app.getSurfaceExtent()` (or `FrameContext::surfaceExtent`) |
| `app.getRenderPass()` | Gone: dynamic rendering. Use `graph.surfaceTarget()` and `graph.formats(target)`. |
| `forceFullFloatColor` | Render into a graph image of the format you want (`createImage(..., VK_FORMAT_R32G32B32A32_SFLOAT, ...)`) |
| `app.addPreDrawCallback(cb)` | A CPU node: `graph.addCpu(name, setup, [](FrameContext& f) {...})` |
| `app.setRenderGraph(std::move(graph))` | `RenderGraph& graph = app.createGraph(); ... app.setActiveGraph(graph);` |
| Window size fixed at construction | `app.setWindowSize(w, h)`; `app.addSurfaceResizeListener(fn)` |

### Resources

| Old | New |
|---|---|
| `std::unique_ptr<Image>` / `std::unique_ptr<Buffer<T>>` out parameters | `Handle<Image>` / `Handle<Buffer<T>>` out parameters (reference counted; see `core/ResourceManager.h`) |
| `Image::createFromFile(out, path, queue, pool, device, physicalDevice)` | `Image::createFromFile(out, ctx, path)` (same for `createFromUcharBuffer`, `createEmpty`, `createEmptyRGBA`) |
| `Image::createEmpty*` used as a storage image | `Image::createStorage(out, ctx, format, w, h)`. Storage images can't be sRGB; use e.g. `R16G16B16A16_SFLOAT` or `R8G8B8A8_UNORM`. Sampled-only images no longer get storage usage. |
| `Image::transitionImageLayout(image, ..., queue, pool, device)` | `image->transitionLayout(old, new)`. Usually unnecessary: the render graph transitions images it uses. |
| `Buffer<T>::create(out, n, usage, props, device, physicalDevice)` | `Buffer<T>::create(out, ctx, n, usage, props)`; `createScoped` for staging buffers |
| `Buffer<T>::createAndInitialize(out, data, usage, device, physicalDevice, queue, pool)` | `Buffer<T>::createAndInitialize(out, ctx, data, usage)` |
| `VulkanSampler::createWithAddressMode(out, mode, device, physicalDevice)` (`unique_ptr`) | `VulkanSampler::createWithAddressMode(handle, ctx, mode)` (`Handle<VulkanSampler>`) |
| `std::array<VkImageView, MAX_FRAMES>` for per-frame images | `PerFrameHandles<Image>` collections (`ctx.resources().create<PerFrameHandles<Image>>(ctx, factory)`), or graph images with `FrameMode::PerFrame` |

### Descriptors and materials

The per-material descriptor sets are gone. `Descriptor`, `BufferDescriptor`,
`UniformBufferDescriptor`, `StorageImageDescriptor` and `CombinedImageSamplerDescriptor` have no
direct equivalents.

| Old | New |
|---|---|
| `std::vector<std::shared_ptr<Descriptor>>` passed to a material | `std::vector<MaterialBinding>`: each wraps a descriptor entry (`ctx.descriptors().sampledImage(img)`, `.sampler(s)`, `.storageImage(img)`, `.storageBuffer(buf)`), shared or per frame (`FrameBinding`), plus the access its shaders make |
| `UniformBufferDescriptor` + `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT` | A storage buffer (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`) per frame, its entries in a `PerFrameHandles<DescriptorEntry>`, and `MaterialBinding::storageBuffer(entries)`. In GLSL, `BINDLESS_BUFFER(readonly, Name, Type, name)` and `name[indices.x].data`. |
| `CombinedImageSamplerDescriptor(images, sampler)` | Two bindings, `sampled(entry)` and `sampler(entry)`, combined in GLSL as `sampler2D(bindlessTextures[i], bindlessSamplers[j])` |
| `StorageImageDescriptor` | `MaterialBinding::storageImage(entry, Access::StorageWrite / StorageRead / StorageReadWrite)`; GLSL `bindlessStorageImages[i]` (no format qualifier) |
| `layout(binding = N) ...` in GLSL | `#include "bindless.glsl"` and a push constant block of `uint` indices in binding order |
| `uboDescriptor->bindBuffer(frame, buffer)` then `populateDescriptorSet(frame)` (the "JANK" pattern) | Create the per-frame buffers and entries in the material's constructor and set `bindings_` there (see `TutorialMaterial` in the example) |
| `Material<MAX_FRAMES>(device, physicalDevice, descriptors)` | `Material(ctx, bindings)`. Subclasses implement `createPipeline()` only if they're new material kinds. |
| `BasicMaterial<MAX_FRAMES, N>(device, physicalDevice, descriptors, swapchainExtent, renderPass, vertSpirv, fragSpirv, binding, attributes)` | `GraphicsMaterial(ctx, bindings, graph.formats(target), ctx.shaders().get("x.vert"), ctx.shaders().get("x.frag"), GraphicsPipelineOptions{.vertexLayout = VertexLayout{{binding}, {attributes...}}})`. The old pipeline state (triangle lists, back faces culled, opaque, depth tested) is the options' default. |
| `ComputeMaterial<MAX_FRAMES>(device, physicalDevice, descriptors, spirv)` | `ComputeMaterial(ctx, bindings, ctx.shaders().get("x.comp"))` |
| `material->getDescriptorSet(frame)`, binding sets yourself | `material->bind(commandBuffer, frameIndex)` (pipeline and push constants). The graph binds the universal set; outside a graph, call `ctx.descriptors().bind(commandBuffer, bindPoint)`. |
| `MeshRenderable<V, MAX_FRAMES>(vertices, indices, std::move(material), device, physicalDevice, queue, pool)` with a `unique_ptr` material | `ctx.resources().create<MeshRenderable<V>>(vertices, indices, materialHandle, ctx)`. Materials are `Handle<Material>` and can be shared between renderables. |
| `update(frame, swapchainExtent)` called by each node | `update(frameIndex, surfaceExtent)` called once per frame per distinct material by the graph |

### Shaders

| Old | New |
|---|---|
| `readFile(absolutePath + "/x.spv")` (`FileUtil.h`) | `ctx.shaders().get("x.vert")`, with `AppConfig::shaderSearchPaths` (or `VulkanContextConfig::shaderSearchPaths`) pointing at the SPIR-V directory. Name SPIR-V files `<name>.<stage>.spv`. |
| Restart to pick up shader changes | `app.reloadShaders()` or `AppConfig::autoReloadShaders = true` |

### Render graph

| Old | New |
|---|---|
| `RenderGraph<MAX_FRAMES>(device)`, `addNode(std::make_unique<...>)` | `RenderGraph& graph = app.createGraph()` (or `RenderGraph graph(ctx)` headless); `graph.add<NodeType>(name, args...)` |
| `AcquireImageNode`, `PresentNode` | Remove them: acquire and present are automatic around the node writing `graph.surfaceTarget()` |
| `ComputeNode(material, device, queue, commandBuffers)` | `graph.add<DispatchNode>(name, material)` |
| `RenderableNode(renderable, device, queue, renderPass, commandBuffers)` / `RenderablesNode(...)` | `graph.add<RenderablesNode>(name, std::vector<Handle<Renderable>>{...}, renderTarget)` |
| `addEdge(a, b)` | Remove: order comes from declared resource uses (materials declare theirs). Use `graph.addDependency(a, b)` only for dependencies the graph can't see. |
| `flagNodeAsFrameBlocking(node)` | Remove: frames wait for their slot's previous work automatically |
| Custom `RenderNode` subclasses (`submit(RenderEvalContext&)`) | Subclass `GraphicsNode`/`ComputeNode`/`TransferNode`/`CpuNode`: declare uses in `setup(NodeBuilder&)`, record in `record(RecordContext&)` (no begin/end, submit or semaphores; graphics nodes are already inside rendering with the viewport set) |
| Images shared between nodes by `VkImageView` arrays | Import them (`graph.importImage(name, handleOrPerFrameHandles, ImageState::...)`) or create them in the graph (`graph.createImage(name, FrameMode::..., ...)`); give materials `graph.sampledEntry(id)` / `storageEntry(id)` |
| Offscreen render pass + framebuffer | A `RenderTarget` of graph images; `ReadbackNode` to read results back |

## Behavior changes to be aware of

- **Synchronization is derived, not hand written.** The old graph had real bugs that ported code
  shouldn't reintroduce: a one-element wait stage array for several semaphores, render-finished
  semaphores reused before presentation was done with them, and compute output read by the fragment
  shader without a sufficient wait. The new graph emits sync2 barriers and timeline waits from the
  declared uses. Undeclared uses are unsynchronized, so declare everything.
- **Images rest in declared layouts.** Imported images are returned to their `ImageState` at the end
  of every frame. A storage image you sample must be declared as `SampledRead` (it's transitioned to
  `SHADER_READ_ONLY_OPTIMAL` for that use), not sampled in `GENERAL` as the old example did.
- **More queues.** Compute work may run on a different queue from graphics. Resources are shared
  across queue families concurrently, so nothing changes for users, but code submitting on its own
  must use the context's timelines (`ctx.submit` returns a value to wait on).
- **Frames:** each `RenderGraph::execute()` is one frame (`ctx.beginFrame`/`advanceFrame`). Don't
  call `advanceFrame` yourself when using graphs.
- **Validation:** `AppConfig::enableValidation` turns on the Khronos layer, and
  `enableSynchronizationValidation` turns on hazard checking.
