// The WebGPU device ZGAL's generated code runs on, and where its assets come from.

import { readableStream, type Stream } from './stream.ts';

// Reads an asset (an image file, a buffer file) by its path, relative to the program's assets.
// signal is the context's (Context.signal): a loader that fetches gives it to fetch, so the fetch
// ends when the context's loads do.
export type AssetLoader = (path: string, signal: AbortSignal) => Promise<Uint8Array<ArrayBuffer>>;

// Opens an asset (a stream-buffer's file) as a stream of its bytes, by its path as for AssetLoader
// (and given the context's signal as it is). Rejects if it can't be opened (e.g. it doesn't exist);
// the stream fails if reading it does.
export type AssetStreamLoader = (path: string, signal: AbortSignal) => Promise<ReadableStream<Uint8Array>>;

export interface ContextOptions {
  // The WebGPU entry point: navigator.gpu in browsers; under Node, Dawn's (the webgpu package)
  gpu?: GPU;
  // Assets by path, read whole (assets) and streamed (assetStreams). Without either, browsers fetch
  // both from assets/ beside the page; given one, the other's loads fail rather than quietly
  // fetching from elsewhere.
  assets?: AssetLoader;
  assetStreams?: AssetStreamLoader;
  // Features to enable when the adapter has them, besides those ZGAL uses (subgroups,
  // float32-filterable, texture-formats-tier1 and -tier2 for more storage image formats, and
  // indirect-first-instance for indirect draws' first instances)
  features?: GPUFeatureName[];
  // Features not to enable even when the adapter has them, e.g. to check what devices without them
  // run (library kernels' variants without subgroups)
  withoutFeatures?: GPUFeatureName[];
  // Aborting it ends the context's loads, as destroy() does (see Context.signal): e.g. a page's
  // program stopping while its assets download
  signal?: AbortSignal;
}

// Optional features ZGAL enables when the adapter has them
const zgalFeatures: GPUFeatureName[] = ['subgroups', 'float32-filterable', 'texture-formats-tier1', 'texture-formats-tier2', 'indirect-first-instance'] as GPUFeatureName[];

export class Context {
  readonly device: GPUDevice;
  readonly adapter: GPUAdapter;
  readonly gpu: GPU;
  // WebGPU's validation errors and device loss, which the runtime treats as a validation layer's
  // messages: tests fail on any
  readonly errors: string[] = [];
  // How many frames graphs submit before waiting for the oldest, and how many copies resources
  // with frames 'per-frame' have (see RenderGraph.createImage)
  get framesInFlight(): number {
    return this.inFlight;
  }
  // The frame slot graphs record into, in [0, framesInFlight): which copy of a per-frame resource
  // a frame uses, as natively VulkanContext::currentFrame
  get frameIndex(): number {
    return this.slot;
  }
  private inFlight = 2;
  private slot = 0;
  private pendingFramesInFlight: number | null = null;
  private readonly assets: AssetLoader;
  private readonly assetStreams: AssetStreamLoader;
  private readonly loads = new AbortController();

  // Requests a new number of frames in flight, applied at the next frame boundary (advanceFrame),
  // as natively: framesInFlight keeps the current count until then
  setFramesInFlight(count: number): void {
    if (!Number.isInteger(count) || count < 1) throw new Error(`frames in flight must be at least 1, not ${count}`);
    this.pendingFramesInFlight = count;
  }

  // Ends a frame (a graph's execute() does, once it has submitted): moves on to the next slot, or
  // applies a requested number of frames in flight, restarting at slot 0, as natively
  // VulkanContext::advanceFrame. Graphs then give their per-frame resources as many copies, keeping
  // the first ones (and their contents) and adding zeroed ones (PerFrame's resizing).
  advanceFrame(): void {
    const pending = this.pendingFramesInFlight;
    this.pendingFramesInFlight = null;
    if (pending !== null && pending !== this.inFlight) {
      this.inFlight = pending;
      this.slot = 0;
    } else {
      this.slot = (this.slot + 1) % this.inFlight;
    }
  }

  private constructor(gpu: GPU, adapter: GPUAdapter, device: GPUDevice, assets: AssetLoader, assetStreams: AssetStreamLoader, signal: AbortSignal | undefined) {
    this.gpu = gpu;
    this.adapter = adapter;
    this.device = device;
    this.assets = assets;
    this.assetStreams = assetStreams;
    if (signal?.aborted) this.loads.abort(signal.reason);
    else signal?.addEventListener('abort', () => this.loads.abort(signal.reason), { once: true });
    device.addEventListener('uncapturederror', (event) => {
      this.errors.push((event as GPUUncapturedErrorEvent).error.message);
    });
    device.lost.then((info) => {
      if (info.reason !== 'destroyed') this.errors.push(`device lost: ${info.message}`);
    });
  }

  static async create(options: ContextOptions = {}): Promise<Context> {
    const gpu = options.gpu ?? (globalThis.navigator as Navigator | undefined)?.gpu;
    if (!gpu) throw new Error('WebGPU isn\'t available here');
    const adapter = await gpu.requestAdapter();
    if (!adapter) throw new Error('No WebGPU adapter');
    const wanted = [...zgalFeatures, ...(options.features ?? [])];
    const requiredFeatures = wanted.filter((f) => adapter.features.has(f) && !options.withoutFeatures?.includes(f));
    // Arrays of textures bind a texture array per distinct size and format of their elements,
    // arrays of storage images a storage texture per distinct image (graph.ts's TextureArray) and
    // arrays of buffers a storage buffer per distinct buffer (BufferArray), so as many sampled and
    // storage textures and storage buffers as the adapter allows, and each array's table is a uniform
    // buffer, so as many of those too
    const requiredLimits = {
      maxSampledTexturesPerShaderStage: adapter.limits.maxSampledTexturesPerShaderStage,
      maxStorageTexturesPerShaderStage: adapter.limits.maxStorageTexturesPerShaderStage,
      maxStorageBuffersPerShaderStage: adapter.limits.maxStorageBuffersPerShaderStage,
      maxUniformBuffersPerShaderStage: adapter.limits.maxUniformBuffersPerShaderStage,
    };
    const device = await adapter.requestDevice({ requiredFeatures, requiredLimits });
    const fetchAsset = async (path: string, signal: AbortSignal) => {
      const response = await fetch(new URL(`assets/${path}`, globalThis.location?.href), { signal });
      if (!response.ok) throw new Error(`asset ${path}: ${response.status} ${response.statusText}`);
      return response;
    };
    const fetching = options.assets === undefined && options.assetStreams === undefined;
    const assets = options.assets ?? (fetching
      ? async (path: string, signal: AbortSignal) => new Uint8Array(await (await fetchAsset(path, signal)).arrayBuffer())
      : async () => { throw new Error('this context reads no whole assets (ContextOptions.assets)'); });
    const assetStreams = options.assetStreams ?? (fetching
      ? async (path: string, signal: AbortSignal) => (await fetchAsset(path, signal)).body ?? new Blob([]).stream()
      : async () => { throw new Error('this context streams no assets (ContextOptions.assetStreams)'); });
    return new Context(gpu, adapter, device, assets, assetStreams, options.signal);
  }

  // Aborted once the context's loads end: when ContextOptions.signal is aborted, or the context is
  // destroyed. Loads then end at once, whatever their loaders do: one waiting for an asset rejects
  // (and later ones do at once), and the streams loadAssetStream opened that are still downloading
  // are cancelled, with their sources (a finished one has nothing to cancel). Loaders get it (the default ones and a page's give it to fetch), and so can a program's
  // own StreamOpener that fetches.
  get signal(): AbortSignal {
    return this.loads.signal;
  }

  // load(signal), rejecting as soon as the context's loads end
  private async untilAborted<T>(load: (signal: AbortSignal) => Promise<T>): Promise<T> {
    const signal = this.loads.signal;
    signal.throwIfAborted();
    let stop!: () => void;
    const aborted = new Promise<never>((_, reject) => {
      stop = () => reject(signal.reason);
      signal.addEventListener('abort', stop, { once: true });
    });
    try {
      return await Promise.race([load(signal), aborted]);
    } finally {
      signal.removeEventListener('abort', stop);
    }
  }

  async loadAsset(path: string): Promise<Uint8Array<ArrayBuffer>> {
    try {
      return await this.untilAborted((signal) => this.assets(path, signal));
    } catch (e) {
      throw new Error(`can't load asset ${path}: ${(e as Error).message}`);
    }
  }

  // An asset as a stream, for a stream-buffer: the streaming sibling of loadAsset. Rejects if the
  // asset can't be opened (so an opener using it fails the graph's build); the stream fails if
  // reading the asset does.
  async loadAssetStream(path: string): Promise<Stream> {
    const opening: { source?: Promise<ReadableStream<Uint8Array>> } = {};
    let source: ReadableStream<Uint8Array>;
    try {
      source = await this.untilAborted((signal) => (opening.source = this.assetStreams(path, signal)));
    } catch (e) {
      // A source opened after the loads ended is cancelled (a fetch's body has failed already)
      void opening.source?.then((late) => late.cancel(), () => {}).catch(() => {});
      throw new Error(`can't open asset stream ${path}: ${(e as Error).message}`);
    }
    // Cancelled (with its source) when the loads end, even if they already have
    return readableStream(source, `asset stream ${path}`, this.loads.signal);
  }

  hasFeature(feature: string): boolean {
    return this.device.features.has(feature);
  }

  // The format the surface (a canvas) is shown in
  preferredSurfaceFormat(): GPUTextureFormat {
    return this.gpu.getPreferredCanvasFormat();
  }

  // Ends its loads (see signal) and destroys its device
  destroy(): void {
    this.loads.abort();
    this.device.destroy();
  }
}
