// PNG files of 8-bit RGBA pixels under Node (captures and screenshot tests), with Node's zlib
// (png-pixels.ts reads them anywhere, png-write.ts lays them out)

import { deflateSync, inflateSync } from 'node:zlib';
import { pngPixels, readPng } from './png-pixels.ts';
import { pngFile, pngRows } from './png-write.ts';

// A PNG of width x height RGBA pixels, rows top to bottom
export function encodePng(width: number, height: number, rgba: Uint8Array): Uint8Array {
  return pngFile(width, height, deflateSync(pngRows(width, height, rgba)));
}

// A PNG's pixels as RGBA (the PNGs readPng reads)
export function decodePng(file: Uint8Array): { width: number; height: number; rgba: Uint8Array<ArrayBuffer> } {
  const png = readPng(file);
  return pngPixels(png, inflateSync(png.compressed));
}
