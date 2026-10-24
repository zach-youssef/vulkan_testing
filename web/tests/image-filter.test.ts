// examples/image_filter on devices without the texture-formats-tier2 feature (e.g. SwiftShader and
// lavapipe), which can't bind half-float storage images read-write: every pass, the threshold
// included, gives the same image as on the device with the adapter's features. make test-web
// compiles the example beside the tests and copies its input into tests/assets.
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { fileURLToPath } from 'node:url';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { FilterGraph } from '../image_filter.zgal.ts';

const assets = fileURLToPath(new URL('assets', import.meta.url));
const contexts: [string, zgal.Context][] = [];

before(async () => {
  const full = await createNodeContext([assets]);
  contexts.push([full.hasFeature('texture-formats-tier2') ? 'with texture-formats-tier2' : 'without texture-formats-tier2 (the adapter has none)', full]);
  const without = await createNodeContext([assets], { withoutFeatures: ['texture-formats-tier2' as GPUFeatureName] });
  assert.equal(without.hasFeature('texture-formats-tier2'), false);
  contexts.push(['without texture-formats-tier2', without]);
});

after(() => {
  for (const [, context] of contexts) context.destroy();
});

// The filtered input.png's pixels, with blur, edges and threshold as `enabled` says
async function filter(context: zgal.Context, enabled: { blur: boolean; edges: boolean; threshold: boolean }): Promise<Uint8Array> {
  const graph = new zgal.RenderGraph(context);
  const ids = await FilterGraph.build(context, graph, { radius: 2, level: 0.15, file: 'input.png' });
  graph.setEnabled(ids.nodes.blurRows, enabled.blur);
  graph.setEnabled(ids.nodes.blurColumns, enabled.blur);
  graph.setEnabled(ids.nodes.sobelRows, enabled.edges);
  graph.setEnabled(ids.nodes.sobelColumns, enabled.edges);
  graph.setEnabled(ids.nodes.threshold, enabled.threshold);
  graph.setEnabled(ids.nodes.store, !enabled.threshold);
  await graph.execute();
  const pixels = await ids.readbackPixels(graph);
  graph.destroy();
  return pixels;
}

test('image_filter\'s passes, the threshold included, run without texture-formats-tier2', async () => {
  const outputs: Uint8Array[][] = [];
  for (const [variant, context] of contexts) {
    const copied = await filter(context, { blur: false, edges: false, threshold: false });
    const thresholded = await filter(context, { blur: true, edges: true, threshold: true });
    assert.deepEqual(context.errors, [], variant);
    // Black or white, and opaque (the input is)
    for (let i = 0; i < thresholded.length; i += 4) {
      const [r, g, b, a] = thresholded.subarray(i, i + 4);
      assert.ok((r === 0 || r === 255) && g === r && b === r && a === 255, `${variant}: pixel ${i / 4} is ${[r, g, b, a]}`);
    }
    assert.ok(thresholded.some((c) => c === 0) && thresholded.some((c) => c === 255), `${variant}: all black or all white`);
    outputs.push([copied, thresholded]);
  }
  // Removing a feature changes no pixel
  assert.deepEqual(outputs[1], outputs[0]);
});
