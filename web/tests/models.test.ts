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
import { Corner, CulledModels, ImportedLayers, LayerGraph, Layers, Meshes, Models, ModelVertex } from '../models.zgal.ts';

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
  const shown = graph.createImage('shown', 'rgba8unorm', { surface: 1 });
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
