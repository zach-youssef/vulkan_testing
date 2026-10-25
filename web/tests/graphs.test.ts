// web.zgal's materials and graphs on Dawn: the generated WGSL computes what Vulkan's GLSL would, and
// the generated TypeScript builds and runs graphs
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Culling, Shapes, Winding, type Mixed } from '../web.zgal.ts';

let context: zgal.Context;
let graph: zgal.RenderGraph;
let shapes: Shapes;

const seeds = Array.from({ length: 128 }, (_, i) => (i * 37 + 11) % 200);
const uniformValue: Mixed = {
  direction: [0.25, -1, 2],
  intensity: 3,
  flags: 7,
  level: -300,
  bytes: [1, 2, 3],
  offset: -8,
  wide: 0xdeadbeef,
  balance: -12345,
  matrix: [[1, 2], [3, 4]],
};

before(async () => {
  context = await createNodeContext();
  graph = new zgal.RenderGraph(context);
  shapes = await Shapes.build(context, graph, {
    size: 2,
    seeds,
    tint: () => [1, 1, 0.5, 1],
    frameLights: () => uniformValue,
  });
  await graph.execute();
});

after(() => {
  graph.destroy();
  context.destroy();
});

test('push constants (a uniform buffer) and structs of small integers have the std430 layout', async () => {
  const mixed = await shapes.readMixedContents(graph);
  assert.equal(mixed.length, 4);
  for (const [i, m] of mixed.entries()) {
    assert.deepEqual(m, {
      direction: [1, 2, 3],
      intensity: 0.5,
      flags: 3 + i,
      level: -3,
      bytes: [i, 255, (i + 300) & 255],
      // Written alone after the struct: a byte of a word other invocations write
      offset: i - 100,
      wide: 1000003,
      balance: i - 7,
      matrix: [[i, 2], [3, 4]],
    });
  }
});

test('shared memory, barriers and bytes in shared words', async () => {
  const sums = await shapes.readSumsContents(graph);
  assert.deepEqual(sums, [0, 1].map((w) => seeds.slice(w * 64, (w + 1) * 64).reduce((a, b) => a + b, 0)));
  const marks = await shapes.readMarksContents(graph);
  assert.deepEqual(marks, Array.from({ length: 128 }, (_, i) => 64 - (i % 64)));
});

test('atomics, emulated where WGSL lacks them', async () => {
  const counts = await shapes.readCountsContents(graph);
  assert.equal(counts[0], 255);
  assert.equal(counts[1], 0xffffffff);
  // The first invocation to get there exchanged its value
  assert.ok(counts[2] >= 1 && counts[2] <= 256);
  assert.deepEqual(counts.slice(3, 7), [64, 64, 64, 64]);
  assert.deepEqual(await shapes.readBalancesContents(graph), [-255, 155, -512]);
  assert.deepEqual(await shapes.readWeightsContents(graph), [128]);
});

test('subgroup operations, with the scans WGSL lacks built from shuffles', async () => {
  const sizes = await shapes.readSizesContents(graph);
  const size = sizes[0];
  assert.ok(sizes.every((s) => s === size));
  const lanes = await shapes.readLanesContents(graph);
  for (let i = 0; i < 64; i++) {
    const first = Math.floor(i / size) * size;
    const before = Array.from({ length: i - first }, (_, k) => first + k);
    const all = Array.from({ length: size }, (_, k) => first + k);
    assert.deepEqual(lanes[i], [
      Math.max(...[...before, i].map((j) => (j % 5) * 3)),
      before.reduce((x, j) => (x | (1 << (j % 16))) >>> 0, 0),
      before.filter((j) => j % 3 === 0).length,
      all.filter((j) => j % 3 === 0).length + 1000,
    ], `invocation ${i} of subgroups of ${size}`);
  }
});

test('expressions WGSL spells differently: mod, shifts, inverses, vector comparisons, bitcasts, negated uints', async () => {
  assert.deepEqual(await shapes.readSpellContents(graph), [[1.5, 8, 1.25, 4294967296]]);
});

test('draws: a flat varying, a sampled image a copy filled, push constants and a depth test', async () => {
  const pixels = await shapes.readCanvasPixels(graph);
  assert.equal(pixels.length, 8 * 8 * 4);
  for (let p = 0; p < 64; p++) assert.deepEqual([...pixels.subarray(p * 4, p * 4 + 4)], [192, 128, 32, 255], `pixel ${p}`);
});

test('clip space and winding are Vulkan\'s: y down, front faces counter-clockwise on the screen', async () => {
  const winding = new zgal.RenderGraph(context);
  const ids = await Winding.build(context, winding, {});
  await winding.execute();
  const lit = (pixels: Uint8Array) => Array.from({ length: 64 }, (_, p) => pixels[p * 4] > 0);
  // Clockwise: culled as a back face
  assert.deepEqual(lit(await ids.readClockwisePixels(winding)), new Array(64).fill(false));
  // Counter-clockwise: the top left corner (row 0, column 0) is drawn, the bottom right isn't
  const counter = lit(await ids.readCounterPixels(winding));
  assert.equal(counter[0], true);
  assert.equal(counter[63], false);
  assert.equal(counter[7 * 8], false, 'the bottom left corner');
  winding.destroy();
});

test('culling both faces draws no triangles (nor runs their fragments), but still draws points', async () => {
  const culling = new zgal.RenderGraph(context);
  const ids = await Culling.build(context, culling, {});
  await culling.execute();
  const lit = (pixels: Uint8Array) => Array.from({ length: 64 }, (_, p) => pixels[p * 4] > 0);
  // The clear colour survives, and no fragment marked the buffer
  const culled = await ids.readCulledPixels(culling);
  for (let p = 0; p < 64; p++) assert.deepEqual([...culled.subarray(p * 4, p * 4 + 4)], [0, 0, 255, 255], `pixel ${p}`);
  assert.deepEqual(await ids.readCulledMarksContents(culling), [0]);
  // As points, the triangles' corners are drawn: pixels (1, 1), (6, 1), (1, 6) and (6, 6)
  const corners = [1 * 8 + 1, 1 * 8 + 6, 6 * 8 + 1, 6 * 8 + 6];
  assert.deepEqual(lit(await ids.readDottedPixels(culling)), Array.from({ length: 64 }, (_, p) => corners.includes(p)));
  assert.deepEqual(await ids.readDottedMarksContents(culling), [1]);
  await culling.finish();
  culling.destroy();
});

test('uniforms hold their per-frame value', async () => {
  const [copied] = await shapes.readUniformContents(graph);
  assert.deepEqual(copied, uniformValue);
});

test('WebGPU reports no validation errors', async () => {
  await graph.finish();
  assert.deepEqual(context.errors, []);
});
