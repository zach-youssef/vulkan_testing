// web.zgal's Shadows on Dawn: a depth image both sampled and compared, with one ZGAL sampler doing
// both, bound as the variants WebGPU needs (a non-filtering sampler for the depth texture, a
// comparison sampler), directly and through functions. All in compute shaders: the fragment stage's
// path (textureSample, comparisons in a function under the uniformity analysis) is only compiled,
// by the golden Lit material and Dawn under make test-dsl
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { PeekDepth, Shade, Shadows, Uncompared } from '../web.zgal.ts';

// The shadow map (8x8) Slope draws: depth rising along x and, twice as fast, y
const size = 8;
const depthAt = (i: number, j: number) => (i + 0.5) / 32 + (j + 0.5) / 16;
const clamp = (x: number) => Math.min(Math.max(x, 0), size - 1);
const nearest = (u: number, v: number) => depthAt(clamp(Math.floor(u * size)), clamp(Math.floor(v * size)));
// The picture's constant red (0xFF4080C0 texels)
const red = 0xc0 / 255;

// uv and a reference depth: texel centres and not, beyond the edges (clamped) and references on
// either side of the depth there
const points: [number, number, number][] = [
  [0.3, 0.7, 0.4],
  [0.3, 0.7, 0.5],
  [0.0625, 0.0625, 0.03],
  [0.0625, 0.0625, 0.05],
  [1.2, -0.5, 0.3],
  [0.95, 0.95, 0.7],
  [0.5, 0.5, 0.33],
];

let context: zgal.Context;
let graph: zgal.RenderGraph;
let shadows: Shadows;

before(async () => {
  context = await createNodeContext();
  graph = new zgal.RenderGraph(context);
  shadows = await Shadows.build(context, graph, { count: points.length, points: points.map((p) => [...p, 0]) });
  await graph.execute();
});

after(() => {
  graph.destroy();
  context.destroy();
});

test('a sampler used to sample and to compare is described as its variants, of one resource', () => {
  const variants = Shade.bindings.filter((b) => b.resource === 'shadowSampler');
  assert.deepEqual(variants.map((b) => [b.name, b.kind, b.depth]), [
    ['shadowSampler', 'sampler', false],
    ['zgalDepth_shadowSampler', 'depth-sampler', false],
    ['zgalCompare_shadowSampler', 'comparison-sampler', true],
  ]);
  assert.deepEqual(Shade.bindings.map((b) => b.name).indexOf('picture'), 4, 'the variants take binding indices');
});

test('sampling a depth image gives its depth, and comparing with it (less) its test, through one sampler', async () => {
  const samples = await shadows.readSamplesContents(graph);
  const expected = points.map(([u, v, reference]) => {
    const depth = nearest(u, v);
    return [depth, reference < depth ? 1 : 0];
  });
  for (const [k, [depth, compared]] of expected.entries()) {
    const [sampled, comparedTo, viaFunction, comparedViaFunction] = samples[k];
    assert.ok(Math.abs(sampled - depth) < 1e-6, `point ${k}: sampled ${sampled}, expected ${depth}`);
    assert.equal(comparedTo, compared, `point ${k}: compared`);
    assert.ok(Math.abs(viaFunction - depth) < 1e-6, `point ${k}: sampled through lit ${viaFunction}, expected ${depth}`);
    assert.equal(comparedViaFunction, compared, `point ${k}: compared through lit`);
  }
});

test('a function only sampling its texture takes the depth image and a colour image in turn', async () => {
  const reds = await shadows.readRedsContents(graph);
  for (const [k, [u, v]] of points.entries()) {
    const [ofDepth, ofPicture] = reds[k];
    assert.ok(Math.abs(ofDepth - nearest(u, v)) < 1e-6, `point ${k}: red of the depth image ${ofDepth}, expected ${nearest(u, v)}`);
    assert.ok(Math.abs(ofPicture - red) < 1e-6, `point ${k}: red of the picture ${ofPicture}, expected ${red}`);
  }
});

test('comparing with a sampler without #:compare is refused', async () => {
  const other = new zgal.RenderGraph(context);
  try {
    await Uncompared.build(context, other, { count: 1, points: [[0.5, 0.5, 0.5, 0]] });
    await assert.rejects(other.execute(), /material Shade compares depths with sampler plain, which has no #:compare/);
  } finally {
    other.destroy();
  }
});

test('sampling a depth image without comparing with it needs a nearest sampler', async () => {
  const other = new zgal.RenderGraph(context);
  try {
    await PeekDepth.build(context, other, {});
    await assert.rejects(other.execute(), /material Peek samples depth image shadowMap without comparing with it, which the web does only with a nearest sampler, and sampler linear filters: give it nearest filters \(#:filter and #:mipmap\)/);
  } finally {
    other.destroy();
  }
});

