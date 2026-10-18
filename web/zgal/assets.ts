// Loading what graphs load when they're built, and small helpers generated graphs use

import type { Context } from './context.ts';
import type { FrameInfo, FrameSource, GraphBuffer, RenderGraph } from './graph.ts';
import { stride, type Type } from './types.ts';

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
