// extern_glsl.zgal on Dawn: ZGAL bodies calling functions written in GLSL (extern-glsl), whose
// materials compile through GLSL (glslc and naga) as a whole: the reference's noise example, an
// integer function checked exactly with an integer atomic, and a GLSL vertex body sharing a
// material with a ZGAL fragment body
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { ExternGlsl, Grain, Tint, Weave } from '../extern_glsl.zgal.ts';

// shaders/extern_glsl_weave.glsl's weave
const weave = (a: number, b: number) => (Math.imul(a, 2654435761) ^ (b << 3) ^ (a >>> 2)) >>> 0;
const count = 37;
const salt = 7;

let context: zgal.Context;
let graph: zgal.RenderGraph;
let program: ExternGlsl;

before(async () => {
  context = await createNodeContext();
  graph = new zgal.RenderGraph(context);
  program = await ExternGlsl.build(context, graph, { count, zero: [0] });
  await graph.execute();
});

after(() => {
  graph.destroy();
  context.destroy();
});

test('materials calling GLSL functions, or with GLSL bodies, compile through GLSL', () => {
  assert.equal(Grain.glsl, true);
  assert.equal(Weave.glsl, true);
  assert.equal(Tint.glsl, true);
});

test("the reference's grain, half of noise written in GLSL, is stored for each texel", async () => {
  const pixels = [...(await program.readGrainPixels(graph))];
  assert.equal(pixels.length, 8 * 8 * 4);
  const grains = new Set<number>();
  for (let k = 0; k < pixels.length; k += 4) {
    const [r, g, b, a] = pixels.slice(k, k + 4);
    assert.ok(g === r && b === r && a === r, `texel ${k / 4}: ${[r, g, b, a]}, expected one grain in all four`);
    // At most half of 1 (noise is a fraction), allowing for rounding
    assert.ok(r <= 128, `texel ${k / 4}: ${r}`);
    grains.add(r);
  }
  // noise(0, 0) is 0; elsewhere it varies
  assert.equal(pixels[0], 0);
  assert.ok(grains.size > 16, `only ${grains.size} different grains`);
});

test('an integer GLSL function writes its results exactly, and an atomic adds them up', async () => {
  const expected = Array.from({ length: count }, (_, i) => weave(i, salt));
  assert.deepEqual(await program.readWovenContents(graph), expected);
  assert.deepEqual(await program.readTotalContents(graph), [expected.reduce((sum, w) => (sum + w) >>> 0, 0)]);
});

test("a ZGAL fragment body draws with its GLSL vertex body's triangle, reading the material's push constants", async () => {
  const pixels = [...(await program.readCanvasPixels(graph))];
  const tint = [0.2, 0.4, 0.6, 1].map((c) => Math.round(c * 255));
  for (const [k, v] of pixels.entries()) assert.ok(Math.abs(v - tint[k % 4]) <= 1, `canvas byte ${k}: ${v}, expected ${tint[k % 4]}`);
});
