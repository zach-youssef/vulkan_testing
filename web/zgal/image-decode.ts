// Decoding image files with stb_image built to WebAssembly (web/decode, built to
// build/web-decode/stb_image.wasm), so the web gets exactly the RGBA8 bytes native stbi_load(..., 4)
// gives: the same PNG, JPEG, BMP, TGA, GIF, PSD, PIC and PNM decoding, in Node and browsers. HDR
// files fail, where native converts them to 8 bits.

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

  // From stb_image.wasm at url: a file: URL is read under Node, others are fetched
  static async load(url: URL | string): Promise<ImageDecoder> {
    const location = new URL(url, globalThis.location?.href);
    if (location.protocol === 'file:') {
      const { readFile } = await import('node:fs/promises');
      return ImageDecoder.create(new Uint8Array(await readFile(location)));
    }
    const response = await fetch(location);
    if (!response.ok) throw new Error(`loading the image decoder (${location.href}): ${response.status} ${response.statusText}`);
    return ImageDecoder.create(await response.arrayBuffer());
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

function cString(memory: ArrayBuffer, at: number): string {
  const bytes = new Uint8Array(memory, at);
  return new TextDecoder().decode(bytes.subarray(0, bytes.indexOf(0)));
}
