// Decoding image files with stb_image built to WebAssembly (web/decode, built to
// build/web-decode/stb_image.wasm), so the web gets exactly the RGBA8 bytes native stbi_load(..., 4)
// gives: the same PNG, JPEG, BMP, TGA, GIF, PSD, PIC and PNM decoding, in Node and browsers. HDR
// files fail, where native converts them to 8 bits. Image files (image-file, models' textures) are
// decoded with decodeImage, which uses the module beside this file (zgal/stb_image.wasm in a built
// program, which zgal-web-program copies there).

export interface DecodedImage {
  width: number;
  height: number;
  // Tightly packed RGBA8 rows, top row first
  rgba: Uint8Array<ArrayBuffer>;
}

interface DecoderExports {
  memory: WebAssembly.Memory;
  zgal_alloc(size: number): number;
  zgal_decode(file: number, size: number, out: number): number;
  zgal_failure_reason(): number;
  zgal_reset(): void;
}

// One instance of the decoder. It decodes one file at a time (decode is synchronous), and its
// memory is reused from one file to the next.
export class ImageDecoder {
  private readonly exports: DecoderExports;

  private constructor(exports: DecoderExports) {
    this.exports = exports;
  }

  // From the module's bytes, or the module compiled
  static async create(module: BufferSource | WebAssembly.Module): Promise<ImageDecoder> {
    const instance =
      module instanceof WebAssembly.Module
        ? await WebAssembly.instantiate(module, {})
        : (await WebAssembly.instantiate(module, {})).instance;
    return new ImageDecoder(instance.exports as unknown as DecoderExports);
  }

  // From stb_image.wasm at url (see compile)
  static async load(url: URL | string): Promise<ImageDecoder> {
    return ImageDecoder.create(await ImageDecoder.compile(url));
  }

  // stb_image.wasm at url compiled, for creating decoders: a file: URL is read under Node, others
  // are fetched
  static async compile(url: URL | string): Promise<WebAssembly.Module> {
    const location = new URL(url, globalThis.location?.href);
    let bytes: BufferSource;
    if (location.protocol === 'file:') {
      const { readFile } = await import('node:fs/promises');
      try {
        bytes = new Uint8Array(await readFile(location));
      } catch (error) {
        throw new Error(`loading the image decoder (${location.href}): ${(error as Error).message}`);
      }
    } else {
      const response = await fetch(location);
      if (!response.ok) throw new Error(`loading the image decoder (${location.href}): ${response.status} ${response.statusText}`);
      bytes = await response.arrayBuffer();
    }
    return WebAssembly.compile(bytes);
  }

  // An image file's pixels as RGBA8. Throws with stb_image's reason when the file can't be decoded.
  decode(file: Uint8Array): DecodedImage {
    const wasm = this.exports;
    try {
      const out = wasm.zgal_alloc(8);
      const input = wasm.zgal_alloc(file.length);
      if (out === 0 || input === 0) throw new Error(`can't decode the image: no memory for its ${file.length} bytes`);
      new Uint8Array(wasm.memory.buffer, input, file.length).set(file);
      const pixels = wasm.zgal_decode(input, file.length, out);
      // Decoding may grow the memory, which replaces its buffer
      const memory = wasm.memory.buffer;
      if (pixels === 0) throw new Error(`can't decode the image: ${cString(memory, wasm.zgal_failure_reason())}`);
      const [width, height] = new Int32Array(memory, out, 2);
      return { width, height, rgba: new Uint8Array(memory.slice(pixels, pixels + width * height * 4)) };
    } finally {
      wasm.zgal_reset();
    }
  }
}

// The runtime's decoder module, stb_image.wasm beside this file, compiled once. A failed load isn't
// kept, so a later decode tries again.
let runtimeModule: Promise<WebAssembly.Module> | undefined;

// Starts loading the runtime's decoder if it isn't loading or loaded, so that a caller about to load
// an image file can fetch both at once rather than one after the other. Its failure is reported by
// the decodes that need it.
export function prepareImageDecoder(): Promise<WebAssembly.Module> {
  if (!runtimeModule) {
    const loading = ImageDecoder.compile(new URL('./stb_image.wasm', import.meta.url));
    runtimeModule = loading;
    loading.catch(() => {
      if (runtimeModule === loading) runtimeModule = undefined;
    });
  }
  return runtimeModule;
}

// An image file's pixels as RGBA8, as native stbi_load(..., 4) gives them, with the runtime's
// decoder. Each decode has its own instance, so its memory (as large as the largest image it
// decoded) is freed with it rather than kept for the page's life. Waiting for the decoder rejects as
// soon as signal (e.g. a Context's) is aborted.
export async function decodeImage(file: Uint8Array, signal?: AbortSignal): Promise<DecodedImage> {
  const module = await untilAborted(prepareImageDecoder(), signal);
  return (await ImageDecoder.create(module)).decode(file);
}

async function untilAborted<T>(promise: Promise<T>, signal: AbortSignal | undefined): Promise<T> {
  if (!signal) return promise;
  signal.throwIfAborted();
  let stop!: () => void;
  const aborted = new Promise<never>((_, reject) => {
    stop = () => reject(signal.reason);
    signal.addEventListener('abort', stop, { once: true });
  });
  try {
    return await Promise.race([promise, aborted]);
  } finally {
    signal.removeEventListener('abort', stop);
  }
}

function cString(memory: ArrayBuffer, at: number): string {
  const bytes = new Uint8Array(memory, at);
  return new TextDecoder().decode(bytes.subarray(0, bytes.indexOf(0)));
}
