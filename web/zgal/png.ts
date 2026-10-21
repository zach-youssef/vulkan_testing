// PNG files of 8-bit RGBA pixels under Node (captures and screenshot tests), with Node's zlib
// (png-pixels.ts reads them anywhere)

import { deflateSync, inflateSync } from 'node:zlib';
import { pngPixels, readPng } from './png-pixels.ts';

const signature = Uint8Array.of(137, 80, 78, 71, 13, 10, 26, 10);

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  view.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

// A PNG of width x height RGBA pixels, rows top to bottom
export function encodePng(width: number, height: number, rgba: Uint8Array): Uint8Array {
  const header = new Uint8Array(13);
  const view = new DataView(header.buffer);
  view.setUint32(0, width);
  view.setUint32(4, height);
  header.set([8, 6, 0, 0, 0], 8); // 8 bits, RGBA, deflate, no filter method, no interlace
  const rows = new Uint8Array(height * (1 + width * 4));
  for (let y = 0; y < height; y++) rows.set(rgba.subarray(y * width * 4, (y + 1) * width * 4), y * (1 + width * 4) + 1);
  const parts = [signature, chunk('IHDR', header), chunk('IDAT', deflateSync(rows)), chunk('IEND', new Uint8Array())];
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

// A PNG's pixels as RGBA (the PNGs readPng reads)
export function decodePng(file: Uint8Array): { width: number; height: number; rgba: Uint8Array<ArrayBuffer> } {
  const png = readPng(file);
  return pngPixels(png, inflateSync(png.compressed));
}
