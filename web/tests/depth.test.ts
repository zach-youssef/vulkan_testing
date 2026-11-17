// web.zgal's Shadows on Dawn: a depth image both sampled and compared, with one ZGAL sampler doing
// both, bound as the variants WebGPU needs (a non-filtering sampler for the depth texture, a
// comparison sampler), directly and through functions; a linear sampler filters the depth image
// by hand (WebGPU samples depth textures only with non-filtering samplers) as Vulkan's would, and
// samplers clamping to a border sample and compare past the edges by hand. All in
// compute shaders: the fragment stage's path (textureSample, comparisons in a function under the
// uniformity analysis) is only compiled, by the golden Lit material and Dawn under make test-dsl
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
// The linear sampler repeats: the texel block around uv, each texel's value (or its comparison
// with the reference) weighted by uv's position in the block
const repeat = (x: number) => ((x % size) + size) % size;
function bilinear(u: number, v: number, texel: (i: number, j: number) => number, n = size, wrap = repeat): number {
  const x = u * n - 0.5;
  const y = v * n - 0.5;
  const i = Math.floor(x);
  const j = Math.floor(y);
  const fx = x - i;
  const fy = y - j;
  const row = (jj: number) => texel(wrap(i), wrap(jj)) * (1 - fx) + texel(wrap(i + 1), wrap(jj)) * fx;
  return row(j) * (1 - fy) + row(j + 1) * fy;
}
// A sampler clamping to a border: texels outside an n-texel square are the border colour's red
const bordered = (n: number, texel: (i: number, j: number) => number, border: number) => (i: number, j: number) =>
  i < 0 || j < 0 || i >= n || j >= n ? border : texel(i, j);
const unwrapped = (x: number) => x;
const linear = (u: number, v: number) => bilinear(u, v, depthAt);
const compared = (u: number, v: number, reference: number) => bilinear(u, v, (i, j) => (reference < depthAt(i, j) ? 1 : 0));
// The picture's constant red (0xFF4080C0 texels)
const red = 0xc0 / 255;

// A sampler's samples and reds against what it should give: each sample's depth and comparison
// (directly and through lit), and the reds of the depth image and of the picture
function checkSamples(samples: number[][], reds: number[][], expect: (u: number, v: number, reference: number) => [number, number, number], tolerance: { depth: number; compared: number }) {
  for (const [k, [u, v, reference]] of points.entries()) {
    const [depth, test, pictureRed] = expect(u, v, reference);
    const [sampled, comparedTo, viaFunction, comparedViaFunction] = samples[k];
    assert.ok(Math.abs(sampled - depth) < tolerance.depth, `point ${k}: sampled ${sampled}, expected ${depth}`);
    assert.ok(Math.abs(comparedTo - test) < tolerance.compared, `point ${k}: compared ${comparedTo}, expected ${test}`);
    assert.ok(Math.abs(viaFunction - depth) < tolerance.depth, `point ${k}: sampled through lit ${viaFunction}, expected ${depth}`);
    assert.ok(Math.abs(comparedViaFunction - test) < tolerance.compared, `point ${k}: compared through lit ${comparedViaFunction}, expected ${test}`);
    const [ofDepth, ofPicture] = reds[k];
    assert.ok(Math.abs(ofDepth - depth) < tolerance.depth, `point ${k}: red of the depth image ${ofDepth}, expected ${depth}`);
    assert.ok(Math.abs(ofPicture - pictureRed) < tolerance.depth, `point ${k}: red of the picture ${ofPicture}, expected ${pictureRed}`);
  }
}

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
  [0.4, 0.3, 0.2],
  [0.4, 0.3, 0.26],
  [0.96, 0.98, 0.5],
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

test('a nearest sampler with a linear mipmap filter samples nearest texels: one level has no mipmaps to filter', async () => {
  const samples = await shadows.readMipSamplesContents(graph);
  for (const [k, [u, v, reference]] of points.entries()) {
    const [sampled, comparedTo] = samples[k];
    const depth = nearest(u, v);
    assert.ok(Math.abs(sampled - depth) < 1e-6, `point ${k}: sampled ${sampled}, expected ${depth}`);
    assert.equal(comparedTo, reference < depth ? 1 : 0, `point ${k}: compared`);
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

test('a linear sampler filters the depth image bilinearly, wrapping as it says, and its comparisons', async () => {
  const samples = await shadows.readLinearSamplesContents(graph);
  const reds = await shadows.readLinearRedsContents(graph);
  for (const [k, [u, v, reference]] of points.entries()) {
    const [sampled, comparedTo, viaFunction, comparedViaFunction] = samples[k];
    const depth = linear(u, v);
    const test = compared(u, v, reference);
    // The filter by hand is exact; the comparisons are filtered by the device, with its weights
    assert.ok(Math.abs(sampled - depth) < 1e-5, `point ${k}: sampled ${sampled}, expected ${depth}`);
    assert.ok(Math.abs(comparedTo - test) < 1 / 128, `point ${k}: compared ${comparedTo}, expected ${test}`);
    assert.ok(Math.abs(viaFunction - depth) < 1e-5, `point ${k}: sampled through lit ${viaFunction}, expected ${depth}`);
    assert.ok(Math.abs(comparedViaFunction - test) < 1 / 128, `point ${k}: compared through lit ${comparedViaFunction}, expected ${test}`);
    const [ofDepth, ofPicture] = reds[k];
    assert.ok(Math.abs(ofDepth - depth) < 1e-5, `point ${k}: red of the depth image ${ofDepth}, expected ${depth}`);
    assert.ok(Math.abs(ofPicture - red) < 1e-6, `point ${k}: red of the picture ${ofPicture}, expected ${red}`);
  }
});

test('a nearest sampler clamping to the default border (opaque black) gives depth 0 past the edges, and compares with it', async () => {
  // Shadows' nearestBorder: #:compare greater
  const texel = bordered(size, depthAt, 0);
  checkSamples(await shadows.readNearestBorderSamplesContents(graph), await shadows.readNearestBorderRedsContents(graph), (u, v, reference) => {
    const depth = texel(Math.floor(u * size), Math.floor(v * size));
    const pictureRed = bordered(2, () => red, 0)(Math.floor(u * 2), Math.floor(v * 2));
    return [depth, reference > depth ? 1 : 0, pictureRed];
  }, { depth: 1e-6, compared: 1e-6 });
  // The point beyond the edges is the border's
  const samples = await shadows.readNearestBorderSamplesContents(graph);
  assert.deepEqual([samples[4][0], samples[4][1]], [0, 1]);
});

test('a linear sampler clamping to a white border filters it in with the texels by the edges, and compares each first', async () => {
  // Shadows' linearBorder: #:border opaque-white #:compare less-or-equal; all by hand, so exact
  const texel = bordered(size, depthAt, 1);
  checkSamples(await shadows.readLinearBorderSamplesContents(graph), await shadows.readLinearBorderRedsContents(graph), (u, v, reference) => [
    bilinear(u, v, texel, size, unwrapped),
    bilinear(u, v, (i, j) => (reference <= texel(i, j) ? 1 : 0), size, unwrapped),
    bilinear(u, v, bordered(2, () => red, 1), 2, unwrapped),
  ], { depth: 1e-5, compared: 1e-5 });
  const samples = await shadows.readLinearBorderSamplesContents(graph);
  assert.ok(samples[9][0] > linear(0.96, 0.98), 'the white border raises the depth by the far corner');
});
