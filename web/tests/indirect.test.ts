// Indirect dispatches and draws on the web: what tests/gpu/ZgalTest.cpp checks natively, with the
// commands written by shaders, imported or uploaded, and draws counted by a buffer (which WebGPU
// draws from the commands a limiting dispatch copied)
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { GivenCommands, IndirectGraph, LoopDraws, OverrideGraph } from '../indirect.zgal.ts';

let context: zgal.Context;

before(async () => {
  context = await createNodeContext();
});

after(() => {
  context.destroy();
});

const pixel = (pixels: Uint8Array, x: number, y: number) => [...pixels.subarray((y * 4 + x) * 4, (y * 4 + x) * 4 + 4)];
const draw = { vertexCount: 3, instanceCount: 1, firstVertex: 0, firstInstance: 0 };

test('graphs dispatch and draw what shaders wrote, as many draws as a buffer says', async () => {
  // The second draw's first instance (1) draws green; none draws nothing, leaving the clear blue
  for (const [drawn, color] of [[1, [255, 0, 0, 255]], [2, [0, 255, 0, 255]], [0, [0, 0, 255, 255]]] as const) {
    const graph = new zgal.RenderGraph(context);
    const ids = await IndirectGraph.build(context, graph, { groups: 5, drawn });
    for (let frame = 0; frame < 3; frame++) await graph.execute();
    // Workgroups 0 to 4
    assert.deepEqual(await ids.readTallyContents(graph), [10], `${drawn} drawn`);
    assert.deepEqual(pixel(await ids.readPicturePixels(graph), 0, 0), color, `${drawn} drawn`);
    // The limiting dispatch is disabled with its draw
    graph.setEnabled(ids.nodes.paint, false);
    await graph.execute();
    graph.destroy();
  }
  assert.deepEqual(context.errors, []);
});

test('dispatches read commands from imported and uploaded buffers', async () => {
  const commands = (usage: number) => {
    const buffer = context.device.createBuffer({ size: 12, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | usage });
    context.device.queue.writeBuffer(buffer, 0, new Uint32Array([4, 1, 1]));
    return buffer;
  };
  const given = commands(GPUBufferUsage.INDIRECT);
  const graph = new zgal.RenderGraph(context);
  const ids = await GivenCommands.build(context, graph, { given, uploads: [{ x: 9, y: 9, z: 9 }, { x: 3, y: 1, z: 1 }] });
  await graph.execute();
  // Workgroups 0 to 3, then 0 to 2
  assert.deepEqual(await ids.readTallyContents(graph), [6 + 3]);
  graph.destroy();
  given.destroy();

  // The graph checks an imported buffer was created for indirect use
  const plain = commands(0);
  const unusable = new zgal.RenderGraph(context);
  await GivenCommands.build(context, unusable, { given: plain, uploads: [{ x: 1, y: 1, z: 1 }, { x: 1, y: 1, z: 1 }] });
  await assert.rejects(unusable.execute(), /fromImport: buffer given wasn't created with GPUBufferUsage.INDIRECT/);
  unusable.destroy();
  plain.destroy();
  assert.deepEqual(context.errors, []);
});

test('indirect nodes give their own push constants, sharing a material', async () => {
  const graph = new zgal.RenderGraph(context);
  // Two commands, of which the counted draw draws as many as its count says
  const ids = await OverrideGraph.build(context, graph, { drawCommands: [draw, draw] });
  await graph.execute();
  const expected = new Array(64).fill(0);
  expected.fill(5, 0, 10);
  expected.fill(7, 30, 34);
  expected.fill(3, 60, 64);
  for (const increment of [1, 2]) for (let i = 0; i < 64; i++) expected[i] = expected[i] * 2 + increment;
  assert.deepEqual(await ids.readCellsContents(graph), expected);
  // A row each, in red, green, yellow and blue: the draws of commands pushed their own rows and
  // shades too
  const rows = [[255, 0, 0, 255], [0, 255, 0, 255], [255, 255, 0, 255], [0, 0, 255, 255]];
  const pixels = await ids.readPicturePixels(graph);
  for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) assert.deepEqual(pixel(pixels, x, y), rows[y], `(${x}, ${y})`);
  graph.destroy();
  assert.deepEqual(context.errors, []);
});

test('counted draws in loops use each iteration\'s command and count', async () => {
  const graph = new zgal.RenderGraph(context);
  const ids = await LoopDraws.build(context, graph, { commands: [draw, draw, draw, draw], counts: [0, 0, 1, 1] });
  await graph.execute();
  // A band covers its row and those below it: the first two rows' counts are 0, so only the
  // third's band, from its own row down, was drawn (and the fourth's over it)
  const pixels = await ids.readPicturePixels(graph);
  for (let y = 0; y < 4; y++) {
    const shade = y < 2 ? 0 : 255;
    assert.deepEqual(pixel(pixels, 0, y), [shade, shade, shade, 255], `row ${y}`);
  }
  graph.destroy();
  assert.deepEqual(context.errors, []);
});
