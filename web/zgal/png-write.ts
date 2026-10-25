// Writing PNG files of 8-bit RGBA pixels anywhere: png.ts compresses them with Node's zlib (captures
// under Node, screenshot tests), encodePngStream with the CompressionStream browsers and Node both
// have (captures in pages)

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

// The rows a PNG of width x height RGBA pixels (rows top to bottom) compresses: each a filter byte
// (none) and its pixels
export function pngRows(width: number, height: number, rgba: Uint8Array): Uint8Array<ArrayBuffer> {
  if (rgba.length !== width * height * 4) throw new Error(`a ${width}x${height} PNG needs ${width * height * 4} bytes of RGBA, not ${rgba.length}`);
  const rows = new Uint8Array(height * (1 + width * 4));
  for (let y = 0; y < height; y++) rows.set(rgba.subarray(y * width * 4, (y + 1) * width * 4), y * (1 + width * 4) + 1);
  return rows;
}

// The PNG file of pngRows's rows compressed as a zlib stream
export function pngFile(width: number, height: number, compressed: Uint8Array): Uint8Array<ArrayBuffer> {
  const header = new Uint8Array(13);
  const view = new DataView(header.buffer);
  view.setUint32(0, width);
  view.setUint32(4, height);
  header.set([8, 6, 0, 0, 0], 8); // 8 bits, RGBA, deflate, no filter method, no interlace
  const parts = [signature, chunk('IHDR', header), chunk('IDAT', compressed), chunk('IEND', new Uint8Array())];
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

// A PNG of width x height RGBA pixels, rows top to bottom, compressed with CompressionStream
export async function encodePngStream(width: number, height: number, rgba: Uint8Array): Promise<Uint8Array<ArrayBuffer>> {
  const stream = new Blob([pngRows(width, height, rgba)]).stream().pipeThrough(new CompressionStream('deflate'));
  return pngFile(width, height, new Uint8Array(await new Response(stream).arrayBuffer()));
}
