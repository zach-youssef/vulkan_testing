// Running ZGAL's generated code under Node, with Dawn (the webgpu package) and assets from files
import { readFile } from 'node:fs/promises';
import { isAbsolute, join } from 'node:path';
import { create, globals } from 'webgpu';
import { Context, type ContextOptions } from './context.ts';

// WebGPU's globals (GPUBufferUsage, ...) for code written for browsers
Object.assign(globalThis, globals);

// Kept alive: a collected instance takes its devices with it
let gpu: GPU | null = null;

// A context on Dawn, reading assets from the first of assetDirectories that has them (absolute paths
// as they are)
export async function createNodeContext(assetDirectories: string[] = [], options: ContextOptions = {}): Promise<Context> {
  gpu ??= create([]);
  return Context.create({
    gpu,
    assets: async (path) => {
      // A path given on a command line
      if (isAbsolute(path)) return new Uint8Array(await readFile(path));
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
