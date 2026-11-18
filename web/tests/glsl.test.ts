// glsl.zgal on Dawn: materials whose bodies are GLSL, translated by glslc and naga, read and write
// buffers, push constants, storage images, textures through samplers (y as Vulkan's) and depth
// comparisons in the sampler; a sampler clamping to a border, which WebGPU's samplers lack, is
// refused
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Bordered, Compare, Glsl } from '../glsl.zgal.ts';

// The shadow map (8x8) Slope draws: depth rising along x and, twice as fast, y
const depthAt = (i: number, j: number) => (i + 0.5) / 32 + (j + 0.5) / 16;
// Texel centres (uv) and a reference depth on either side of the depth there
const points: [number, number, number][] = [
  [0.0625, 0.0625, 0.04],
  [0.0625, 0.0625, 0.05],
  [0.3125, 0.6875, 0.4],
  [0.3125, 0.6875, 0.45],
  [0.9375, 0.1875, 0.3],
  [0.9375, 0.1875, 0.35],
];
const values = [1, 2, 3, 4, 5, 6];

let context: zgal.Context;
let graph: zgal.RenderGraph;
let glsl: Glsl;

before(async () => {
  context = await createNodeContext();
  graph = new zgal.RenderGraph(context);
  glsl = await Glsl.build(context, graph, { count: values.length, values, points: points.map((p) => [...p, 0]) });
  await graph.execute();
});

after(() => {
  graph.destroy();
  context.destroy();
});

test('a GLSL material is described by its bindings, one each, a sampler it compares with a comparison sampler', () => {
  assert.equal(Compare.glsl, true);
  assert.deepEqual(Compare.bindings.map((b) => [b.name, b.kind, b.depth]), [
    ['shadow', 'texture', true],
    ['shadowSampler', 'comparison-sampler', true],
    ['points', 'buffer-array', false],
    ['compared', 'buffer-array', false],
  ]);
});

test('buffers and push constants reach a GLSL compute shader', async () => {
  assert.deepEqual(await glsl.readScaledContents(graph), values.map((v) => v * 2.5));
});

test('a GLSL shader stores to a storage image, and another samples it with y as Vulkan has it', async () => {
  const texel = (x: number, y: number, green = 1) => [Math.round(x * 0.2 * 255), Math.round(y * 0.4 * 255 * green), 255, 255];
  const rows = (green?: number) => [0, 1].flatMap((y) => [0, 1, 2, 3].flatMap((x) => texel(x, y, green)));
  assert.deepEqual([...(await glsl.readStampsPixels(graph))], rows());
  // Tinted (1, 0.5, 1, 1)
  const canvas = [...(await glsl.readCanvasPixels(graph))];
  const expected = rows(0.5);
  for (const [k, v] of canvas.entries()) assert.ok(Math.abs(v - expected[k]) <= 1, `canvas byte ${k}: ${v}, expected ${expected[k]}`);
});

test('a GLSL sampler2DShadow compares in the sampler with its #:compare (less), against depth a GLSL draw wrote', async () => {
  const compared = await glsl.readComparedContents(graph);
  for (const [k, [u, v, reference]] of points.entries()) {
    const depth = depthAt(Math.floor(u * 8), Math.floor(v * 8));
    assert.equal(compared[k], reference < depth ? 1 : 0, `point ${k}: reference ${reference}, depth ${depth}`);
  }
});

test('a GLSL material refuses a sampler clamping to a border colour', async () => {
  const bordered = new zgal.RenderGraph(context);
  try {
    await assert.rejects(async () => {
      await Bordered.build(context, bordered, {});
      await bordered.execute();
    }, /clamps to a border colour: WebGPU's samplers have none/);
  } finally {
    bordered.destroy();
  }
});
