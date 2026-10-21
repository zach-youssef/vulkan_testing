// The compute library's nodes on the web: scans, reductions and sorts give what tests/gpu/ZgalTest.cpp
// checks natively, with the subgroups feature (when Dawn's adapter has it) and without it (the
// kernels' shared-memory variants, which the runtime picks on devices without the feature)
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { LibraryGraph, SortGraph, floatFromSortKey } from '../kernels.zgal.ts';

const contexts: [string, zgal.Context][] = [];

before(async () => {
  const subgroups = await createNodeContext();
  contexts.push([subgroups.hasFeature('subgroups') ? 'with subgroups' : 'without subgroups (the adapter has none)', subgroups]);
  const without = await createNodeContext([], { withoutFeatures: ['subgroups' as GPUFeatureName] });
  assert.equal(without.hasFeature('subgroups'), false);
  contexts.push(['without subgroups', without]);
});

after(() => {
  for (const [, context] of contexts) context.destroy();
});

const inclusiveScan = (xs: number[]) => xs.map(((sum) => (x: number) => (sum = (sum + x) >>> 0))(0));

test('scans and reductions, counted by the CPU or the GPU, and a node per iteration', async () => {
  for (const [variant, context] of contexts) {
    // Small, one tile, and several tiles per workgroup (with every workgroup of the grid)
    for (const size of [5, 256, 70001]) {
      const at = `${variant}, ${size} elements`;
      const values = Array.from({ length: size }, (_, i) => ((i * 7919) % 100) + 3);
      const floats = Array.from({ length: size }, (_, i) => Math.fround(((i * 31) % 1000) / 8 - 20));
      const gpuCount = size - Math.floor(size / 3);
      const graph = new zgal.RenderGraph(context);
      const library = await LibraryGraph.build(context, graph, { gpuCount, values, floats });
      for (let frame = 0; frame < 2; frame++) await graph.execute();

      const sums = inclusiveScan(values);
      assert.deepEqual(await library.readSumsContents(graph), sums, at);
      assert.deepEqual(await library.readInPlaceContents(graph), sums, at);
      // Exclusive, over the first gpuCount elements (the rest are left as they were: zero)
      const before = [0, ...inclusiveScan(values.slice(0, gpuCount - 1)), ...new Array(size - gpuCount).fill(0)];
      assert.deepEqual(await library.readBeforeContents(graph), before, at);
      // The maximum's identity is the smallest int, not 0
      assert.deepEqual(await library.readMaximaContents(graph), new Array(size).fill(-1), at);

      const results = await library.readResultsContents(graph);
      assert.equal(results[0], sums[size - 1], at);
      assert.equal(results[1], Math.min(...values.slice(0, gpuCount)), at);
      const floatResults = await library.readFloatResultsContents(graph);
      assert.equal(floatResults[0], Math.max(...floats), at);
      const floatSum = floats.reduce((a, b) => a + b, 0);
      assert.ok(Math.abs(floatResults[1] - floatSum) <= 1e-4 * Math.abs(floatSum), `${at}: ${floatResults[1]} vs ${floatSum}`);
      assert.deepEqual(await library.readPrefixSumsContents(graph),
        [1, 2, 3].map((third) => values.slice(0, Math.floor((size * third) / 3)).reduce((a, b) => a + b, 0)), at);
      graph.destroy();
    }
    assert.deepEqual(context.errors, [], variant);
  }
});

test('sorts keys and their values, stably, counted by the CPU or the GPU', async () => {
  for (const [variant, context] of contexts) {
    // One workgroup (up to 4096 keys), and passes
    for (const size of [7, 256, 4096, 4097, 100003]) {
      const at = `${variant}, ${size} keys`;
      const keys: number[] = [];
      const floats: number[] = [];
      let state = 12345;
      for (let i = 0; i < size; i++) {
        state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
        // Few distinct keys in some sizes, to check the sort is stable
        keys.push(size === 256 ? state % 7 : state);
        floats.push(Math.fround((((state >>> 8) % 20001) - 10000) * Math.fround(0.37)));
      }
      const gpuCount = size - Math.floor(size / 4);
      const graph = new zgal.RenderGraph(context);
      const sorting = await SortGraph.build(context, graph, { gpuCount, keys, floats });
      for (let frame = 0; frame < 2; frame++) await graph.execute();

      // Stable: equal keys keep their order, so the values (indices) match a stable sort's
      const order = keys.map((_, i) => i).sort((a, b) => keys[a] - keys[b] || a - b);
      assert.deepEqual(await sorting.readSortedContents(graph), order.map((i) => keys[i]), at);
      assert.deepEqual(await sorting.readOrderContents(graph), order, at);
      // The first gpuCount keys only
      const partial = [...keys.slice(0, gpuCount).sort((a, b) => a - b), ...keys.slice(gpuCount)];
      assert.deepEqual(await sorting.readPartialContents(graph), partial, at);
      // Float keys sort as their floats, and come back from floatFromSortKey
      const floatKeys = await sorting.readFloatKeysContents(graph);
      assert.deepEqual(floatKeys.map(floatFromSortKey), [...floats].sort((a, b) => a - b), at);
      graph.destroy();
    }
    assert.deepEqual(context.errors, [], variant);
  }
});
