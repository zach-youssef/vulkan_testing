// Writing PNG files of 8-bit RGBA pixels under Node (captures and screenshot tests), with Node's
// zlib (png-write.ts lays them out; image-decode.ts reads them)

import { deflateSync } from 'node:zlib';
import { pngFile, pngRows } from './png-write.ts';

// A PNG of width x height RGBA pixels, rows top to bottom
export function encodePng(width: number, height: number, rgba: Uint8Array): Uint8Array {
  return pngFile(width, height, deflateSync(pngRows(width, height, rgba)));
}

