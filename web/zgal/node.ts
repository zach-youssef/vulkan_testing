// Running ZGAL's generated code under Node, with Dawn (the webgpu package) and assets from files
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { create, globals } from 'webgpu';
import { Context, type ContextOptions } from './context.ts';

// WebGPU's globals (GPUBufferUsage, ...) for code written for browsers
Object.assign(globalThis, globals);

// Kept alive: a collected instance takes its devices with it
let gpu: GPU | null = null;

// A context on Dawn, reading assets from the first of assetDirectories that has them
export async function createNodeContext(assetDirectories: string[] = [], options: ContextOptions = {}): Promise<Context> {
  gpu ??= create([]);
  return Context.create({
    gpu,
    assets: async (path) => {
      for (const directory of assetDirectories) {
        try {
          return new Uint8Array(await readFile(join(directory, path)));
        } catch {
          // The next directory
        }
      }
      throw new Error(`not found in ${assetDirectories.join(', ') || 'no asset directories'}`);
    },
    ...options,
  });
}
