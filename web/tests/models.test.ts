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
import { Corner, CulledModels, LayerGraph, Meshes, Models, ModelVertex } from '../models.zgal.ts';

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
  assert.equal(graph.textureOf(ids.images.halfColors).depthOrArrayLayers, 3);
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
  assert.equal(obj.textureOf(objIds.images.halfColors).depthOrArrayLayers, 2);
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
  model.baseColors.destroy();

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

test('arrays of image files are texture arrays, their images resized to the largest', async () => {
  const graph = new zgal.RenderGraph(context);
  const ids = await LayerGraph.build(context, graph, {});
  await graph.execute();
  const pixels = await ids.canvasReadPixels(graph);
  // The top rows are texels.png's; the bottom rows quadrants.png's, widened, so only their ends
  // are the original texels
  const texels = [[[255, 0, 0, 255], [0, 255, 0, 128], [10, 20, 30, 0], [200, 100, 50, 255]],
    [[1, 2, 3, 4], [128, 128, 128, 255], [255, 255, 255, 255], [0, 0, 0, 255]]];
  for (let y = 0; y < 2; y++) {
    for (let x = 0; x < 4; x++) assert.deepEqual(pixel(pixels, x, y), texels[y][x], `(${x}, ${y})`);
  }
  assert.deepEqual([pixel(pixels, 0, 2), pixel(pixels, 3, 2)], [[255, 0, 0, 255], [0, 255, 0, 255]]);
  assert.deepEqual([pixel(pixels, 0, 3), pixel(pixels, 3, 3)], [[0, 0, 255, 255], [128, 128, 128, 255]]);
  graph.destroy();

  // Layers without an image are white
  const device = context.device;
  const texture = (format: GPUTextureFormat) => device.createTexture({ size: [1, 1], format, usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
  const layers = [texture('rgba8unorm'), null];
  const array = zgal.textureArray(device, 'two', 'rgba8unorm', layers);
  assert.equal(array.depthOrArrayLayers, 2);
  array.destroy();
  layers[0]!.destroy();
  assert.deepEqual(context.errors, []);
});
