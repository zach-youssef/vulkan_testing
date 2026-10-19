// How generated graphs run on the web: loops of nodes, conditions, enables, once nodes, buffers
// sized each frame, copies, per-frame data uploads, buffer files, and cpu and extern nodes
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Flow, Bump } from '../web.zgal.ts';

const assets = mkdtempSync(join(tmpdir(), 'zgal-web-'));
let context: zgal.Context;

before(async () => {
  writeFileSync(join(assets, 'saved.bin'), new Uint8Array(new Uint32Array([1, 2, 3]).buffer));
  writeFileSync(join(assets, 'more.bin'), new Uint8Array(new Uint32Array([4]).buffer));
  context = await createNodeContext([assets]);
});

after(() => {
  context.destroy();
  rmSync(assets, { recursive: true });
});

async function flow(extra: boolean, state: { count: number; values: number[]; bumpBy: number }) {
  const graph = new zgal.RenderGraph(context);
  let ticks = 0;
  const externArguments: number[] = [];
  const result = await Flow.build(context, graph, {
    extra,
    steps: [2, 5],
    count: () => state.count,
    bumpBy: () => state.bumpBy,
    values: () => state.values,
    tick: () => {
      ticks++;
    },
    // Adds a node of its own, bumping element 12 by its argument
    more: (g, ids, k) => {
      externArguments.push(k);
      const bump = new zgal.MaterialInstance(g, Bump, { sums: { buffer: ids.buffers.sums } }, (_, push) => {
        push.first = 12;
        push.amount = k;
      }, () => [1]);
      return [g.add(new zgal.DispatchNode('more', bump))];
    },
  });
  return { graph, result, ticks: () => ticks, externArguments };
}

test('loops add a node per element and step, with their own push constants', async () => {
  const { graph, result } = await flow(false, { count: 3, values: [10, 20, 30], bumpBy: 0 });
  assert.equal(result.nodes.bumpEach.length, 4);
  await graph.execute();
  const sums = await result.readSumsContents(graph);
  // Steps 2 and 5, each at s and s + 1, add s + 1; then the once node adds 100 to element 15
  const expected = new Array(16).fill(0);
  for (const s of [2, 5]) for (const k of [0, 1]) expected[s + k] += s + 1;
  expected[15] += 100;
  assert.deepEqual(sums, expected);
  graph.destroy();
});

test('#:when leaves nodes and buffers out; #:enabled and #:once decide which run', async () => {
  const off = await flow(false, { count: 3, values: [1, 2, 3], bumpBy: 0 });
  assert.equal(off.result.nodes.bumpExtra, undefined);
  assert.equal(off.result.buffers.optional, undefined);
  assert.equal(off.graph.isEnabled(off.result.nodes.bumpDisabled), false);
  off.graph.destroy();

  const on = await flow(true, { count: 3, values: [1, 2, 3], bumpBy: 0 });
  assert.notEqual(on.result.nodes.bumpExtra, undefined);
  await on.graph.execute();
  const first = await on.result.readSumsContents(on.graph);
  // bumpDisabled and bumpExtra add 1 (the instance's amount) to 0-15 and 14
  assert.equal(first[0], 1);
  assert.equal(first[14], 2);
  assert.equal(first[15], 101);
  await on.graph.execute();
  // The once node ran in the first frame only
  assert.equal((await on.result.readSumsContents(on.graph))[15], 1);
  on.graph.destroy();
});

test('runOnce runs a node once more, setEnabled cancels it, and followers run with their leader', async () => {
  const { graph, result } = await flow(false, { count: 1, values: [1, 2, 3], bumpBy: 0 });
  const element15 = async () => (await result.readSumsContents(graph))[15];
  await graph.execute();
  assert.equal(await element15(), 100);
  assert.equal(graph.isPendingOnce(result.nodes.bumpOnce), false);
  assert.equal(graph.isEnabled(result.nodes.bumpOnce), false);
  // bumpAll (adding bumpBy, 0) follows bumpOnce: disabled now, it runs once with it
  graph.enableWith(result.nodes.bumpAll, result.nodes.bumpOnce);
  assert.equal(graph.isEnabled(result.nodes.bumpAll), false);
  graph.runOnce(result.nodes.bumpOnce);
  assert.equal(graph.isPendingOnce(result.nodes.bumpAll), true);
  await graph.execute();
  assert.equal(await element15(), 100);
  assert.equal(graph.isEnabled(result.nodes.bumpAll), false);
  graph.runOnce(result.nodes.bumpOnce);
  graph.setEnabled(result.nodes.bumpOnce, false);
  await graph.execute();
  assert.equal(await element15(), 0);
  assert.throws(() => graph.enableWith(result.nodes.bumpOnce, result.nodes.bumpAll));
  graph.destroy();
});

test('per-frame inputs: push constants, buffer sizes and data follow them', async () => {
  const state = { count: 3, values: [10, 20, 30], bumpBy: 2 };
  const { graph, result, ticks, externArguments } = await flow(false, state);
  await graph.execute();
  assert.deepEqual(await result.readSizedContents(graph), [9, 9, 9]);
  const sums = await result.readCopiedContents(graph);
  // Two values from the second, at element 8
  assert.deepEqual(sums.slice(8, 10), [20, 30]);
  assert.equal(sums[0], 2);

  state.count = 5;
  state.values = [7, 8, 9];
  await graph.execute();
  assert.deepEqual(await result.readSizedContents(graph), [9, 9, 9, 9, 9]);
  assert.deepEqual((await result.readCopiedContents(graph)).slice(8, 10), [8, 9]);
  assert.equal(ticks(), 2);
  assert.deepEqual(externArguments, [7]);
  // The extern node's bump of element 12, after readSums but before readLast
  assert.equal((await result.readSumsContents(graph))[12], 2);
  assert.equal((await result.readLastContents(graph))[12], 7 + 2);
  graph.destroy();
});

test('buffer files hold their files\' elements in order', async () => {
  const { graph, result } = await flow(false, { count: 1, values: [1, 2, 3], bumpBy: 0 });
  await graph.execute();
  assert.deepEqual(await result.readSavedContents(graph), [1, 2, 3, 4]);
  graph.destroy();
});

test('WebGPU reports no validation errors', async () => {
  await context.device.queue.onSubmittedWorkDone();
  assert.deepEqual(context.errors, []);
});
