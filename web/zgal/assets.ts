// Loading what graphs load when they're built, and small helpers generated graphs use

import type { Context } from './context.ts';
import type { FrameInfo, FrameSource, GraphBuffer, GraphImage, RenderGraph } from './graph.ts';
import { stride, type Type } from './types.ts';
import { isPng, pngPixels, readPng, type Png } from './png-pixels.ts';

// Files' bytes, one after another, as elements of type t in a buffer (a memory dump, say): the
// files must hold whole elements
export async function loadBufferFile(context: Context, graph: RenderGraph, name: string, t: Type<any>, paths: string[]): Promise<GraphBuffer> {
  const files = await Promise.all(paths.map((p) => context.loadAsset(p)));
  const s = stride(t);
  for (const [i, f] of files.entries()) {
    if (f.length % s !== 0) {
      throw new Error(`buffer-file ${name}: ${paths[i]} has ${f.length} bytes, which aren't whole ${t.name}s of ${s} bytes`);
    }
  }
  const total = files.reduce((n, f) => n + f.length, 0);
  const bytes = new Uint8Array(Math.max(4, Math.ceil(total / 4) * 4));
  let at = 0;
  for (const f of files) {
    bytes.set(f, at);
    at += f.length;
  }
  const buffer = context.device.createBuffer({ label: name, size: bytes.length, usage: 0x80 | 0x4 | 0x8 | 0x100 | 0x20 });
  context.device.queue.writeBuffer(buffer, 0, bytes);
  return graph.importBuffer(name, t, buffer, true);
}

// An image decoded: its RGBA8 texels, or (decoded by the browser) a bitmap
export type DecodedImage =
  | { readonly width: number; readonly height: number; readonly rgba: Uint8Array; readonly bitmap?: undefined }
  | { readonly width: number; readonly height: number; readonly bitmap: ImageBitmap; readonly rgba?: undefined };

// An image file's bytes decoded. Browsers decode any image they show (PNG, JPEG, WebP, ...), without
// colour management; under Node only PNGs (8-bit, not interlaced) are read.
export async function decodeImage(bytes: Uint8Array): Promise<DecodedImage> {
  // PNGs are read here, so transparent texels keep their colour (browsers' decoders may
  // premultiply); other images (JPEGs, and 16-bit, palette or interlaced PNGs) by the browser
  let png: Png | null = null;
  if (isPng(bytes)) {
    try {
      png = readPng(bytes);
    } catch (error) {
      if (typeof createImageBitmap !== 'function') throw error;
    }
  } else if (typeof createImageBitmap !== 'function') {
    throw new Error('only PNG files are read under Node');
  }
  if (png) {
    const stream = new Blob([png.compressed]).stream().pipeThrough(new DecompressionStream('deflate'));
    const image = pngPixels(png, new Uint8Array(await new Response(stream).arrayBuffer()));
    return { width: image.width, height: image.height, rgba: image.rgba };
  }
  const bitmap = await createImageBitmap(new Blob([bytes as Uint8Array<ArrayBuffer>]), { colorSpaceConversion: 'none', premultiplyAlpha: 'none' });
  return { width: bitmap.width, height: bitmap.height, bitmap };
}

// Writes a decoded image (closing its bitmap) into a texture's layer, which is its size. A bitmap
// needs the texture to have RENDER_ATTACHMENT usage.
export function writeImage(device: GPUDevice, texture: GPUTexture, image: DecodedImage, layer = 0): void {
  if (image.rgba) {
    device.queue.writeTexture({ texture, origin: [0, 0, layer] }, image.rgba as Uint8Array<ArrayBuffer>, { bytesPerRow: image.width * 4 }, [image.width, image.height]);
  } else {
    device.queue.copyExternalImageToTexture({ source: image.bitmap }, { texture, origin: [0, 0, layer], premultipliedAlpha: false }, [image.width, image.height]);
    image.bitmap.close();
  }
}

// An image file's texels in an RGBA8 texture for sampling, sRGB-encoded (decoded when sampled) or
// linear, as native graphs load theirs (see decodeImage for the formats read)
export async function loadImageFile(context: Context, graph: RenderGraph, name: string, path: string, color: 'srgb' | 'linear'): Promise<GraphImage> {
  const format: GPUTextureFormat = color === 'srgb' ? 'rgba8unorm-srgb' : 'rgba8unorm';
  // TEXTURE_BINDING | COPY_SRC | COPY_DST, and RENDER_ATTACHMENT for copying an ImageBitmap in
  const usage = 0x04 | 0x01 | 0x02 | 0x10;
  let texture: GPUTexture;
  try {
    const image = await decodeImage(await context.loadAsset(path));
    texture = context.device.createTexture({ label: name, size: [image.width, image.height], format, usage });
    writeImage(context.device, texture, image);
  } catch (error) {
    throw new Error(`image-file ${name} (${path}): ${error instanceof Error ? error.message : String(error)}`);
  }
  return graph.importImage(name, texture, true);
}

// A per-frame value computed once a frame, however many use it
export function frameCache<T>(source: FrameSource<T>): FrameSource<T> {
  let frameNumber = -1;
  let value: T;
  return (frame: FrameInfo) => {
    if (frame.frameNumber !== frameNumber) {
      value = source(frame);
      frameNumber = frame.frameNumber;
    }
    return value;
  };
}

// x, which a #:when may have left undefined
export function need<T>(x: T | undefined, what: string): T {
  if (x === undefined) throw new Error(`${what} isn't created (its #:when is false)`);
  return x;
}
