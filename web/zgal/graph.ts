// A render graph for WebGPU: resources, and nodes recorded in order into one command encoder a
// frame. WebGPU synchronizes on its own, so nodes run in the order they were added (which is
// program order, as in the Vulkan RenderGraph) without declaring uses for barriers.

import type { Context } from './context.ts';
import {
  isDepthFormat,
  layoutEntries,
  storageFormats,
  workgroupsFor,
  writePush,
  type BoundFormats,
  type ComputeMaterialDesc,
  type GraphicsMaterialDesc,
  type MaterialDesc,
} from './material.ts';
import { decode, encode, stride, type Type } from './types.ts';

export interface FrameInfo {
  // Always 0: WebGPU's queue orders frames, so per-frame resources aren't duplicated
  readonly frameIndex: number;
  // One more each frame
  readonly frameNumber: number;
  // The surface's size: the canvas's, or one set headless; [0, 0] without either
  readonly surfaceExtent: readonly [number, number];
}

// A value computed each frame
export type FrameSource<T> = (frame: FrameInfo) => T;

export type Extent = { fixed: readonly [number, number] } | { surface: number };

export class GraphImage {
  texture: GPUTexture | null = null;
  // Bumped when the texture is recreated, so bind groups follow
  generation = 0;
  readonly name: string;
  readonly format: GPUTextureFormat;
  readonly extent: Extent | null;
  constructor(name: string, format: GPUTextureFormat, extent: Extent | null, imported: GPUTexture | null = null) {
    this.name = name;
    this.format = format;
    this.extent = extent;
    this.texture = imported;
  }
  get size(): [number, number] {
    return this.texture ? [this.texture.width, this.texture.height] : [0, 0];
  }
}

export class GraphBuffer {
  buffer: GPUBuffer | null = null;
  generation = 0;
  readonly name: string;
  readonly element: Type<any>;
  readonly count: number | FrameSource<number> | null;
  constructor(name: string, element: Type<any>, count: number | FrameSource<number> | null, imported: GPUBuffer | null = null) {
    this.name = name;
    this.element = element;
    this.count = count;
    this.buffer = imported;
  }
}

export type LoadOp = 'clear' | 'load' | 'dont-care';
export type StoreOp = 'store' | 'dont-care';

export interface ColorAttachment {
  readonly image: GraphImage | 'surface';
  readonly load: LoadOp;
  readonly store: StoreOp;
  readonly clear: readonly [number, number, number, number];
}

export interface DepthAttachment {
  readonly image: GraphImage;
  readonly load: LoadOp;
  readonly store: StoreOp;
  readonly clear: number;
}

export interface Target {
  readonly colors: readonly ColorAttachment[];
  readonly depth: DepthAttachment | null;
}

// A node: work recorded each frame it's enabled. prepare runs first, for every node, before any is
// recorded (writes to buffers the queue makes before the frame's commands); submitted runs after the
// frame is submitted.
export interface Node {
  readonly name: string;
  prepare?(frame: FrameInfo): void;
  record(encoder: GPUCommandEncoder, frame: FrameInfo): void;
  submitted?(frame: FrameInfo): void;
}

export type NodeId = number;

// A per-frame value of type T in a storage buffer, written when it changes
export class Uniform<T> {
  readonly resource: GraphBuffer;
  private last: Uint8Array | null = null;
  private lastFrame = -1;
  private readonly graph: RenderGraph;
  readonly type: Type<T>;
  private readonly source: FrameSource<T>;
  constructor(graph: RenderGraph, name: string, type: Type<T>, source: FrameSource<T>) {
    this.graph = graph;
    this.type = type;
    this.source = source;
    this.resource = graph.createBuffer(name, type, 1);
  }
  update(frame: FrameInfo): void {
    if (this.lastFrame === frame.frameNumber) return;
    this.lastFrame = frame.frameNumber;
    const bytes = encode(this.type, [this.source(frame)]);
    if (this.last && bytes.length === this.last.length && bytes.every((b, i) => b === this.last![i])) return;
    this.last = bytes;
    this.graph.context.device.queue.writeBuffer(this.graph.bufferOf(this.resource), 0, bytes);
  }
}

// What a material binding is bound to
export type Bound =
  | { readonly image: GraphImage }
  | { readonly buffer: GraphBuffer }
  | { readonly sampler: GPUSampler; readonly filtering: boolean }
  | { readonly uniform: Uniform<any> };

export interface PipelineOptions {
  readonly topology?: GPUPrimitiveTopology;
  readonly cull?: GPUCullMode;
  readonly frontFace?: GPUFrontFace;
  readonly blend?: 'opaque' | 'alpha' | 'premultiplied-alpha' | 'additive';
  readonly depthTest?: boolean;
  readonly depthWrite?: boolean;
  readonly depthCompare?: GPUCompareFunction;
}

export type PushValues = Record<string, unknown>;
// Gives push constants their values for a frame
export type PushSource = (frame: FrameInfo, push: PushValues) => void;

// A material bound to resources, with its push constants and dispatch size computed each frame
export class MaterialInstance {
  private pipelines = new Map<string, GPUComputePipeline | GPURenderPipeline>();
  private layouts = new Map<string, { group: GPUBindGroupLayout; pipeline: GPUPipelineLayout }>();
  readonly graph: RenderGraph;
  readonly desc: MaterialDesc;
  readonly bindings: Readonly<Record<string, Bound>>;
  readonly push: PushSource | null;
  readonly invocations: FrameSource<number[]> | null;
  readonly options: PipelineOptions;
  constructor(graph: RenderGraph, desc: MaterialDesc, bindings: Readonly<Record<string, Bound>>, push: PushSource | null, invocations: FrameSource<number[]> | null, options: PipelineOptions = {}) {
    this.graph = graph;
    this.desc = desc;
    this.bindings = bindings;
    this.push = push;
    this.invocations = invocations;
    this.options = options;
    for (const b of desc.bindings) {
      if (!(b.name in bindings)) throw new Error(`material ${desc.name}'s binding ${b.name} isn't given`);
    }
  }

  formats(): BoundFormats {
    const textures: Record<string, GPUTextureFormat> = {};
    const samplers: Record<string, 'filtering' | 'non-filtering'> = {};
    let unfilterable = false;
    const filterable = this.graph.context.hasFeature('float32-filterable');
    for (const b of this.desc.bindings) {
      const bound = this.bindings[b.name];
      if ('image' in bound) {
        textures[b.name] = bound.image.format;
        if (b.kind === 'texture' && !b.depth && (isDepthFormat(bound.image.format) || (!filterable && /32float|32uint|32sint/.test(bound.image.format)))) {
          unfilterable = true;
        }
      }
    }
    for (const b of this.desc.bindings) {
      const bound = this.bindings[b.name];
      if ('sampler' in bound) samplers[b.name] = unfilterable || !bound.filtering ? 'non-filtering' : 'filtering';
    }
    return { textures, samplers };
  }

  private layout(formats: BoundFormats) {
    const key = JSON.stringify(formats);
    let layout = this.layouts.get(key);
    if (!layout) {
      const device = this.graph.context.device;
      const group = device.createBindGroupLayout({ label: this.desc.name, entries: layoutEntries(device, this.desc, formats) });
      layout = { group, pipeline: device.createPipelineLayout({ label: this.desc.name, bindGroupLayouts: [group] }) };
      this.layouts.set(key, layout);
    }
    return layout;
  }

  computePipeline(): GPUComputePipeline {
    const desc = this.desc as ComputeMaterialDesc;
    const formats = this.formats();
    const key = JSON.stringify(formats);
    let p = this.pipelines.get(key) as GPUComputePipeline | undefined;
    if (!p) {
      const device = this.graph.context.device;
      const module = device.createShaderModule({ label: `${desc.name}.comp`, code: desc.compute(storageFormats(desc, formats)) });
      p = device.createComputePipeline({ label: desc.name, layout: this.layout(formats).pipeline, compute: { module, entryPoint: 'main' } });
      this.pipelines.set(key, p);
    }
    return p;
  }

  renderPipeline(target: Target): GPURenderPipeline {
    const desc = this.desc as GraphicsMaterialDesc;
    const formats = this.formats();
    const colorFormats = target.colors.map((c) => this.graph.attachmentFormat(c.image));
    const depthFormat = target.depth ? target.depth.image.format : null;
    const key = JSON.stringify([formats, colorFormats, depthFormat]);
    let p = this.pipelines.get(key) as GPURenderPipeline | undefined;
    if (!p) {
      const device = this.graph.context.device;
      const storage = storageFormats(desc, formats);
      const o = this.options;
      const blend = blendState(o.blend ?? 'opaque');
      p = device.createRenderPipeline({
        label: desc.name,
        layout: this.layout(formats).pipeline,
        vertex: {
          module: device.createShaderModule({ label: `${desc.name}.vert`, code: desc.vertex(storage) }),
          entryPoint: 'main',
          buffers: desc.vertexInput
            ? [{ arrayStride: desc.vertexInput.stride, attributes: desc.vertexInput.attributes.map((a) => ({ shaderLocation: a.location, offset: a.offset, format: a.format })) }]
            : [],
        },
        fragment: {
          module: device.createShaderModule({ label: `${desc.name}.frag`, code: desc.fragment(storage) }),
          entryPoint: 'main',
          targets: colorFormats.map((format) => (blend ? { format, blend } : { format })),
        },
        primitive: { topology: o.topology ?? 'triangle-list', cullMode: o.cull ?? 'none', frontFace: o.frontFace ?? 'ccw' },
        depthStencil: depthFormat
          ? {
            format: depthFormat,
            depthWriteEnabled: o.depthWrite ?? o.depthTest ?? true,
            depthCompare: (o.depthTest ?? true) ? (o.depthCompare ?? 'less') : 'always',
          }
          : undefined,
      });
      this.pipelines.set(key, p);
    }
    return p;
  }

  // A bind group of its bindings, and a push buffer (written each frame) for one node using it
  binder(nodeName: string): MaterialBinder {
    return new MaterialBinder(this, nodeName);
  }

  groupLayout(): GPUBindGroupLayout {
    return this.layout(this.formats()).group;
  }
}

// One node's use of a material instance: the bind group (rebuilt when resources are recreated)
// and its push constants
export class MaterialBinder {
  private group: GPUBindGroup | null = null;
  private key = '';
  private pushBuffer: GPUBuffer | null = null;
  private pushBytes: Uint8Array<ArrayBuffer> | null = null;
  private lastPush: Uint8Array | null = null;
  readonly instance: MaterialInstance;
  readonly nodeName: string;
  constructor(instance: MaterialInstance, nodeName: string) {
    this.instance = instance;
    this.nodeName = nodeName;
    const desc = instance.desc;
    if (desc.push) {
      this.pushBuffer = instance.graph.context.device.createBuffer({
        label: `${nodeName} push constants`,
        size: desc.push.size,
        usage: 0x40 | 0x8, // UNIFORM | COPY_DST
      });
      this.pushBytes = new Uint8Array(desc.push.size);
    }
  }

  // Updates the instance's uniforms and writes the push constants: the instance's, then the
  // node's own (override)
  prepare(frame: FrameInfo, override: PushSource | null = null): void {
    const { instance } = this;
    for (const b of Object.values(instance.bindings)) {
      if ('uniform' in b) b.uniform.update(frame);
    }
    if (!this.pushBuffer || !this.pushBytes) return;
    const values: PushValues = {};
    instance.push?.(frame, values);
    override?.(frame, values);
    for (const name of Object.keys(instance.desc.push!.fields)) {
      if (!(name in values)) throw new Error(`${this.nodeName}: material ${instance.desc.name}'s push constant ${name} isn't given`);
    }
    writePush(instance.desc, values, new DataView(this.pushBytes.buffer));
    if (this.lastPush && this.lastPush.every((b, i) => b === this.pushBytes![i])) return;
    this.lastPush = this.pushBytes.slice();
    instance.graph.context.device.queue.writeBuffer(this.pushBuffer, 0, this.pushBytes);
  }

  bindGroup(): GPUBindGroup {
    const { instance } = this;
    const graph = instance.graph;
    const desc = instance.desc;
    const key = desc.bindings
      .map((b) => {
        const bound = instance.bindings[b.name];
        if ('image' in bound) return `i${bound.image.generation}`;
        if ('buffer' in bound) return `b${bound.buffer.generation}`;
        if ('uniform' in bound) return `u${bound.uniform.resource.generation}`;
        return 's';
      })
      .join(',');
    if (!this.group || key !== this.key) {
      const entries: GPUBindGroupEntry[] = desc.bindings.map((b, i) => {
        const bound = instance.bindings[b.name];
        if ('image' in bound) return { binding: i, resource: graph.viewOf(bound.image) };
        if ('buffer' in bound) return { binding: i, resource: { buffer: graph.bufferOf(bound.buffer) } };
        if ('uniform' in bound) return { binding: i, resource: { buffer: graph.bufferOf(bound.uniform.resource) } };
        return { binding: i, resource: bound.sampler };
      });
      if (desc.push) entries.push({ binding: desc.push.binding, resource: { buffer: this.pushBuffer! } });
      this.group = graph.context.device.createBindGroup({ label: this.nodeName, layout: instance.groupLayout(), entries });
      this.key = key;
    }
    return this.group;
  }

  destroy(): void {
    this.pushBuffer?.destroy();
  }
}

function blendState(mode: NonNullable<PipelineOptions['blend']>): GPUBlendState | null {
  switch (mode) {
    case 'opaque':
      return null;
    case 'alpha':
      return {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
      };
    case 'premultiplied-alpha':
      return {
        color: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
      };
    case 'additive':
      return { color: { srcFactor: 'one', dstFactor: 'one', operation: 'add' }, alpha: { srcFactor: 'one', dstFactor: 'one', operation: 'add' } };
  }
}

// Where a graph shows its surface: a canvas's context, configured by the graph
export interface Surface {
  readonly canvas: GPUCanvasContext;
}

interface NodeRecord {
  readonly node: Node;
  enabled: boolean;
  once: boolean;
  // Nodes taking this one's enabled state (enableWith)
  followers: NodeId[];
  leader: NodeId | null;
}

export class RenderGraph {
  private images: GraphImage[] = [];
  private buffers: GraphBuffer[] = [];
  private nodes: NodeRecord[] = [];
  private frameNumber = 0;
  private surface: Surface | null = null;
  private surfaceFormat: GPUTextureFormat | null = null;
  private surfaceTexture: GPUTexture | null = null;
  private surfaceView: GPUTextureView | null = null;
  private headlessExtent: [number, number] = [0, 0];
  private views = new Map<GraphImage, { generation: number; view: GPUTextureView }>();
  private inFlight: Promise<undefined>[] = [];
  private owned: { destroy(): void }[] = [];

  readonly context: Context;

  constructor(context: Context) {
    this.context = context;
  }

  // Shows the surface in a canvas: the graph configures it and draws into an sRGB view, so shaders
  // output linear values
  setSurface(canvas: GPUCanvasContext): void {
    const format = this.context.preferredSurfaceFormat();
    const srgb = `${format}-srgb` as GPUTextureFormat;
    canvas.configure({ device: this.context.device, format, viewFormats: [srgb], alphaMode: 'opaque' });
    this.surface = { canvas };
    this.surfaceFormat = srgb;
  }

  // The surface's size headless, for surface-relative images and FrameInfo
  setSurfaceExtent(extent: readonly [number, number]): void {
    this.headlessExtent = [extent[0], extent[1]];
  }

  surfaceExtent(): [number, number] {
    if (this.surface) {
      const c = this.surface.canvas.canvas as { width: number; height: number };
      return [c.width, c.height];
    }
    return this.headlessExtent;
  }

  hasSurface(): boolean {
    return this.surface !== null;
  }

  createImage(name: string, format: GPUTextureFormat, extent: Extent): GraphImage {
    const image = new GraphImage(name, format, extent);
    this.images.push(image);
    return image;
  }

  importImage(name: string, texture: GPUTexture): GraphImage {
    const image = new GraphImage(name, texture.format, null, texture);
    this.images.push(image);
    return image;
  }

  // A buffer of `count` elements (a number, or computed each frame: the buffer is recreated, its
  // contents discarded, when it changes)
  createBuffer(name: string, element: Type<any>, count: number | FrameSource<number>): GraphBuffer {
    const buffer = new GraphBuffer(name, element, count);
    this.buffers.push(buffer);
    return buffer;
  }

  // A buffer the application owns, or the graph if owned (destroyed with it)
  importBuffer(name: string, element: Type<any>, buffer: GPUBuffer, owned = false): GraphBuffer {
    const b = new GraphBuffer(name, element, null, buffer);
    this.buffers.push(b);
    if (owned) this.owned.push(buffer);
    return b;
  }

  // A buffer created now holding elements
  upload<T>(name: string, element: Type<T>, elements: readonly T[]): GraphBuffer {
    const bytes = encode(element, elements);
    const buffer = this.context.device.createBuffer({ label: name, size: bytes.length, usage: bufferUsage });
    this.context.device.queue.writeBuffer(buffer, 0, bytes);
    return this.importBuffer(name, element, buffer, true);
  }

  sampler(descriptor: GPUSamplerDescriptor): GPUSampler {
    return this.context.device.createSampler(descriptor);
  }

  uniform<T>(name: string, type: Type<T>, source: FrameSource<T>): Uniform<T> {
    return new Uniform(this, name, type, source);
  }

  surfaceTarget(clear: readonly [number, number, number, number], depth: GPUTextureFormat | null): Target {
    if (!this.surface) throw new Error('the graph has no surface (setSurface): a headless graph can\'t draw to one');
    return {
      colors: [{ image: 'surface', load: 'clear', store: 'store', clear }],
      depth: depth ? { image: this.createImage('surface depth', depth, { surface: 1 }), load: 'clear', store: 'dont-care', clear: 1 } : null,
    };
  }

  add(node: Node): NodeId {
    this.nodes.push({ node, enabled: true, once: false, followers: [], leader: null });
    return this.nodes.length - 1;
  }

  node<T extends Node>(id: NodeId): T {
    return this.nodes[id].node as T;
  }

  // Enables or disables a node and its followers, cancelling a pending runOnce
  setEnabled(id: NodeId, enabled: boolean): void {
    const r = this.nodes[id];
    r.enabled = enabled;
    r.once = false;
    for (const f of r.followers) this.setEnabled(f, enabled);
  }

  isEnabled(id: NodeId): boolean {
    return this.nodes[id].enabled;
  }

  // Whether the node runs once and hasn't yet
  isPendingOnce(id: NodeId): boolean {
    return this.nodes[id].once;
  }

  // Enables a node (and its followers) for the next frame, then disables it
  runOnce(id: NodeId): void {
    const r = this.nodes[id];
    r.enabled = true;
    r.once = true;
    for (const f of r.followers) this.runOnce(f);
  }

  // Makes follower take leader's enabled state, now and whenever setEnabled or runOnce changes it
  enableWith(follower: NodeId, leader: NodeId): void {
    const following = this.nodes[follower];
    const leading = this.nodes[leader];
    if (following.leader !== null) throw new Error(`node '${following.node.name}' already follows another`);
    for (let at: NodeId | null = leader; at !== null; at = this.nodes[at].leader) {
      if (at === follower) throw new Error(`node '${following.node.name}' can't follow a node following it`);
    }
    following.leader = leader;
    following.enabled = leading.enabled;
    following.once = leading.once;
    leading.followers.push(follower);
  }

  attachmentFormat(image: GraphImage | 'surface'): GPUTextureFormat {
    if (image === 'surface') return this.surfaceFormat!;
    return image.format;
  }

  imageExtent(image: GraphImage): [number, number] {
    this.ensureImage(image, this.frameInfo());
    return image.size;
  }

  bufferOf(b: GraphBuffer): GPUBuffer {
    if (!b.buffer) this.ensureBuffer(b, this.frameInfo());
    return b.buffer!;
  }

  textureOf(image: GraphImage): GPUTexture {
    if (!image.texture) this.ensureImage(image, this.frameInfo());
    return image.texture!;
  }

  viewOf(image: GraphImage): GPUTextureView {
    const texture = this.textureOf(image);
    const cached = this.views.get(image);
    if (cached && cached.generation === image.generation) return cached.view;
    const view = texture.createView();
    this.views.set(image, { generation: image.generation, view });
    return view;
  }

  attachmentView(image: GraphImage | 'surface'): GPUTextureView {
    if (image === 'surface') {
      if (!this.surfaceView) throw new Error('the surface is drawn outside a frame');
      return this.surfaceView;
    }
    return this.viewOf(image);
  }

  private frameInfo(): FrameInfo {
    return { frameIndex: 0, frameNumber: this.frameNumber, surfaceExtent: this.surfaceExtent() };
  }

  private ensureImage(image: GraphImage, frame: FrameInfo): void {
    if (!image.extent) return;
    const [w, h] =
      'fixed' in image.extent
        ? image.extent.fixed
        : [Math.max(1, Math.floor(frame.surfaceExtent[0] * image.extent.surface)), Math.max(1, Math.floor(frame.surfaceExtent[1] * image.extent.surface))];
    if (image.texture && image.texture.width === w && image.texture.height === h) return;
    image.texture?.destroy();
    image.texture = this.context.device.createTexture({
      label: image.name,
      size: [w, h],
      format: image.format,
      usage: textureUsage(this.context, image.format),
    });
    image.generation++;
  }

  private ensureBuffer(b: GraphBuffer, frame: FrameInfo): void {
    if (b.count === null) return;
    const count = typeof b.count === 'number' ? b.count : b.count(frame);
    const size = Math.max(4, Math.ceil((count * stride(b.element)) / 4) * 4);
    if (b.buffer && b.buffer.size === size) return;
    b.buffer?.destroy();
    b.buffer = this.context.device.createBuffer({ label: b.name, size, usage: bufferUsage });
    b.generation++;
  }

  // Records and submits a frame: every enabled node in order. Waits only when the context's frames
  // in flight already are.
  async execute(): Promise<void> {
    while (this.inFlight.length >= this.context.framesInFlight) await this.inFlight.shift();
    if (this.surface) {
      this.surfaceTexture = this.surface.canvas.getCurrentTexture();
      this.surfaceView = this.surfaceTexture.createView({ format: this.surfaceFormat! });
    }
    const frame = this.frameInfo();
    for (const image of this.images) this.ensureImage(image, frame);
    for (const b of this.buffers) this.ensureBuffer(b, frame);
    const active = this.nodes.filter((r) => r.enabled);
    for (const r of active) r.node.prepare?.(frame);
    const encoder = this.context.device.createCommandEncoder({ label: `frame ${frame.frameNumber}` });
    for (const r of active) {
      encoder.pushDebugGroup(r.node.name);
      r.node.record(encoder, frame);
      encoder.popDebugGroup();
    }
    this.context.device.queue.submit([encoder.finish()]);
    for (const r of active) r.node.submitted?.(frame);
    // Nodes that ran once are done
    for (const r of active) {
      if (r.once) {
        r.once = false;
        r.enabled = false;
      }
    }
    this.inFlight.push(this.context.device.queue.onSubmittedWorkDone());
    this.surfaceView = null;
    this.frameNumber++;
  }

  // Waits for the frames submitted so far
  async finish(): Promise<void> {
    await this.context.device.queue.onSubmittedWorkDone();
    this.inFlight = [];
  }

  destroy(): void {
    for (const r of this.nodes) (r.node as { destroy?(): void }).destroy?.();
    for (const image of this.images) if (image.extent) image.texture?.destroy();
    for (const b of this.buffers) if (b.count !== null) b.buffer?.destroy();
    for (const o of this.owned) o.destroy();
  }
}

const bufferUsage = 0x80 | 0x4 | 0x8 | 0x100 | 0x20; // STORAGE | COPY_SRC | COPY_DST | INDIRECT | VERTEX

const storageFormats_ = new Set<GPUTextureFormat>([
  'rgba8unorm', 'rgba8snorm', 'rgba8uint', 'rgba8sint', 'rgba16uint', 'rgba16sint', 'rgba16float',
  'r32uint', 'r32sint', 'r32float', 'rg32uint', 'rg32sint', 'rg32float', 'rgba32uint', 'rgba32sint', 'rgba32float',
]);

function textureUsage(context: Context, format: GPUTextureFormat): number {
  // COPY_SRC | COPY_DST | TEXTURE_BINDING | RENDER_ATTACHMENT, and STORAGE_BINDING where the format allows
  let usage = 0x1 | 0x2 | 0x4 | 0x10;
  if (storageFormats_.has(format) || (format === 'bgra8unorm' && context.hasFeature('bgra8unorm-storage'))) usage |= 0x8;
  return usage;
}

// ---------------------------------------------------------------------------------------------
// Nodes

// Dispatches a compute material: its own push constants (override) and size, or its instance's
export class DispatchNode implements Node {
  private readonly binder: MaterialBinder;
  readonly name: string;
  readonly material: MaterialInstance;
  readonly override: PushSource | null;
  readonly invocations: FrameSource<number[]> | null;
  constructor(name: string, material: MaterialInstance, override: PushSource | null = null, invocations: FrameSource<number[]> | null = null) {
    this.name = name;
    this.material = material;
    this.override = override;
    this.invocations = invocations;
    this.binder = material.binder(name);
  }
  private workgroups: [number, number, number] = [0, 0, 0];
  prepare(frame: FrameInfo): void {
    this.binder.prepare(frame, this.override);
    const invocations = this.invocations ?? this.material.invocations;
    if (!invocations) throw new Error(`${this.name}: material ${this.material.desc.name} has no dispatch size`);
    this.workgroups = workgroupsFor(this.material.desc as ComputeMaterialDesc, invocations(frame));
  }
  record(encoder: GPUCommandEncoder): void {
    if (this.workgroups.some((n) => n === 0)) return;
    const pass = encoder.beginComputePass({ label: this.name });
    pass.setPipeline(this.material.computePipeline());
    pass.setBindGroup(0, this.binder.bindGroup());
    pass.dispatchWorkgroups(...this.workgroups);
    pass.end();
  }
  destroy(): void {
    this.binder.destroy();
  }
}

function beginRenderPass(graph: RenderGraph, encoder: GPUCommandEncoder, target: Target, label: string): GPURenderPassEncoder {
  const op = (load: LoadOp): GPULoadOp => (load === 'load' ? 'load' : 'clear');
  return encoder.beginRenderPass({
    label,
    colorAttachments: target.colors.map((c) => ({
      view: graph.attachmentView(c.image),
      loadOp: op(c.load),
      storeOp: c.store === 'store' ? 'store' : 'discard',
      clearValue: c.clear as [number, number, number, number],
    })),
    depthStencilAttachment: target.depth
      ? {
        view: graph.attachmentView(target.depth.image),
        depthLoadOp: op(target.depth.load),
        depthStoreOp: target.depth.store === 'store' ? 'store' : 'discard',
        depthClearValue: target.depth.clear,
      }
      : undefined,
  });
}

// Draws count vertices (and instances) of a graphics material into a target, without vertex buffers
export class DrawNode implements Node {
  private readonly binder: MaterialBinder;
  private vertices = 0;
  readonly name: string;
  readonly target: Target;
  readonly material: MaterialInstance;
  readonly count: FrameSource<number>;
  readonly override: PushSource | null;
  constructor(name: string, target: Target, material: MaterialInstance, count: FrameSource<number>, override: PushSource | null = null) {
    this.name = name;
    this.target = target;
    this.material = material;
    this.count = count;
    this.override = override;
    this.binder = material.binder(name);
  }
  prepare(frame: FrameInfo): void {
    this.binder.prepare(frame, this.override);
    this.vertices = this.count(frame);
  }
  record(encoder: GPUCommandEncoder): void {
    const graph = this.material.graph;
    const pass = beginRenderPass(graph, encoder, this.target, this.name);
    pass.setPipeline(this.material.renderPipeline(this.target));
    pass.setBindGroup(0, this.binder.bindGroup());
    if (this.vertices > 0) pass.draw(this.vertices);
    pass.end();
  }
  destroy(): void {
    this.binder.destroy();
  }
}

// Copies of a resource into buffers the CPU maps, one per frame in flight
class Staging {
  private free: GPUBuffer[] = [];
  private readonly device: GPUDevice;
  private readonly label: string;
  constructor(device: GPUDevice, label: string) {
    this.device = device;
    this.label = label;
  }
  take(size: number): GPUBuffer {
    const i = this.free.findIndex((b) => b.size === size);
    if (i >= 0) return this.free.splice(i, 1)[0];
    return this.device.createBuffer({ label: this.label, size, usage: 0x1 | 0x8 }); // MAP_READ | COPY_DST
  }
  give(b: GPUBuffer): void {
    this.free.push(b);
  }
  destroy(): void {
    for (const b of this.free) b.destroy();
  }
}

// Copies a buffer's elements each frame, which the CPU reads once the copy finishes
export class BufferReadbackNode implements Node {
  private readonly staging: Staging;
  private pending: GPUBuffer | null = null;
  private latest: Promise<Uint8Array<ArrayBuffer>> | null = null;
  private finished: Uint8Array<ArrayBuffer> | null = null;
  private taken = true;
  readonly name: string;
  readonly graph: RenderGraph;
  readonly source: GraphBuffer;
  constructor(name: string, graph: RenderGraph, source: GraphBuffer) {
    this.name = name;
    this.graph = graph;
    this.source = source;
    this.staging = new Staging(graph.context.device, `${name} staging`);
  }
  record(encoder: GPUCommandEncoder): void {
    const buffer = this.graph.bufferOf(this.source);
    const staging = this.staging.take(buffer.size);
    encoder.copyBufferToBuffer(buffer, 0, staging, 0, buffer.size);
    this.pending = staging;
  }
  submitted(): void {
    const staging = this.pending!;
    this.pending = null;
    this.latest = staging.mapAsync(1 /* GPUMapMode.READ */).then(() => {
      const bytes = new Uint8Array(staging.getMappedRange().slice(0));
      staging.unmap();
      this.staging.give(staging);
      this.finished = bytes;
      this.taken = false;
      return bytes;
    });
  }
  // The elements of the most recent frame's copy (waiting for it)
  async contents<T>(): Promise<T[]> {
    if (!this.latest) throw new Error(`readback ${this.name} hasn't run`);
    return decode(this.source.element, await this.latest) as T[];
  }
  // The newest finished copy's elements not taken before, without waiting, or null
  takeFinished<T>(): T[] | null {
    if (this.taken || !this.finished) return null;
    this.taken = true;
    return decode(this.source.element, this.finished) as T[];
  }
  destroy(): void {
    this.staging.destroy();
  }
}

// Bytes per texel of the formats readbacks and copies handle
function texelBytes(format: GPUTextureFormat): number {
  if (/^(r8|r8unorm)$/.test(format)) return 1;
  if (/^(rg8unorm|r16float)$/.test(format)) return 2;
  if (/^(rgba8unorm|rgba8unorm-srgb|bgra8unorm|bgra8unorm-srgb|rg16float|r32float|r32uint|r32sint|depth32float)$/.test(format)) return 4;
  if (/^(rgba16float|rg32float)$/.test(format)) return 8;
  if (format === 'rgba32float') return 16;
  throw new Error(`images of format ${format} can't be copied`);
}

// Copies an image each frame, whose texels the CPU reads once the copy finishes
export class ReadbackNode implements Node {
  private readonly staging: Staging;
  private pending: { buffer: GPUBuffer; width: number; height: number; row: number } | null = null;
  private latest: Promise<Uint8Array<ArrayBuffer>> | null = null;
  private finished: { pixels: Uint8Array<ArrayBuffer>; size: [number, number] } | null = null;
  private latestSize: [number, number] = [0, 0];
  private taken = true;
  readonly name: string;
  readonly graph: RenderGraph;
  readonly source: GraphImage;
  constructor(name: string, graph: RenderGraph, source: GraphImage) {
    this.name = name;
    this.graph = graph;
    this.source = source;
    this.staging = new Staging(graph.context.device, `${name} staging`);
  }
  record(encoder: GPUCommandEncoder): void {
    const texture = this.graph.textureOf(this.source);
    const bytes = texelBytes(texture.format);
    const row = Math.ceil((texture.width * bytes) / 256) * 256;
    const staging = this.staging.take(row * texture.height);
    encoder.copyTextureToBuffer({ texture }, { buffer: staging, bytesPerRow: row }, [texture.width, texture.height]);
    this.pending = { buffer: staging, width: texture.width, height: texture.height, row };
  }
  submitted(): void {
    const p = this.pending!;
    this.pending = null;
    const bytes = texelBytes(this.source.format);
    this.latestSize = [p.width, p.height];
    this.latest = p.buffer.mapAsync(1 /* GPUMapMode.READ */).then(() => {
      const mapped = new Uint8Array(p.buffer.getMappedRange());
      const pixels = new Uint8Array(p.width * p.height * bytes);
      for (let y = 0; y < p.height; y++) pixels.set(mapped.subarray(y * p.row, y * p.row + p.width * bytes), y * p.width * bytes);
      p.buffer.unmap();
      this.staging.give(p.buffer);
      this.finished = { pixels, size: [p.width, p.height] };
      this.taken = false;
      return pixels;
    });
  }
  // The texels of the most recent frame's copy, rows top to bottom (waiting for it)
  async pixels(): Promise<Uint8Array<ArrayBuffer>> {
    if (!this.latest) throw new Error(`readback ${this.name} hasn't run`);
    return this.latest;
  }
  // The most recent copy's size
  size(): [number, number] {
    return this.latestSize;
  }
  // The newest finished copy's texels and size not taken before, without waiting, or null
  takeFinished(): { pixels: Uint8Array<ArrayBuffer>; size: [number, number] } | null {
    if (this.taken || !this.finished) return null;
    this.taken = true;
    return this.finished;
  }
  destroy(): void {
    this.staging.destroy();
  }
}

// Copies elements between buffers: count of them (all of the source's from its offset when null),
// at element offsets
export class CopyNode implements Node {
  private args: { from: number; to: number; count: number | null } = { from: 0, to: 0, count: null };
  readonly name: string;
  readonly graph: RenderGraph;
  readonly source: GraphBuffer;
  readonly destination: GraphBuffer;
  readonly sourceOffset: FrameSource<number>;
  readonly destinationOffset: FrameSource<number>;
  readonly count: FrameSource<number> | null;
  constructor(name: string, graph: RenderGraph, source: GraphBuffer, destination: GraphBuffer, sourceOffset: FrameSource<number>, destinationOffset: FrameSource<number>, count: FrameSource<number> | null) {
    this.name = name;
    this.graph = graph;
    this.source = source;
    this.destination = destination;
    this.sourceOffset = sourceOffset;
    this.destinationOffset = destinationOffset;
    this.count = count;
  }
  prepare(frame: FrameInfo): void {
    this.args = { from: this.sourceOffset(frame), to: this.destinationOffset(frame), count: this.count ? this.count(frame) : null };
  }
  record(encoder: GPUCommandEncoder): void {
    const s = stride(this.source.element);
    const source = this.graph.bufferOf(this.source);
    const destination = this.graph.bufferOf(this.destination);
    const from = this.args.from * s;
    const to = this.args.to * s;
    const bytes = this.args.count !== null ? this.args.count * s : source.size - from;
    if (from + bytes > source.size || to + bytes > destination.size) {
      throw new Error(`copy ${this.name}: ${bytes} bytes from ${from} and to ${to} don't fit buffers of ${source.size} and ${destination.size}`);
    }
    if (bytes > 0) encoder.copyBufferToBuffer(source, from, destination, to, Math.ceil(bytes / 4) * 4);
  }
}

// Copies a buffer's elements into an image's texels: a region (the whole image by default) at an
// offset, from an element offset
export class CopyToImageNode implements Node {
  private scratch: GPUBuffer | null = null;
  private args = { from: 0, x: 0, y: 0, extent: null as [number, number] | null };
  readonly name: string;
  readonly graph: RenderGraph;
  readonly source: GraphBuffer;
  readonly destination: GraphImage;
  readonly sourceOffset: FrameSource<number>;
  readonly offset: FrameSource<[number, number]>;
  readonly extent: FrameSource<[number, number]> | null;
  constructor(name: string, graph: RenderGraph, source: GraphBuffer, destination: GraphImage, sourceOffset: FrameSource<number>, offset: FrameSource<[number, number]>, extent: FrameSource<[number, number]> | null) {
    this.name = name;
    this.graph = graph;
    this.source = source;
    this.destination = destination;
    this.sourceOffset = sourceOffset;
    this.offset = offset;
    this.extent = extent;
  }
  prepare(frame: FrameInfo): void {
    const [x, y] = this.offset(frame);
    this.args = { from: this.sourceOffset(frame), x, y, extent: this.extent ? this.extent(frame) : null };
  }
  record(encoder: GPUCommandEncoder): void {
    const texture = this.graph.textureOf(this.destination);
    const bytes = texelBytes(texture.format);
    const [w, h] = this.args.extent ?? [texture.width - this.args.x, texture.height - this.args.y];
    const source = this.graph.bufferOf(this.source);
    const from = this.args.from * stride(this.source.element);
    const packedRow = w * bytes;
    const row = Math.ceil(packedRow / 256) * 256;
    let buffer = source;
    let offset = from;
    if (row !== packedRow || from % 256 !== 0) {
      // WebGPU copies rows 256 bytes apart: copy the rows apart first
      if (!this.scratch || this.scratch.size < row * h) {
        this.scratch?.destroy();
        this.scratch = this.graph.context.device.createBuffer({ label: `${this.name} rows`, size: row * h, usage: bufferUsage });
      }
      for (let y = 0; y < h; y++) encoder.copyBufferToBuffer(source, from + y * packedRow, this.scratch, y * row, packedRow);
      buffer = this.scratch;
      offset = 0;
    }
    encoder.copyBufferToTexture({ buffer, offset, bytesPerRow: row }, { texture, origin: [this.args.x, this.args.y] }, [w, h]);
  }
  destroy(): void {
    this.scratch?.destroy();
  }
}

// Fills a buffer with a uint (WebGPU only clears to zero, so others are a compute pass)
export class ClearNode implements Node {
  private value = 0;
  private pipeline: GPUComputePipeline | null = null;
  private params: GPUBuffer | null = null;
  readonly name: string;
  readonly graph: RenderGraph;
  readonly target: GraphBuffer;
  readonly source: FrameSource<number>;
  constructor(name: string, graph: RenderGraph, target: GraphBuffer, source: FrameSource<number>) {
    this.name = name;
    this.graph = graph;
    this.target = target;
    this.source = source;
  }
  prepare(frame: FrameInfo): void {
    this.value = this.source(frame) >>> 0;
    if (this.value !== 0) {
      const device = this.graph.context.device;
      this.params ??= device.createBuffer({ label: `${this.name} value`, size: 16, usage: 0x40 | 0x8 }); // UNIFORM | COPY_DST
      device.queue.writeBuffer(this.params, 0, new Uint32Array([this.value, 0, 0, 0]));
    }
  }
  record(encoder: GPUCommandEncoder): void {
    const buffer = this.graph.bufferOf(this.target);
    if (this.value === 0) {
      encoder.clearBuffer(buffer);
      return;
    }
    const device = this.graph.context.device;
    this.pipeline ??= device.createComputePipeline({
      label: 'zgal fill',
      layout: 'auto',
      compute: {
        module: device.createShaderModule({
          code: `@group(0) @binding(0) var<storage, read_write> words: array<u32>;
@group(0) @binding(1) var<uniform> value: vec4u;
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) {
    let i = id.x + id.y * 65535u * 64u;
    if (i < arrayLength(&words)) { words[i] = value.x; }
}`,
        }),
        entryPoint: 'main',
      },
    });
    const words = buffer.size / 4;
    const groups = Math.ceil(words / 64);
    const pass = encoder.beginComputePass({ label: this.name });
    pass.setPipeline(this.pipeline);
    pass.setBindGroup(0, device.createBindGroup({ layout: this.pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer } }, { binding: 1, resource: { buffer: this.params! } }] }));
    pass.dispatchWorkgroups(Math.min(groups, 65535), Math.ceil(groups / 65535));
    pass.end();
  }
  destroy(): void {
    this.params?.destroy();
  }
}

// A per-frame data input's elements in a buffer sized for them, written when they change
export class DataUploadNode<T> implements Node {
  private last: readonly T[] | null = null;
  readonly name: string;
  readonly graph: RenderGraph;
  readonly target: GraphBuffer;
  readonly data: FrameSource<readonly T[]>;
  constructor(name: string, graph: RenderGraph, target: GraphBuffer, data: FrameSource<readonly T[]>) {
    this.name = name;
    this.graph = graph;
    this.target = target;
    this.data = data;
  }
  prepare(frame: FrameInfo): void {
    const elements = this.data(frame);
    const buffer = this.graph.bufferOf(this.target);
    if (elements === this.last && this.target.generation === this.uploadedGeneration) return;
    this.last = elements;
    this.uploadedGeneration = this.target.generation;
    if (elements.length > 0) this.graph.context.device.queue.writeBuffer(buffer, 0, encode(this.target.element, elements));
  }
  private uploadedGeneration = -1;
  record(): void { }
}

// Work on the CPU, run in order with the GPU's (between frames: before the frame's commands)
export class CpuNode implements Node {
  readonly name: string;
  readonly run: (frame: FrameInfo) => void;
  constructor(name: string, run: (frame: FrameInfo) => void) {
    this.name = name;
    this.run = run;
  }
  prepare(frame: FrameInfo): void {
    this.run(frame);
  }
  record(): void { }
}
