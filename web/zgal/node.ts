// Running ZGAL's generated code under Node, with Dawn (the webgpu package) and assets from files
import { open, readFile, type FileHandle } from 'node:fs/promises';
import { isAbsolute, join } from 'node:path';
import { Readable } from 'node:stream';
import { create, globals } from 'webgpu';
import { Context, type ContextOptions } from './context.ts';

// WebGPU's globals (GPUBufferUsage, ...) for code written for browsers
Object.assign(globalThis, globals);

// Kept alive: a collected instance takes its devices with it
let gpu: GPU | null = null;

// A file opened for reading, failing here (not when it's read) if it's a directory
async function openFile(path: string): Promise<FileHandle> {
  const file = await open(path);
  try {
    if ((await file.stat()).isDirectory()) throw new Error(`${path} is a directory`);
  } catch (error) {
    await file.close();
    throw error;
  }
  return file;
}

// An asset's file opened: absolute paths as they are (a path given on a command line), others in the
// first of directories that has them
async function openAsset(path: string, directories: string[]): Promise<FileHandle> {
  if (isAbsolute(path)) return openFile(path);
  for (const directory of directories) {
    try {
      return await openFile(join(directory, path));
    } catch {
      // The next directory
    }
  }
  throw new Error(`not found in ${directories.join(', ') || 'no asset directories'}`);
}

// A context on Dawn, reading assets (whole, and streamed from the file as it's read) from the first
// of assetDirectories that has them (absolute paths as they are)
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
    // The file is closed once read to its end or failed
    assetStreams: async (path) => Readable.toWeb(
      (await openAsset(path, assetDirectories)).createReadStream()) as ReadableStream<Uint8Array>,
    ...options,
  });
}
