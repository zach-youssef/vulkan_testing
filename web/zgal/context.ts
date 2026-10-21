// The WebGPU device ZGAL's generated code runs on, and where its assets come from.

// Reads an asset (an image file, a buffer file) by its path, relative to the program's assets
export type AssetLoader = (path: string) => Promise<Uint8Array<ArrayBuffer>>;

export interface ContextOptions {
  // The WebGPU entry point: navigator.gpu in browsers; under Node, Dawn's (the webgpu package)
  gpu?: GPU;
  // Assets by path. Browsers fetch them from assets/ beside the page by default.
  assets?: AssetLoader;
  // Features to enable when the adapter has them, besides those ZGAL uses (subgroups,
  // float32-filterable, and texture-formats-tier1 and -tier2 for more storage image formats)
  features?: GPUFeatureName[];
  // Features not to enable even when the adapter has them, e.g. to check what devices without them
  // run (library kernels' variants without subgroups)
  withoutFeatures?: GPUFeatureName[];
}

// Optional features ZGAL enables when the adapter has them
const zgalFeatures: GPUFeatureName[] = ['subgroups', 'float32-filterable', 'texture-formats-tier1', 'texture-formats-tier2'] as GPUFeatureName[];

export class Context {
  readonly device: GPUDevice;
  readonly adapter: GPUAdapter;
  readonly gpu: GPU;
  // WebGPU's validation errors and device loss, which the runtime treats as a validation layer's
  // messages: tests fail on any
  readonly errors: string[] = [];
  // How many frames graphs submit before waiting for the oldest (frames share their resources on
  // WebGPU, so this only bounds how far the CPU runs ahead)
  framesInFlight = 2;
  private readonly assets: AssetLoader;

  setFramesInFlight(count: number): void {
    if (count < 1) throw new Error(`frames in flight must be at least 1, not ${count}`);
    this.framesInFlight = count;
  }

  private constructor(gpu: GPU, adapter: GPUAdapter, device: GPUDevice, assets: AssetLoader) {
    this.gpu = gpu;
    this.adapter = adapter;
    this.device = device;
    this.assets = assets;
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
    const device = await adapter.requestDevice({ requiredFeatures });
    const assets = options.assets ?? (async (path: string) => {
      const response = await fetch(new URL(`assets/${path}`, globalThis.location?.href));
      if (!response.ok) throw new Error(`asset ${path}: ${response.status} ${response.statusText}`);
      return new Uint8Array(await response.arrayBuffer());
    });
    return new Context(gpu, adapter, device, assets);
  }

  async loadAsset(path: string): Promise<Uint8Array<ArrayBuffer>> {
    try {
      return await this.assets(path);
    } catch (e) {
      throw new Error(`can't load asset ${path}: ${(e as Error).message}`);
    }
  }

  hasFeature(feature: string): boolean {
    return this.device.features.has(feature);
  }

  // The format the surface (a canvas) is shown in
  preferredSurfaceFormat(): GPUTextureFormat {
    return this.gpu.getPreferredCanvasFormat();
  }

  destroy(): void {
    this.device.destroy();
  }
}
