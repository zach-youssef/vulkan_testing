// Reading PNGs' pixels without Node or a browser's decoder (png.ts writes them under Node). Browsers'
// decoders may premultiply alpha whatever they're asked (Firefox's createImageBitmap does), losing
// the colour of transparent texels, so image files that are PNGs are read here everywhere.

export interface Png {
  width: number;
  height: number;
  channels: number;
  // The IDAT chunks' zlib stream
  compressed: Uint8Array<ArrayBuffer>;
}

// A PNG's header and compressed pixels: 8-bit grey, grey-alpha, RGB and RGBA images without
// interlacing. Throws for others (16-bit, palette or interlaced images).
export function readPng(file: Uint8Array): Png {
  if (!isPng(file)) throw new Error('not a PNG file');
  const view = new DataView(file.buffer, file.byteOffset, file.byteLength);
  let at = 8;
  let width = 0;
  let height = 0;
  let colorType = 0;
  const data: Uint8Array[] = [];
  while (at < file.length) {
    const length = view.getUint32(at);
    const type = String.fromCharCode(...file.subarray(at + 4, at + 8));
    const body = file.subarray(at + 8, at + 8 + length);
    if (type === 'IHDR') {
      const h = new DataView(body.buffer, body.byteOffset, body.byteLength);
      width = h.getUint32(0);
      height = h.getUint32(4);
      if (body[8] !== 8 || body[12] !== 0) throw new Error('only 8-bit, non-interlaced PNGs are read');
      colorType = body[9];
    } else if (type === 'IDAT') {
      data.push(body);
    }
    at += 12 + length;
  }
  const channels = ({ 0: 1, 2: 3, 4: 2, 6: 4 } as Record<number, number>)[colorType];
  if (!channels) throw new Error(`PNG colour type ${colorType} isn't read`);
  const compressed = new Uint8Array(data.reduce((n, d) => n + d.length, 0));
  let offset = 0;
  for (const d of data) {
    compressed.set(d, offset);
    offset += d.length;
  }
  return { width, height, channels, compressed };
}

export function isPng(file: Uint8Array): boolean {
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  return file.length >= 8 && signature.every((b, i) => file[i] === b);
}

// A PNG's pixels as RGBA, from its inflated IDAT stream
export function pngPixels(png: Png, raw: Uint8Array): { width: number; height: number; rgba: Uint8Array<ArrayBuffer> } {
  const { width, height, channels } = png;
  const stride = width * channels;
  const pixels = new Uint8Array(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? pixels[y * stride + x - channels] : 0;
      const b = y > 0 ? pixels[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? pixels[(y - 1) * stride + x - channels] : 0;
      let v = row[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - b);
        const pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      pixels[y * stride + x] = v & 0xff;
    }
  }
  const rgba = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    const p = pixels.subarray(i * channels, (i + 1) * channels);
    rgba.set(channels === 4 ? p : channels === 3 ? [p[0], p[1], p[2], 255] : channels === 2 ? [p[0], p[0], p[0], p[1]] : [p[0], p[0], p[0], 255], i * 4);
  }
  return { width, height, rgba };
}

