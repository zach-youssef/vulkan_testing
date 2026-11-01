// The web's image decoder (web/zgal/image-decode.ts, stb_image built to WebAssembly) gives exactly
// the bytes native stbi_load(..., 4) does (stb_dump, built natively), for the repository's images,
// BMP and TGA files made here, and damaged files, which must fail for the same reason or decode the
// same. No GPU. make test-decode runs it, naming in the environment:
//   ZGAL_DECODE_WASM    build/web-decode/stb_image.wasm
//   ZGAL_DECODE_NATIVE  the native stb_dump
//   ZGAL_DECODE_IMAGES  the image files, separated by spaces
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { after, before, test } from 'node:test';
import { pathToFileURL } from 'node:url';
import { ImageDecoder } from '../../zgal/image-decode.ts';

function required(name: string): string {
  const value = process.env[name];
  if (!value) throw new Error(`${name} isn't set (make test-decode sets it)`);
  return value;
}

const wasm = required('ZGAL_DECODE_WASM');
const native = required('ZGAL_DECODE_NATIVE');
const images = required('ZGAL_DECODE_IMAGES').split(' ').filter((path) => path !== '');

let decoder: ImageDecoder;
let scratch: string;

before(async () => {
  decoder = await ImageDecoder.load(pathToFileURL(wasm));
  scratch = mkdtempSync(join(tmpdir(), 'zgal-decode-'));
});

// before is skipped when TEST_ARGS's name pattern matches no test here, as from make test-web
after(() => {
  if (scratch) rmSync(scratch, { recursive: true, force: true });
});

type Outcome = { width: number; height: number; rgba: Uint8Array } | { error: string };

// What native stb_image makes of a file's bytes
function nativeDecode(name: string, file: Uint8Array): Outcome {
  const input = join(scratch, name);
  const output = `${input}.rgba`;
  writeFileSync(input, file);
  const run = spawnSync(native, [input, output], { encoding: 'utf8' });
  if (run.status === 1) return { error: run.stderr.trim() };
  assert.equal(run.status, 0, `stb_dump ${name} failed: ${run.stderr}`);
  const dump = readFileSync(output);
  const header = new DataView(dump.buffer, dump.byteOffset, 8);
  return { width: header.getUint32(0, true), height: header.getUint32(4, true), rgba: new Uint8Array(dump.subarray(8)) };
}

function webDecode(file: Uint8Array): Outcome {
  try {
    return decoder.decode(file);
  } catch (error) {
    const message = (error as Error).message;
    const reason = /^can't decode the image: (.*)$/.exec(message);
    assert.ok(reason, `unexpected error: ${message}`);
    return { error: reason[1] };
  }
}

// Both decoders' outcomes are the same: the same size and bytes, or the same failure
function assertSame(name: string, file: Uint8Array): Outcome {
  const expected = nativeDecode(name, file);
  const got = webDecode(file);
  if ('error' in expected) {
    assert.deepEqual(got, expected, `${name}: native fails (${expected.error})`);
  } else {
    assert.ok(!('error' in got), `${name}: the web decoder fails (${'error' in got ? got.error : ''}), native decodes it`);
    assert.deepEqual([got.width, got.height], [expected.width, expected.height], `${name}'s size`);
    assert.equal(got.rgba.length, expected.width * expected.height * 4);
    // Compared as buffers, to report the first difference rather than every byte
    assert.ok(Buffer.from(got.rgba).equals(Buffer.from(expected.rgba)), `${name}'s pixels differ from native's`);
  }
  return expected;
}

test('the repository\'s images decode exactly as native', () => {
  assert.ok(images.some((path) => path.endsWith('.jpg')) && images.some((path) => path.endsWith('.png')));
  for (const path of images) {
    const outcome = assertSame(basename(path), new Uint8Array(readFileSync(path)));
    assert.ok(!('error' in outcome), `native can't decode ${path}`);
  }
});

test('damaged files fail or decode as native', () => {
  let failures = 0;
  for (const path of images) {
    const file = new Uint8Array(readFileSync(path));
    const name = basename(path);
    // Cut short: JPEGs decode what they have, PNGs fail
    for (const fraction of [0.5, 0.9]) {
      const outcome = assertSame(`${name}-cut-${fraction}`, file.slice(0, Math.floor(file.length * fraction)));
      if ('error' in outcome) failures++;
    }
    // Bytes changed in the middle: the entropy-coded data of a JPEG, the compressed data of a PNG
    const changed = file.slice();
    for (let at = Math.floor(file.length / 2); at < file.length - 16; at += Math.floor(file.length / 16)) changed[at] ^= 0x5a;
    if ('error' in assertSame(`${name}-changed`, changed)) failures++;
  }
  // Not an image at all, and an empty file
  const noise = new Uint8Array(4096).map((_, i) => (i * 7919 + 13) % 251);
  if ('error' in assertSame('noise', noise)) failures++;
  if ('error' in assertSame('empty', new Uint8Array(0))) failures++;
  assert.ok(failures >= 2, 'some damaged files fail');
});

// An uncompressed 24-bit BMP of width x height, its pixels from colour(x, y)
function bmp(width: number, height: number, colour: (x: number, y: number) => number[]): Uint8Array {
  const stride = Math.ceil((width * 3) / 4) * 4;
  const file = new Uint8Array(54 + stride * height);
  const view = new DataView(file.buffer);
  file.set([0x42, 0x4d]);
  view.setUint32(2, file.length, true);
  view.setUint32(10, 54, true);
  view.setUint32(14, 40, true);
  view.setInt32(18, width, true);
  view.setInt32(22, height, true);
  view.setUint16(26, 1, true);
  view.setUint16(28, 24, true);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const [r, g, b] = colour(x, height - 1 - y); // bottom row first
      file.set([b, g, r], 54 + y * stride + x * 3);
    }
  }
  return file;
}

// An uncompressed 32-bit TGA (with alpha), top row first
function tga(width: number, height: number, colour: (x: number, y: number) => number[]): Uint8Array {
  const file = new Uint8Array(18 + width * height * 4);
  const view = new DataView(file.buffer);
  file[2] = 2;
  view.setUint16(12, width, true);
  view.setUint16(14, height, true);
  file[16] = 32;
  file[17] = 0x28; // 8 alpha bits, top-left origin
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const [r, g, b, a] = colour(x, y);
      file.set([b, g, r, a], 18 + (y * width + x) * 4);
    }
  }
  return file;
}

test('BMP and TGA files decode exactly as native', () => {
  const colour = (x: number, y: number) => [(x * 40) % 256, (y * 70) % 256, (x * y * 13) % 256, (x + y * 5) % 256];
  for (const [name, file] of [['made.bmp', bmp(7, 5, colour)], ['made.tga', tga(6, 3, colour)]] as const) {
    const outcome = assertSame(name, file);
    assert.ok(!('error' in outcome), `native can't decode ${name}`);
  }
});

test('the decoder is reused, growing its memory for large images', async () => {
  const jpeg = new Uint8Array(readFileSync(images.find((path) => path.endsWith('.jpg'))!));
  const first = decoder.decode(jpeg);
  const before = (decoder as unknown as { exports: { memory: WebAssembly.Memory } }).exports.memory.buffer.byteLength;
  // A 2048x2048 BMP is 16 MB decoded, more than the decoder had
  const large = bmp(2048, 2048, (x, y) => [x & 255, y & 255, (x ^ y) & 255]);
  const decoded = decoder.decode(large);
  assert.deepEqual([decoded.width, decoded.height], [2048, 2048]);
  assert.deepEqual([...decoded.rgba.subarray((5 * 2048 + 300) * 4, (5 * 2048 + 301) * 4)], [300 & 255, 5, (300 ^ 5) & 255, 255]);
  const memory = (decoder as unknown as { exports: { memory: WebAssembly.Memory } }).exports.memory.buffer.byteLength;
  assert.ok(memory > before, `the memory grew (${before} to ${memory} bytes)`);
  // Earlier results are copies, and a decode after a failure works
  assert.throws(() => decoder.decode(new Uint8Array(16)), /can't decode the image: unknown image type/);
  assert.deepEqual(decoder.decode(jpeg), first);
  // Decoding from the module's bytes gives the same
  const fresh = await ImageDecoder.create(new Uint8Array(readFileSync(wasm)));
  assert.deepEqual(fresh.decode(jpeg), first);
});

test('loading a decoder that isn\'t there fails', async () => {
  await assert.rejects(ImageDecoder.load(pathToFileURL(join(scratch, 'missing.wasm'))), /ENOENT/);
});
