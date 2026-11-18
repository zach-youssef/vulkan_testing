// Loading graphs' image files (zgal.loadImageFile) under Node: their texels arrive exactly as native
// stb_image decodes them, not premultiplied, in an sRGB or linear RGBA8 texture the graph owns.
// tests/assets/texels.png is 4x2: its texels are the expected ones below. texture.jpg (the simple
// example's) arrives as native stb_image decodes it, which make test-web dumps beside it
// (texture.jpg.rgba, from web/decode/tests/stb_dump.cpp). web/tests/image-file.html checks the same
// in a browser.
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';

const expected = [255, 0, 0, 255, 0, 255, 0, 128, 10, 20, 30, 0, 200, 100, 50, 255,
  1, 2, 3, 4, 128, 128, 128, 255, 255, 255, 255, 255, 0, 0, 0, 255];

let context: zgal.Context;

before(async () => {
  context = await createNodeContext([fileURLToPath(new URL('assets', import.meta.url))]);
});

after(() => {
  context.destroy();
});

// An RGBA8 texture's texels, rows top to bottom
async function texels(texture: GPUTexture): Promise<number[]> {
  const device = context.device;
  const row = Math.ceil(texture.width * 4 / 256) * 256;
  const buffer = device.createBuffer({ size: row * texture.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const encoder = device.createCommandEncoder();
  encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow: row }, [texture.width, texture.height]);
  device.queue.submit([encoder.finish()]);
  await buffer.mapAsync(GPUMapMode.READ);
  const bytes = new Uint8Array(buffer.getMappedRange());
  const result: number[] = [];
  for (let y = 0; y < texture.height; y++) result.push(...bytes.subarray(y * row, y * row + texture.width * 4));
  buffer.destroy();
  return result;
}

test('image files load their texels into a texture of their colour space', async () => {
  for (const [color, format] of [['srgb', 'rgba8unorm-srgb'], ['linear', 'rgba8unorm']] as const) {
    const graph = new zgal.RenderGraph(context);
    const image = await zgal.loadImageFile(context, graph, 'picture', 'texels.png', color);
    assert.equal(image.format, format);
    assert.deepEqual(image.size, [4, 2]);
    assert.deepEqual(await texels(image.texture!), expected);
    graph.destroy();
  }
  assert.deepEqual(context.errors, []);
});

test('JPEGs load the texels native stb_image decodes', async () => {
  const graph = new zgal.RenderGraph(context);
  const image = await zgal.loadImageFile(context, graph, 'photo', 'texture.jpg', 'srgb');
  const dump = readFileSync(fileURLToPath(new URL('assets/texture.jpg.rgba', import.meta.url)));
  const size = [dump.readUInt32LE(0), dump.readUInt32LE(4)];
  assert.deepEqual(image.size, size);
  const got = await texels(image.texture!);
  const native = dump.subarray(8);
  assert.equal(got.length, native.length);
  assert.equal(got.findIndex((value, i) => value !== native[i]), -1, 'the texels differ from native stb_image\'s');
  graph.destroy();
  assert.deepEqual(context.errors, []);
});

test('image files name the file and the image when they fail', async () => {
  const graph = new zgal.RenderGraph(context);
  await assert.rejects(zgal.loadImageFile(context, graph, 'picture', 'missing.png', 'srgb'), /image-file picture \(missing\.png\)/);
  // A damaged file fails with stb_image's reason
  const jpeg = join(mkdtempSync(join(tmpdir(), 'zgal-assets-')), 'picture.jpg');
  writeFileSync(jpeg, new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0, 16, 0x4a, 0x46, 0x49, 0x46, 0]));
  await assert.rejects(zgal.loadImageFile(context, graph, 'picture', jpeg, 'srgb'), /^Error: image-file picture \(.*picture\.jpg\): can't decode the image: \S/);
  rmSync(dirname(jpeg), { recursive: true });
  graph.destroy();
});
