// Loading what graphs load when they're built, and small helpers generated graphs use

import type { Context } from './context.ts';
import type { FrameInfo, FrameSource, GraphBuffer, GraphImage, RenderGraph } from './graph.ts';
import { stride, type Type } from './types.ts';
import { decodeImage, prepareImageDecoder, type DecodedImage } from './image-decode.ts';

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

// Writes a decoded image into a texture's layer, which is its size
export function writeImage(device: GPUDevice, texture: GPUTexture, image: DecodedImage, layer = 0): void {
  device.queue.writeTexture({ texture, origin: [0, 0, layer] }, image.rgba, { bytesPerRow: image.width * 4 }, [image.width, image.height]);
}

// An image file's texels in an RGBA8 texture for sampling, sRGB-encoded (decoded when sampled) or
// linear, as native graphs load theirs: decoded by stb_image, as natively (decodeImage), so any
// file native reads but HDR gives the same texels
export async function loadImageFile(context: Context, graph: RenderGraph, name: string, path: string, color: 'srgb' | 'linear'): Promise<GraphImage> {
  const format: GPUTextureFormat = color === 'srgb' ? 'rgba8unorm-srgb' : 'rgba8unorm';
  // TEXTURE_BINDING | COPY_SRC | COPY_DST
  const usage = 0x04 | 0x01 | 0x02;
  let texture: GPUTexture;
  try {
    // The decoder downloads while the file does
    void prepareImageDecoder();
    const image = await decodeImage(await context.loadAsset(path), context.signal);
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
