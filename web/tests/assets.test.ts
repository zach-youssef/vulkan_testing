// Loading graphs' image files (zgal.loadImageFile) under Node: PNGs' texels arrive exactly, not
// premultiplied, in an sRGB or linear RGBA8 texture the graph owns. tests/assets/texels.png is 4x2:
// its texels are the expected ones below (web/tests/image-file.html checks the same in a browser).
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
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

// A 4x2 RGBA8 texture's texels
async function texels(texture: GPUTexture): Promise<number[]> {
  const device = context.device;
  const buffer = device.createBuffer({ size: 256 * 2, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const encoder = device.createCommandEncoder();
  encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow: 256 }, [4, 2]);
  device.queue.submit([encoder.finish()]);
  await buffer.mapAsync(GPUMapMode.READ);
  const bytes = new Uint8Array(buffer.getMappedRange());
  const result = [...bytes.subarray(0, 16), ...bytes.subarray(256, 272)];
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

test('image files name the file and the image when they fail', async () => {
  const graph = new zgal.RenderGraph(context);
  await assert.rejects(zgal.loadImageFile(context, graph, 'picture', 'missing.png', 'srgb'), /image-file picture \(missing\.png\)/);
  // Under Node only PNGs are read
  const jpeg = join(mkdtempSync(join(tmpdir(), 'zgal-assets-')), 'picture.jpg');
  writeFileSync(jpeg, new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0, 16, 0x4a, 0x46, 0x49, 0x46, 0]));
  await assert.rejects(zgal.loadImageFile(context, graph, 'picture', jpeg, 'srgb'), /image-file picture .*only PNG files/);
  rmSync(dirname(jpeg), { recursive: true });
  graph.destroy();
});
