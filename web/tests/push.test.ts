// push.zgal on Dawn: struct and array push constants reach the shaders with every value in place,
// though the uniform buffer holding them is laid out differently from the shape the body reads
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Arrays, Pushes, type Packed } from '../push.zgal.ts';

let context: zgal.Context;

before(async () => {
  context = await createNodeContext();
});

after(() => {
  context.destroy();
});

function packed(base: number): Packed {
  return {
    a: [base + 1, base + 2],
    vals: [base + 3, base + 4, base + 5],
    inner: { w: base + 6, k: base + 7 },
    tail: base + 8,
    m: [[base + 9, base + 10], [base + 11, base + 12]],
    cols: [[base + 13, base + 14], [base + 15, base + 16]],
    inners: [{ w: base + 17, k: base + 18 }, { w: base + 19, k: base + 20 }],
    last: base + 21,
  };
}

async function pushes(state: { packed: Packed; rounds: number }) {
  const graph = new zgal.RenderGraph(context);
  const result = await Pushes.build(context, graph, {
    packed: () => state.packed,
    rounds: () => state.rounds,
    // Its own instance of Arrays, whose push constants are arrays
    more: (g, ids, count) => {
      const arrays = new zgal.MaterialInstance(g, Arrays, { values: { buffer: ids.buffers.values } }, (_, push) => {
        push.scales = [1.5, 2.5, 3.5];
        push.points = [[4, 5], [6, 7], [8, 9]];
        push.items = [{ w: 10, k: 11 }, { w: 12, k: 13 }, { w: 14, k: 15 }];
        push.count = count + 1;
      }, () => [1]);
      return [g.add(new zgal.DispatchNode('arrays', arrays))];
    },
  });
  return { graph, result };
}

test('a struct push constant reaches the shader whole, and follows its per-frame input', async () => {
  const state = { packed: packed(0), rounds: 3 };
  const { graph, result } = await pushes(state);
  await graph.execute();
  assert.deepEqual(await result.readCopiesContents(graph), [packed(0)]);
  // The array's elements (read at a loop's index) and lead, summed and doubled each round; a
  // barrier runs in each round (under the push constants' rounds)
  assert.deepEqual(await result.readSumsContents(graph), [22, 44, 88, 0]);

  state.packed = packed(100);
  state.rounds = 4;
  await graph.execute();
  assert.deepEqual(await result.readCopiesContents(graph), [packed(100)]);
  assert.deepEqual(await result.readSumsContents(graph), [322, 644, 1288, 2576]);
  graph.destroy();
});

test('array push constants (of floats, 2-component vectors and structs) reach the shader', async () => {
  const { graph, result } = await pushes({ packed: packed(0), rounds: 0 });
  await graph.execute();
  assert.deepEqual(await result.readValuesContents(graph), [1.5, 2.5, 3.5, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
  graph.destroy();
});
