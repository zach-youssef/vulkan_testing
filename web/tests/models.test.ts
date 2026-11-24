// Models, meshes and texture arrays on the web: what tests/gpu/ZgalTest.cpp checks natively, with
// models' base colours in a texture array (a layer per material), meshes the test makes, and image
// files of different sizes copied into one texture array. tests/gpu/assets' models are copied
// beside web/tests/assets' images (make test-web).
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Corner, Cull, CulledModels, ImportedLayers, ImportedStores, LayerGraph, Layers, Meshes, Models, ModelVertex, Scale, Spread, Spreads, Stamps } from '../models.zgal.ts';

const assets = fileURLToPath(new URL('assets', import.meta.url));
let context: zgal.Context;

before(async () => {
  context = await createNodeContext([assets]);
});

after(() => {
  context.destroy();
});

const pixel = (pixels: Uint8Array, x: number, y: number) => [...pixels.subarray((y * 4 + x) * 4, (y * 4 + x) * 4 + 4)];
const black = [0, 0, 0, 255];
const green = [0, 128, 0, 255];

// As tests/cpu/ModelTest.cpp checks natively
test('glTF models bake their nodes\' transforms and keep their base colour materials', async () => {
  const model = await zgal.loadModelData(context, 'halves.gltf');
  // The file's two materials, then the default one
  assert.deepEqual(model.materials.map((m) => [m.name, m.baseColorFactor, m.baseColorTexture?.path ?? null]),
    [['quadrants', [1, 1, 1, 1], 'quadrants.png'], ['green', [0, 0.5, 0, 1], null], ['default', [1, 1, 1, 1], null]]);
  assert.equal(model.primitives.length, 2);
  const [left, right] = model.primitives;
  assert.equal(left.material, 0);
  assert.deepEqual(left.indices, [0, 1, 2, 2, 3, 0]);
  // The unit quad, scaled by (1, 2, 1) and moved by (-1, -1, 0)
  assert.deepEqual(left.positions.slice(6, 9), [0, 1, 0]);
  assert.deepEqual(left.uvs.slice(4, 6), [1, 1]);
  assert.deepEqual(left.normals.slice(0, 3), [0, 0, 1]);
  assert.deepEqual(left.colors, []);
  assert.equal(right.material, 1);
  assert.deepEqual(right.positions.slice(0, 3), [0, -1, 0]);
});

test('binary glTF models hold their buffers and images', async () => {
  // halves.gltf as a .glb: its buffer in the binary chunk, followed by quadrants.png's bytes
  const gltf = JSON.parse(readFileSync(join(assets, 'halves.gltf'), 'utf8'));
  const data = Buffer.from(gltf.buffers[0].uri.split(',')[1], 'base64');
  const png = readFileSync(join(assets, 'quadrants.png'));
  const pad = (n: number) => (n + 3) & ~3;
  const binary = Buffer.alloc(pad(data.length) + pad(png.length));
  data.copy(binary);
  png.copy(binary, pad(data.length));
  gltf.buffers = [{ byteLength: binary.length }];
  gltf.bufferViews.push({ buffer: 0, byteOffset: pad(data.length), byteLength: png.length });
  gltf.images[0] = { bufferView: gltf.bufferViews.length - 1, mimeType: 'image/png' };
  const json = Buffer.from(JSON.stringify(gltf).padEnd(pad(JSON.stringify(gltf).length)));
  const chunk = (type: number, bytes: Buffer) => {
    const header = Buffer.alloc(8);
    header.writeUInt32LE(bytes.length, 0);
    header.writeUInt32LE(type, 4);
    return Buffer.concat([header, bytes]);
  };
  const chunks = Buffer.concat([chunk(0x4e4f534a, json), chunk(0x004e4942, binary)]);
  const header = Buffer.alloc(12);
  header.writeUInt32LE(0x46546c67, 0);
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(12 + chunks.length, 8);
  const directory = mkdtempSync(join(tmpdir(), 'zgal-models-'));
  try {
    const path = join(directory, 'halves.glb');
    writeFileSync(path, Buffer.concat([header, chunks]));
    const model = await zgal.loadModelData(context, path);
    assert.deepEqual(model.primitives.map((p) => p.positions), (await zgal.loadModelData(context, 'halves.gltf')).primitives.map((p) => p.positions));
    assert.deepEqual([...model.materials[0].baseColorTexture!.encoded!], [...png]);
    // Drawn as the .gltf is
    const graph = new zgal.RenderGraph(context);
    const ids = await Models.build(context, graph, { file: path });
    await graph.execute();
    const pixels = await ids.canvasReadPixels(graph);
    assert.deepEqual([pixel(pixels, 0, 0), pixel(pixels, 0, 3), pixel(pixels, 3, 0)], [[255, 0, 0, 255], [0, 0, 255, 255], green]);
    graph.destroy();
  } finally {
    rmSync(directory, { recursive: true });
  }
  assert.deepEqual(context.errors, []);
});

test('OBJ models split faces by material, flip texture coordinates and read their .mtl', async () => {
  const model = await zgal.loadModelData(context, 'quad.obj');
  assert.deepEqual(model.materials.map((m) => [m.name, m.baseColorFactor, m.baseColorTexture?.path ?? null]),
    [['tinted', [0.5, 0.25, 1, 1], 'quadrants.png'], ['default', [1, 1, 1, 1], null]]);
  assert.equal(model.primitives.length, 2);
  // The triangle before any usemtl has the default material
  const withMaterial = (material: number) => model.primitives.find((p) => p.material === material)!;
  assert.equal(withMaterial(1).indices.length, 3);
  const quad = withMaterial(0);
  assert.equal(quad.indices.length, 6);
  assert.equal(quad.positions.length, 4 * 3);   // Shared corners are one vertex
  assert.deepEqual(quad.uvs.slice(0, 2), [0, 1]);
  assert.equal(quad.normals.length, 4 * 3);
});

test('models that can\'t be loaded say why', async () => {
  await assert.rejects(zgal.loadModelData(context, 'missing.gltf'), /missing.gltf: not found/);
  await assert.rejects(zgal.loadModelData(context, 'quadrants.png'), /isn't a .gltf, .glb or .obj file/);
  await assert.rejects(zgal.loadModelData(context, 'broken.gltf'), /Failed to parse glTF file/);
});

test('graphs draw models with their materials\' base colours', async () => {
  const graph = new zgal.RenderGraph(context);
  const ids = await Models.build(context, graph, { file: 'halves.gltf' });
  // Two materials and the default one
  assert.equal(ids.images.halfColors.length, 3);
  await graph.execute();
  const pixels = await ids.canvasReadPixels(graph);
  // The left half samples quadrants.png (sRGB, decoded), the right half is the green factor
  assert.deepEqual(pixel(pixels, 0, 0), [255, 0, 0, 255]);
  assert.deepEqual(pixel(pixels, 1, 1), [0, 255, 0, 255]);
  assert.deepEqual(pixel(pixels, 0, 3), [0, 0, 255, 255]);
  assert.deepEqual(pixel(pixels, 1, 2), [55, 55, 55, 255]);
  for (let y = 0; y < 4; y++) {
    assert.deepEqual(pixel(pixels, 2, y), green);
    assert.deepEqual(pixel(pixels, 3, y), green);
  }
  graph.destroy();

  // OBJ files too: the quad's material, then the default one for the triangle without any
  const obj = new zgal.RenderGraph(context);
  const objIds = await Models.build(context, obj, { file: 'quad.obj' });
  assert.equal(objIds.images.halfColors.length, 2);
  await obj.execute();
  obj.destroy();

  // A model that can't be found
  const missing = new zgal.RenderGraph(context);
  await assert.rejects(Models.build(context, missing, { file: 'missing.gltf' }),
    /^Error: loading Models's model halves: can't load asset missing.gltf: not found/);
  missing.destroy();
  assert.deepEqual(context.errors, []);
});

test('models fill vertex formats\' attributes by name, or with their defaults', async () => {
  const bare = (normals: number[]): zgal.ModelData => ({
    primitives: [{ positions: [0, 0, 0], normals, uvs: [0.5, 0.5], colors: [], tangents: [], indices: [0, 0, 0], material: 0 }],
    materials: [{ name: 'default', baseColorFactor: [1, 1, 1, 1], baseColorTexture: null }],
  });
  await assert.rejects(zgal.uploadModel(context, 'bare', bare([]), ModelVertex),
    { message: 'the model has no normals, which ModelVertex\'s normal reads (or give it a #:default)' });
  const model = await zgal.uploadModel(context, 'bare', bare([0, 0, 1]), ModelVertex);
  assert.deepEqual(model.draws, [{ indexCount: 3, instanceCount: 1, firstIndex: 0, vertexOffset: 0, firstInstance: 0 }]);
  assert.deepEqual(model.bounds, [[0, 0, 0, 0]]);
  zgal.destroyMesh(model.mesh);
  for (const t of new Set(model.baseColors)) t.destroy();

  // Packed as natively: a half-float uv (0.5 is 0x3800), snorm16 normal, unorm8 colour (white)
  const bytes = zgal.encodeVertices(ModelVertex, [{ position: [1, 2, 3], uv: [0.5, 0.5], normal: [0, 0, 1] }]);
  assert.equal(bytes.length, 28);
  const view = new DataView(bytes.buffer);
  assert.deepEqual([view.getFloat32(0, true), view.getFloat32(4, true), view.getFloat32(8, true)], [1, 2, 3]);
  assert.deepEqual([view.getUint16(12, true), view.getUint16(14, true)], [0x3800, 0x3800]);
  assert.deepEqual([view.getInt16(16, true), view.getInt16(18, true), view.getInt16(20, true)], [0, 0, 32767]);
  assert.deepEqual([...bytes.subarray(24, 28)], [255, 255, 255, 255]);
  assert.throws(() => zgal.encodeVertices(ModelVertex, [{ position: [0, 0, 0] }]),
    { message: 'vertex 0 has no uv (a ModelVertex attribute without a default)' });
});

test('graphs draw the meshes of models a shader chose', async () => {
  for (const side of [1, -1]) {
    const graph = new zgal.RenderGraph(context);
    const ids = await CulledModels.build(context, graph, { file: 'halves.gltf', side });
    for (let frame = 0; frame < 3; frame++) await graph.execute();
    const pixels = await ids.canvasReadPixels(graph);
    // Only the half on the chosen side: the right half's green factor, or the left's texture
    assert.deepEqual(pixel(pixels, 3, 1), side > 0 ? green : black, `side ${side}`);
    assert.deepEqual(pixel(pixels, 0, 0), side > 0 ? black : [255, 0, 0, 255], `side ${side}`);
    graph.destroy();
  }
  assert.deepEqual(context.errors, []);
});

test('graphs draw meshes they were given, each with its instance index', async () => {
  // The left and right halves, as two triangles each
  const half = (left: number, instanceIndex: number) => zgal.createMesh(context.device,
    zgal.encodeVertices(Corner, [[left, -1], [left + 1, -1], [left + 1, 1], [left, 1]].map((position) => ({ position }))),
    [0, 1, 2, 0, 2, 3], instanceIndex);
  const shapes = [half(-1, 0), half(0, 1)];
  const graph = new zgal.RenderGraph(context);
  const ids = await Meshes.build(context, graph, { shapes });
  await graph.execute();
  const pixels = await ids.canvasReadPixels(graph);
  for (let y = 0; y < 4; y++) {
    for (let x = 0; x < 4; x++) assert.deepEqual(pixel(pixels, x, y), x < 2 ? [255, 0, 0, 255] : [0, 255, 0, 255], `(${x}, ${y})`);
  }
  graph.destroy();
  shapes.forEach(zgal.destroyMesh);
  assert.deepEqual(context.errors, []);
});

test('arrays of image files are texture arrays, an image per layer, one per distinct size and format', async () => {
  const graph = new zgal.RenderGraph(context);
  const ids = await LayerGraph.build(context, graph, {});
  await graph.execute();
  const pixels = await ids.canvasReadPixels(graph);
  // The top rows are texels.png's (4x2); the bottom rows quadrants.png's (2x2), each texel twice
  const texels = [[[255, 0, 0, 255], [0, 255, 0, 128], [10, 20, 30, 0], [200, 100, 50, 255]],
    [[1, 2, 3, 4], [128, 128, 128, 255], [255, 255, 255, 255], [0, 0, 0, 255]]];
  const quadrants = [[[255, 0, 0, 255], [0, 255, 0, 255]], [[0, 0, 255, 255], [128, 128, 128, 255]]];
  for (let y = 0; y < 4; y++) {
    for (let x = 0; x < 4; x++) assert.deepEqual(pixel(pixels, x, y), y < 2 ? texels[y][x] : quadrants[y - 2][x >> 1], `(${x}, ${y})`);
  }
  graph.destroy();
  assert.deepEqual(context.errors, []);
});

test('texture arrays put each distinct image once in the bucket of its size and format', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  // TEXTURE_BINDING | COPY_SRC
  const texture = (width: number, height: number, format: GPUTextureFormat) =>
    device.createTexture({ size: [width, height], format, usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
  const a = texture(4, 2, 'rgba8unorm');
  const b = texture(2, 2, 'rgba8unorm-srgb');
  const c = texture(4, 2, 'rgba8unorm');
  const d = texture(4, 2, 'rgba8unorm-srgb');
  const images = graph.importImages('mixed', [a, b, a, c, d], 'fixed');
  assert.equal(new Set(images).size, 4);
  const array = graph.textureArray('mixed', images);
  assert.equal(graph.textureArray('again', images), array);
  const built = graph.arrayOf(array);
  assert.deepEqual(built.shape, { count: 5, buckets: ['rgba8unorm', 'rgba8unorm-srgb', 'rgba8unorm-srgb'] });
  assert.deepEqual(built.sizes, [[4, 2], [2, 2], [4, 2]]);
  assert.deepEqual(built.buckets.map((t) => t.depthOrArrayLayers), [2, 1, 1]);
  assert.equal(graph.arrayOf(array), built);

  // Past the sampled textures a stage may use, the error names the array's sizes
  const many = Array.from({ length: device.limits.maxSampledTexturesPerShaderStage + 1 }, (_, i) => texture(i + 1, 1, 'rgba8unorm'));
  const pick = new zgal.Sampler(device, 'pick', {}, null);
  const material = new zgal.MaterialInstance(graph, Layers, { layers: { array: graph.textureArray('many', graph.importImages('many', many, 'fixed')) }, pick: { sampler: pick } }, null, null);
  assert.throws(() => material.groupLayout(),
    new RegExp(`^Error: material Layers's fragment stage samples ${many.length} textures, more than this WebGPU device allows \\(maxSampledTexturesPerShaderStage ${many.length - 1}\\): .*it binds layers \\(${many.length} texture arrays, for its images' distinct sizes and formats: 1x1 rgba8unorm, 2x1 rgba8unorm, `));
  graph.destroy();
  for (const t of [a, b, c, d, ...many]) t.destroy();
  assert.deepEqual(context.errors, []);
});

test('texture arrays view graph images and imported ones as buckets of their own, rebuilt when one is recreated', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const texture = (width: number, height: number) =>
    device.createTexture({ size: [width, height], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST });
  const fixed = graph.importImages('fixed', [texture(4, 2), texture(4, 2)], 'fixed');
  const shown = graph.createImage('shown', 'shared', 'rgba8unorm', { surface: 1 });
  const imported = graph.importImage('imported', texture(4, 2), 'live');
  graph.setSurfaceExtent([3, 2]);
  const array = graph.textureArray('mixed', [fixed[0], shown, fixed[1], imported, shown]);
  const built = graph.arrayOf(array);
  // The fixed images are layers of a copy; the others are their own textures, a bucket each
  assert.deepEqual(built.shape, { count: 5, buckets: ['rgba8unorm', 'rgba8unorm', 'rgba8unorm'] });
  assert.deepEqual(built.copied, [true, false, false]);
  assert.equal(built.buckets[0].depthOrArrayLayers, 2);
  assert.equal(built.buckets[1], graph.textureOf(shown));
  assert.equal(built.buckets[2], graph.textureOf(imported));
  assert.deepEqual(built.sizes, [[4, 2], [3, 2], [4, 2]]);
  const generation = array.generation;
  // The surface resizing recreates shown: the views and table are rebuilt, keeping the copy
  graph.setSurfaceExtent([5, 3]);
  graph.imageExtent(shown);
  const rebuilt = graph.arrayOf(array);
  assert.notEqual(rebuilt, built);
  assert.equal(array.generation, generation + 1);
  assert.equal(rebuilt.buckets[0], built.buckets[0]);
  assert.equal(rebuilt.table, built.table);
  assert.equal(rebuilt.buckets[1], graph.textureOf(shown));
  assert.deepEqual(rebuilt.sizes, [[4, 2], [5, 3], [4, 2]]);
  assert.equal(graph.arrayOf(array), rebuilt);
  graph.destroy();
  for (const t of [...fixed, imported]) t.texture!.destroy();
  assert.deepEqual(context.errors, []);
});

test('texture arrays view each frame slot\'s copy of per-frame images, following the frames in flight', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const fixed = graph.importImages('fixed', [device.createTexture({ size: [4, 2], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST })], 'fixed');
  const rotating = graph.createImage('rotating', 'per-frame', 'rgba8unorm', { fixed: [2, 2] });
  const shown = graph.createImage('shown', 'shared', 'rgba8unorm', { fixed: [3, 2] });
  // rotating twice: a bucket at its first appearance, which the second shares
  const array = graph.textureArray('rotated', [fixed[0], rotating, shown, rotating]);
  const check = (built: zgal.BuiltArray) => {
    assert.equal(rotating.textures.length, context.framesInFlight);
    assert.equal(new Set(rotating.textures).size, context.framesInFlight);
    assert.deepEqual(built.shape, { count: 4, buckets: ['rgba8unorm', 'rgba8unorm', 'rgba8unorm'] });
    assert.deepEqual(built.copied, [true, false, false]);
    assert.equal(built.buckets[1], rotating.textures[0]);
    // A slot per frame in flight, each viewing its copy of rotating, and the shared image and copy
    assert.equal(built.views.length, context.framesInFlight);
    built.views.forEach((views, slot) => {
      assert.equal(views.length, 3);
      assert.equal(views[0].label, built.buckets[0].label);
      assert.equal(views[1].label, `rotating (frame ${slot})`);
      assert.equal(views[2].label, 'shown');
    });
  };
  const built = graph.arrayOf(array);
  check(built);
  // A new count, applied at the frame boundary: the frame's start adds rotating's copy, and the
  // array is rebuilt with a slot more, keeping its copy of the fixed image and its table
  const before = context.framesInFlight;
  context.setFramesInFlight(before + 1);
  context.advanceFrame();
  graph.imageExtent(rotating);
  const rebuilt = graph.arrayOf(array);
  assert.notEqual(rebuilt, built);
  assert.equal(context.framesInFlight, before + 1);
  check(rebuilt);
  assert.equal(rebuilt.buckets[0], built.buckets[0]);
  assert.equal(rebuilt.table, built.table);
  context.setFramesInFlight(before);
  context.advanceFrame();
  graph.destroy();
  fixed[0].texture!.destroy();
  assert.deepEqual(context.errors, []);
});

test('arrays of imported images sample what the application last wrote to them', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const texture = (width: number, height: number) =>
    device.createTexture({ size: [width, height], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  const write = (t: GPUTexture, texels: number[][]) =>
    device.queue.writeTexture({ texture: t }, new Uint8Array(texels.flat()), { bytesPerRow: t.width * 4 }, [t.width, t.height]);
  const wide = texture(4, 2);
  const small = texture(2, 2);
  const wideTexels = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [10, 20, 30, 255], [40, 50, 60, 255], [70, 80, 90, 255], [100, 110, 120, 255], [130, 140, 150, 255]];
  write(wide, wideTexels);
  write(small, [[1, 2, 3, 255], [4, 5, 6, 255], [7, 8, 9, 255], [11, 12, 13, 255]]);
  const ids = await ImportedLayers.build(context, graph, { given: [wide, small] });
  assert.deepEqual(ids.images.given.map((i) => i.texture), [wide, small]);
  const rows = async () => {
    await graph.execute();
    const pixels = await ids.canvasReadPixels(graph);
    return [0, 1, 2, 3].map((y) => [0, 1, 2, 3].map((x) => pixel(pixels, x, y)));
  };
  // The top rows are wide's texels, the bottom ones small's, each twice
  const expected = (smallTexels: number[][]) => [
    wideTexels.slice(0, 4), wideTexels.slice(4, 8),
    [smallTexels[0], smallTexels[0], smallTexels[1], smallTexels[1]], [smallTexels[2], smallTexels[2], smallTexels[3], smallTexels[3]]];
  assert.deepEqual(await rows(), expected([[1, 2, 3, 255], [4, 5, 6, 255], [7, 8, 9, 255], [11, 12, 13, 255]]));
  // Written after the array was bound: the next frame samples the new texels
  const again = [[200, 0, 0, 255], [0, 200, 0, 255], [0, 0, 200, 255], [50, 50, 50, 255]];
  write(small, again);
  assert.deepEqual(await rows(), expected(again));
  graph.destroy();
  wide.destroy();
  small.destroy();
  assert.deepEqual(context.errors, []);
});

// The texels of a 2D rgba8unorm or r32float texture (COPY_SRC), row by row: rgba8unorm's bytes, and
// r32float's red times 255
async function readTexels(device: GPUDevice, t: GPUTexture): Promise<number[][]> {
  const bytesPerRow = 256;
  const buffer = device.createBuffer({ size: bytesPerRow * t.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const encoder = device.createCommandEncoder();
  encoder.copyTextureToBuffer({ texture: t }, { buffer, bytesPerRow }, [t.width, t.height]);
  device.queue.submit([encoder.finish()]);
  await buffer.mapAsync(GPUMapMode.READ);
  const bytes = new Uint8Array(buffer.getMappedRange());
  const floats = new Float32Array(bytes.buffer);
  const texels: number[][] = [];
  for (let y = 0; y < t.height; y++) {
    for (let x = 0; x < t.width; x++) {
      const at = y * bytesPerRow + x * 4;
      texels.push(t.format === 'r32float' ? [Math.round(floats[at / 4] * 255)] : [...bytes.subarray(at, at + 4)]);
    }
  }
  buffer.destroy();
  return texels;
}

test('arrays of storage images store to the images themselves, a bucket each', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const texture = (width: number, height: number, format: GPUTextureFormat) =>
    device.createTexture({ size: [width, height], format, usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.COPY_SRC });
  const wide = texture(4, 2, 'rgba8unorm');
  const tall = texture(2, 3, 'rgba8unorm');
  const red = texture(3, 1, 'r32float');
  const given = [wide, tall, red];
  const ids = await ImportedStores.build(context, graph, { given });
  await graph.execute();
  for (const [e, t] of given.entries()) {
    const expected: number[][] = [];
    for (let y = 0; y < t.height; y++) {
      for (let x = 0; x < t.width; x++) {
        const texel = [e * 50 + 10, x * 40 + 20, y * 60 + 30, 255];
        expected.push(t.format === 'r32float' ? [texel[0]] : texel);
      }
    }
    assert.deepEqual(await readTexels(device, t), expected, `element ${e}`);
  }
  // Each image its own bucket, of its format, bound write-only
  const built = graph.arrayOf(graph.textureArray('again', ids.images.given));
  assert.deepEqual(built.shape, { count: given.length, buckets: given.map((t) => t.format) });
  assert.deepEqual(built.buckets, given);
  const entries = zgal.layoutEntries(device, Stamps, { textures: {}, arrays: { targets: built.shape }, samplers: {} });
  assert.deepEqual(entries.map((e) => [e.binding, e.storageTexture ?? e.buffer]),
    [[0, { type: 'uniform' }], ...given.map((t, i) => [2 + i, { access: 'write-only', format: t.format, viewDimension: '2d-array' }])]);
  graph.destroy();
  for (const t of given) t.destroy();
  assert.deepEqual(context.errors, []);
});

test('arrays of storage images are checked against the storage textures a stage may bind, and read-write formats', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const texture = (width: number) =>
    device.createTexture({ size: [width, 1], format: 'rgba8unorm', usage: GPUTextureUsage.STORAGE_BINDING });
  // Past the storage textures a stage may bind, a bucket per distinct image
  const many = Array.from({ length: device.limits.maxStorageTexturesPerShaderStage + 1 }, (_, i) => texture(i + 1));
  const material = new zgal.MaterialInstance(graph, Stamps, { targets: { array: graph.textureArray('many', graph.importImages('many', many, 'live')) } }, null, null);
  assert.throws(() => material.groupLayout(),
    new RegExp(`^Error: material Stamps's compute stage binds ${many.length} storage images, more than this WebGPU device allows \\(maxStorageTexturesPerShaderStage ${many.length - 1}\\): .*it binds targets \\(${many.length} distinct images\\)`));
  // Image files and models' images are copied into buckets, which shaders can't store to
  const fixed = graph.importImages('fixed', [many[0]], 'fixed');
  assert.throws(() => new zgal.MaterialInstance(graph, Stamps, { targets: { array: graph.textureArray('fixed', fixed) } }, null, null),
    /^Error: material Stamps's binding targets is an array of storage images, but its element fixed\[0\] is an image file's or a model's image/);
  // Read-write needs texture-formats-tier2 for rgba8unorm
  const without = await createNodeContext([assets], { withoutFeatures: ['texture-formats-tier2' as GPUFeatureName] });
  const readWrite = { ...Stamps, bindings: Stamps.bindings.map((b) => ({ ...b, access: 'read-write' as const })) };
  assert.throws(() => zgal.layoutEntries(without.device, readWrite, { textures: {}, arrays: { targets: { count: 1, buckets: ['rgba8unorm'] } }, samplers: {} }),
    /^Error: material Stamps: storage image array targets is read-write, which this WebGPU device only allows for r32float, r32uint and r32sint images \(not rgba8unorm\)/);
  without.destroy();
  graph.destroy();
  for (const t of many) t.destroy();
  assert.deepEqual(context.errors, []);
});

// A buffer's bytes, copied out
async function readBytes(device: GPUDevice, source: GPUBuffer): Promise<Uint8Array> {
  const buffer = device.createBuffer({ size: source.size, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const encoder = device.createCommandEncoder();
  encoder.copyBufferToBuffer(source, 0, buffer, 0, source.size);
  device.queue.submit([encoder.finish()]);
  await buffer.mapAsync(GPUMapMode.READ);
  const bytes = new Uint8Array(buffer.getMappedRange()).slice();
  buffer.destroy();
  return bytes;
}

test('arrays of buffers read and write their elements, a bucket per distinct buffer, uniforms updated each frame', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const storage = (size: number) =>
    device.createBuffer({ size, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
  const second = storage(8);
  device.queue.writeBuffer(second, 0, zgal.encode(Scale, [{ factor: 5, tag: 1000 }]));
  const even = storage(32);
  const odd = storage(32);
  const evenCount = storage(4);
  const oddCount = storage(4);
  let first: Scale = { factor: 3, tag: 7 };
  const ids = await Spreads.build(context, graph, { first: () => first, second, even, odd, evenCount, oddCount });
  // Rows 0, 2 and 3 are elements of halves that are even, row 1 the one that's odd; even rows read
  // the uniform, odd ones second
  const expected = (scale: Scale) => {
    const halves = { even: new Array(16).fill(0), odd: new Array(16).fill(0) };
    for (let y = 0; y < 4; y++) {
      const s = y % 2 === 0 ? scale : { factor: 5, tag: 1000 };
      for (let x = 0; x < 4; x++) halves[y % 3 === 1 ? 'odd' : 'even'][y * 4 + x] = s.factor * 100 + s.tag + x;
    }
    return halves;
  };
  const halves = async (b: GPUBuffer) => [...new Uint16Array((await readBytes(device, b)).buffer)];
  const count = async (b: GPUBuffer) => new Uint32Array((await readBytes(device, b)).buffer)[0];
  await graph.execute();
  assert.deepEqual({ even: await halves(even), odd: await halves(odd) }, expected(first));
  assert.deepEqual([await count(evenCount), await count(oddCount)], [12, 4]);
  first = { factor: 4, tag: 9 };
  await graph.execute();
  assert.deepEqual({ even: await halves(even), odd: await halves(odd) }, expected(first));
  assert.deepEqual([await count(evenCount), await count(oddCount)], [24, 8]);
  // Each distinct buffer its own bucket, the same elements the same array
  const counts = graph.bufferArray('again', [ids.buffers.evenCount, ids.buffers.oddCount, ids.buffers.evenCount]);
  assert.deepEqual(counts.buckets, [ids.buffers.evenCount, ids.buffers.oddCount]);
  assert.deepEqual(counts.shape, { count: 3, buckets: [null, null] });
  const shapes = { scales: { count: 2, buckets: [null, null] }, halves: { count: 3, buckets: [null, null] }, counts: counts.shape };
  const entries = zgal.layoutEntries(device, Spread, { textures: {}, arrays: shapes, samplers: {} });
  // The tables, then (after the push constants' binding) the buckets, read-only where the array is
  assert.deepEqual(entries.map((e) => [e.binding, e.buffer?.type]),
    [[0, 'uniform'], [1, 'uniform'], [2, 'uniform'], [4, 'read-only-storage'], [5, 'read-only-storage'], [6, 'storage'], [7, 'storage'], [8, 'storage'], [9, 'storage']]);
  graph.destroy();
  for (const b of [second, even, odd, evenCount, oddCount]) b.destroy();
  assert.deepEqual(context.errors, []);
});

test('arrays of buffers bind each frame slot\'s copy of per-frame buffers, following the frames in flight', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const rotating = graph.createBuffer('rotating', 'per-frame', zgal.uint, 1);
  const shared = graph.createBuffer('shared', 'shared', zgal.uint, 1);
  const scales = graph.bufferArray('scales', [graph.createBuffer('scale', 'shared', Scale, 1)]);
  const halves = graph.bufferArray('halves', [graph.createBuffer('half', 'shared', zgal.uint16, 16)]);
  // rotating twice: a bucket at its first appearance, which the second shares
  const counts = graph.bufferArray('counts', [rotating, shared, rotating]);
  assert.deepEqual(counts.buckets, [rotating, shared]);
  const material = new zgal.MaterialInstance(graph, Spread, { scales: { buffers: scales }, halves: { buffers: halves }, counts: { buffers: counts } }, null, null);
  assert.equal(material.perFrame, true);
  const binder = material.binder('spread');
  // The buffers each bind group binds, recorded as it's created
  const bound = new Map<GPUBindGroup, GPUBuffer[]>();
  const create = device.createBindGroup.bind(device);
  device.createBindGroup = (d: GPUBindGroupDescriptor) => {
    const group = create(d);
    bound.set(group, [...d.entries].map((e) => (e.resource as GPUBufferBinding).buffer));
    return group;
  };
  // Each slot's group binds the slot's copy of rotating, the shared buffer, and the one table
  const check = () => {
    const groups = Array.from({ length: context.framesInFlight }, () => {
      const slot = context.frameIndex;
      const group = binder.bindGroup();
      assert.equal(binder.bindGroup(), group);
      const buffers = bound.get(group)!;
      assert.ok(buffers.includes(rotating.buffers[slot]));
      for (const [i, copy] of rotating.buffers.entries()) if (i !== slot) assert.ok(!buffers.includes(copy));
      assert.ok(buffers.includes(graph.bufferOf(shared)));
      assert.ok(buffers.includes(counts.table));
      context.advanceFrame();
      return group;
    });
    // (A frame's start gives rotating its copies; here the slot that lacks one does)
    assert.equal(rotating.buffers.length, context.framesInFlight);
    assert.equal(new Set(groups).size, context.framesInFlight);
    return groups;
  };
  try {
    const groups = check();
    // Back at slot 0, its group is the cached one
    assert.equal(binder.bindGroup(), groups[0]);
    // A new count, applied at the frame boundary: rotating gains a copy, and so a slot its group
    const before = context.framesInFlight;
    context.setFramesInFlight(before + 1);
    context.advanceFrame();
    check();
    context.setFramesInFlight(before);
    context.advanceFrame();
  } finally {
    device.createBindGroup = create;
  }
  graph.destroy();
  assert.deepEqual(context.errors, []);
});

test('arrays of buffers are checked against the storage buffers a stage may bind', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const limit = device.limits.maxStorageBuffersPerShaderStage;
  const buffers = Array.from({ length: limit }, (_, i) => graph.createBuffer(`b${i}`, 'shared', zgal.uint, 1));
  const scales = graph.bufferArray('scales', [graph.createBuffer('scale', 'shared', Scale, 1)]);
  const halves = graph.bufferArray('halves', [graph.createBuffer('half', 'shared', zgal.uint16, 16)]);
  // One bucket for scales, one for halves, and limit - 1 distinct buffers for counts: limit + 1
  const material = new zgal.MaterialInstance(graph, Spread, { scales: { buffers: scales }, halves: { buffers: halves }, counts: { buffers: graph.bufferArray('counts', buffers.slice(1)) } }, null, null);
  assert.throws(() => material.groupLayout(),
    new RegExp(`^Error: material Spread's compute stage binds ${limit + 1} storage buffers, more than this WebGPU device allows \\(maxStorageBuffersPerShaderStage ${limit}\\): .*it binds scales \\(1 distinct buffers\\) and halves \\(1 distinct buffers\\) and counts \\(${limit - 1} distinct buffers\\)`));
  // Within it, it binds
  const within = new zgal.MaterialInstance(graph, Spread, { scales: { buffers: scales }, halves: { buffers: halves }, counts: { buffers: graph.bufferArray('fewer', buffers.slice(2)) } }, null, null);
  within.groupLayout();
  // An array binding of buffers is bound to a BufferArray
  assert.throws(() => new zgal.MaterialInstance(graph, Spread, { scales: { buffer: buffers[0] }, halves: { buffers: halves }, counts: { buffers: scales } }, null, null),
    /^Error: material Spread's binding scales is an array, bound to a BufferArray/);
  graph.destroy();
  assert.deepEqual(context.errors, []);
});

test('a buffer or image bound twice in a material, either binding writing it, is refused', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const buffer = (name: string, frames: zgal.Frames = 'shared') => graph.createBuffer(name, frames, zgal.uint, 16);
  const [draws, bounds, kept, keptCount] = ['draws', 'bounds', 'kept', 'keptCount'].map((n) => buffer(n));
  const cull = (bindings: Partial<Record<'draws' | 'bounds' | 'kept' | 'keptCount', zgal.GraphBuffer>>) =>
    new zgal.MaterialInstance(graph, Cull, Object.fromEntries(Object.entries({ draws, bounds, kept, keptCount, ...bindings }).map(([k, b]) => [k, { buffer: b }])), null, null);
  // Two plain bindings: both written, or one read and one written
  assert.throws(() => cull({ keptCount: kept }), /^Error: material Cull binds kept to both kept \(written\) and keptCount \(written\): WebGPU forbids/);
  assert.throws(() => cull({ kept: draws }), /^Error: material Cull binds draws to both draws \(read\) and kept \(written\)/);
  // A per-frame buffer: both bindings bind the frame slot's copy
  const rotating = buffer('rotating', 'per-frame');
  assert.throws(() => cull({ kept: rotating, keptCount: rotating }), /^Error: material Cull binds rotating to both kept \(written\) and keptCount \(written\)/);
  // Two imports of one buffer are one buffer
  const given = device.createBuffer({ size: 64, usage: GPUBufferUsage.STORAGE });
  assert.throws(() => cull({ kept: graph.importBuffer('first', zgal.uint, given), keptCount: graph.importBuffer('second', zgal.uint, given) }),
    /^Error: material Cull binds first and second \(one resource\) to both kept \(written\) and keptCount \(written\)/);
  // An import of a graph buffer's GPU buffer is that buffer, per-frame ones' slot copies too
  assert.throws(() => cull({ keptCount: graph.importBuffer('view', zgal.uint, graph.bufferOf(kept)) }),
    /^Error: material Cull binds kept and view \(one resource\) to both kept \(written\) and keptCount \(written\)/);
  assert.throws(() => cull({ kept: rotating, keptCount: graph.importBuffer('slot', zgal.uint, graph.bufferOf(rotating)) }),
    /^Error: material Cull binds rotating and slot \(one resource\) to both kept \(written\) and keptCount \(written\)/);
  // Read by both: allowed, and WebGPU runs it
  const reading = cull({ bounds: draws });
  graph.add(new zgal.DispatchNode('reading', reading, (_, push) => {
    push.total = 2;
    push.side = 1;
  }, () => [2]));
  await graph.execute();

  // Arrays: the same buffer in two (one written), or an element and another binding. Within one
  // array, a repeated element is one bucket (the tests above), but two imports of one GPU buffer
  // are two buckets, which alias where the array is written
  const [scale, even, odd, evenCount] = ['scale', 'even', 'odd', 'evenCount'].map((n) => buffer(n));
  const spread = (scales: zgal.GraphBuffer[], halves: zgal.GraphBuffer[], counts: zgal.GraphBuffer[], desc: zgal.MaterialDesc = Spread, more: Record<string, zgal.Bound> = {}) =>
    new zgal.MaterialInstance(graph, desc, {
      scales: { buffers: graph.bufferArray('scales', scales) }, halves: { buffers: graph.bufferArray('halves', halves) }, counts: { buffers: graph.bufferArray('counts', counts) }, ...more,
    }, null, null);
  spread([scale], [even, odd, even], [evenCount, evenCount]);
  const [halfA, halfB] = ['halfA', 'halfB'].map((n) => graph.importBuffer(n, zgal.uint, given));
  assert.throws(() => spread([scale], [halfA, halfB], [evenCount]), /^Error: material Spread binds halfA and halfB \(one resource\) as two buffers of halves \(written\): WebGPU forbids/);
  spread([halfA, halfB], [even, odd], [evenCount]);
  assert.throws(() => spread([scale], [even, odd], [evenCount, odd]), /^Error: material Spread binds odd to both an element of halves \(written\) and an element of counts \(written\)/);
  assert.throws(() => spread([scale, even], [even, odd], [evenCount]), /^Error: material Spread binds even to both an element of scales \(read\) and an element of halves \(written\)/);
  const extra: zgal.BindingDesc = { name: 'extra', resource: 'extra', kind: 'buffer-array', access: 'read', stages: ['compute'], depth: false };
  const withExtra = { ...Spread, bindings: [...Spread.bindings, extra] };
  assert.throws(() => spread([scale], [even, odd], [evenCount], withExtra, { extra: { buffer: odd } }),
    /^Error: material Spread binds odd to both an element of halves \(written\) and extra \(read\)/);
  spread([scale], [even, odd], [evenCount], withExtra, { extra: { buffer: scale } });

  // Images: a storage image written, and sampled or stored to by another binding. Read by both
  // (a read-only storage image and a sampled texture) is allowed
  const image = (name: string) => graph.createImage(name, 'shared', 'rgba8unorm', { fixed: [2, 2] });
  const [target, other] = [image('target'), image('other')];
  const stamps = (binding: zgal.BindingDesc, bound: zgal.Bound, desc: zgal.MaterialDesc = Stamps) =>
    new zgal.MaterialInstance(graph, { ...desc, bindings: [...desc.bindings, binding] }, { targets: { array: graph.textureArray('targets', [target, other]) }, more: bound }, null, null);
  const sampled: zgal.BindingDesc = { name: 'more', resource: 'more', kind: 'texture', access: null, stages: ['compute'], depth: false };
  const stored: zgal.BindingDesc = { ...sampled, kind: 'storage-image', access: 'write' };
  assert.throws(() => stamps(sampled, { image: target }), /^Error: material Stamps binds target to both an element of targets \(written\) and more \(read\)/);
  assert.throws(() => stamps(stored, { image: other }), /^Error: material Stamps binds other to both an element of targets \(written\) and more \(written\)/);
  stamps(sampled, { image: target }, { ...Stamps, bindings: Stamps.bindings.map((b) => ({ ...b, access: 'read' as const })) });
  stamps(sampled, { image: image('apart') });
  graph.destroy();
  given.destroy();
  assert.deepEqual(context.errors, []);
});

test('arrays of bindings are checked against the uniform buffers a stage may bind: a table each, and the push constants', async () => {
  const device = context.device;
  const graph = new zgal.RenderGraph(context);
  const limit = device.limits.maxUniformBuffersPerShaderStage;
  // limit arrays' tables and the push constants: arrays of textures as far as the stage may sample
  // textures (a bucket each), then arrays of buffers
  const textures = Math.min(limit, device.limits.maxSampledTexturesPerShaderStage);
  assert.ok(limit - textures <= device.limits.maxStorageBuffersPerShaderStage);
  const array = (i: number, kind: 'texture-array' | 'storage-buffer-array'): zgal.BindingDesc =>
    ({ name: `a${i}`, resource: `a${i}`, kind, access: kind === 'texture-array' ? null : 'read', stages: ['compute'], depth: false });
  const bindings = Array.from({ length: limit }, (_, i) => array(i, i < textures ? 'texture-array' : 'storage-buffer-array'));
  const texture = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING });
  const images = graph.textureArray('images', [graph.importImage('image', texture, 'live')]);
  const buffers = graph.bufferArray('buffers', [graph.createBuffer('buffer', 'shared', zgal.uint, 1)]);
  const material = (count: number) => new zgal.MaterialInstance(graph, { ...Cull, name: 'Tables', bindings: bindings.slice(0, count), push: { ...Cull.push!, binding: count } },
    Object.fromEntries(bindings.slice(0, count).map((b) => [b.resource, b.kind === 'texture-array' ? { array: images } : { buffers }])), null, null);
  assert.throws(() => material(limit).groupLayout(),
    new RegExp(`^Error: material Tables's compute stage binds ${limit + 1} uniform buffers, more than this WebGPU device allows \\(maxUniformBuffersPerShaderStage ${limit}\\): .*, and the push constants a uniform buffer, and it binds ${limit} arrays \\(a0, a1, `));
  // Within it, it binds
  material(limit - 1).groupLayout();
  graph.destroy();
  texture.destroy();
  assert.deepEqual(context.errors, []);
});
